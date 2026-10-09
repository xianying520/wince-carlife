/* adbproxy.c — 见 adbproxy.h。
 *
 * 架构上只有一个线程碰 ADB 设备。为什么必须这样：
 * 设备是一个独占的字节流，而 ADB 是严格应答式的（发完 WRTE 必须等 OKAY，
 * 等 OKAY 的过程中又会读到别的通道推来的数据）。如果多个线程各读各的，
 * 就会互相把对方的包吃掉 —— 这种竞态极难查。所以干脆让一个线程全权负责
 * 设备读写，其余部分都用非阻塞 socket，同一个循环里驱动两边。
 */
#include "adbproxy.h"

/* 日志：只在真正上车机编的时候启用。
 * 主机测试（-DADBP_HOST_TEST）里没有 cllog.c，所以那边宏展开成空。 */
/* 屏幕回调（由 viewer 装上来）。没装就是空指针，什么也不做。
 * ⚠ 刻意放在 #ifndef ADBP_HOST_TEST 外面：主机测试也用同一份头文件，
 *   只在车机分支里定义会让主机那边的链接对不上。 */
static void (*g_trace_cb)(const char *what, unsigned int a, unsigned int b);

void adbp_set_trace_cb(void (*cb)(const char *what, unsigned int a, unsigned int b))
{
    g_trace_cb = cb;
}

#ifndef ADBP_HOST_TEST
#include "cllog.h"
#define ADBP_LOG(...) cl_log(__VA_ARGS__)

/* 把 ADB 握手的每一步接到日志上 —— 并且【同时接到屏幕上】。
 * 车机上只有一次机会，而「发到哪一步、手机回没回」是唯一能定位的手段；
 * 只写日志的话，用户在当时是看不见的（这一条是现场反馈回来的真问题）。 */
static void adb_trace_log(const char *what, unsigned int a, unsigned int b)
{
    cl_log("   [握手] %s  (0x%x, 0x%x)", what, a, b);
    if (g_trace_cb)
        g_trace_cb(what, a, b);
}
#else
#define ADBP_LOG(...) ((void)0)
#endif
#include "adb.h"

/* ── 平台层 ──
 * 车机上用 Windows CE 的线程与设备 API；电脑上换成等价垫片。
 * 业务逻辑（下面那个 select 循环）两边完全一样，不做条件编译，
 * 这样主机上跑通就等于车机上跑通。 */
#ifdef ADBP_HOST_TEST
#include "clhost.h"
#include "hostplat.h"
#define ADBP_SLEEP(ms)   usleep((unsigned)(ms) * 1000)
#define ADBP_THREAD_RET  void *
#define ADBP_THREAD_ARG  void *
#define ADBP_API
#else
#include <windows.h>
#include "adbio_ce.h"
#define ADBP_SLEEP(ms)   Sleep(ms)
#define ADBP_THREAD_RET  DWORD
#define ADBP_THREAD_ARG  LPVOID
#define ADBP_API         WINAPI
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define ADBP_MAX_SVC   8
#define ADBP_MAX_CONN  12
#define ADBP_BUF       16384

typedef struct {
    int            used;
    char           service[64];
    SOCKET         lsn;
    unsigned short lport;
} ADBP_SVC;

typedef struct {
    int    used;
    SOCKET sock;
    int    chan;            /* ADB 通道号 */
    int    svc;
} ADBP_CONN;

static ADBP_SVC  g_svc[ADBP_MAX_SVC];
static int       g_nsvc;
static ADBP_CONN g_conn[ADBP_MAX_CONN];
static ADB       g_adb;
#ifdef ADBP_HOST_TEST
static HANDLE    g_thread;
#else
static HANDLE    g_thread;
static DWORD     g_tid;
#endif
static int       g_own_device;        /* 设备是不是本模块打开的（决定要不要关） */

/* 关设备：主机测试下没有设备可关，包一层以免引用到 WinCE 的符号。
 * （原本写成宏，结果后来一次全局替换把宏体里那行也一并换掉了，
 *   宏于是变成自己调自己，链接时报 undefined reference。
 *   改成普通函数就不会被这种替换误伤。） */
#ifdef ADBP_HOST_TEST
static void adbp_close_device(void)
{
    g_own_device = 0;
}
#else
static void adbp_close_device(void)
{
    if (g_own_device) {
        adbio_ce_close();
        g_own_device = 0;
    }
}
#endif
static volatile int g_stop;
static volatile int g_running;
static char      g_status[256];
static unsigned long g_tx_bytes, g_rx_bytes;
static char      g_pkgs[2048];        /* 手机包名清单（截断保留） */
static char      g_note[256];         /* 最近一次拉起的说明（现场排查用） */
static int       g_tried_launch;      /* 只尝试拉起一次，避免反复折腾 */
static int       g_fail_streak;       /* 连续握手失败次数（决定什么时候才真的重开设备） */

/* ══ ADB 访问锁 ══════════════════════════════════════════════════════════════
 * 为什么必须有：ADB 对象 g_adb 是【共享】的，而它会同时被两个线程碰 ——
 *   · 转发器线程（adbp_thread）：adb_pump / adb_recv 搬数据
 *   · 会话线程（viewer.c 拉起手机端）：adb_run_shell → adb_open / adb_pump
 * 两个线程同时改 a->ch[] 通道表，会把通道分配搞坏（本地 id 是按槽位下标算的），
 * 表现是「命令发出去了但回显错乱」「通道莫名关掉」，甚至读到半个消息 ——
 * 在车机上就是「手机端启动了，但怎么都连不上」，而且极难查。
 *
 * 锁范围刻意收窄：【只圈 ADB 调用】，不圈 select / accept / 本地 send。
 * 否则拉起手机端那几十秒会把整个转发卡死。幸好 CRITICAL_SECTION 可重入，
 * open_with_retry → adbp_launch_phone_app 这种同线程嵌套不会死锁。 */
#ifdef ADBP_HOST_TEST
#define ADBP_LOCK()   ((void)0)
#define ADBP_UNLOCK() ((void)0)
static void adbp_lock_init(void) { }
#else
#include <windows.h>          /* CRITICAL_SECTION / InitializeCriticalSection */
static CRITICAL_SECTION g_adb_lock;
static int              g_adb_lock_ready = 0;
static void adbp_lock_init(void)
{
    if (!g_adb_lock_ready) {
        InitializeCriticalSection(&g_adb_lock);
        g_adb_lock_ready = 1;
    }
}
#define ADBP_LOCK()   EnterCriticalSection(&g_adb_lock)
#define ADBP_UNLOCK() LeaveCriticalSection(&g_adb_lock)
#endif

/* 手写的最小格式化：只覆盖本项目用到的 %s %d %u %x %%。
 *
 * ⚠⚠ 为什么坚决不用 _vsnprintf：
 *
 *   在整份工程里，`_vsnprintf` 只被【这一个函数】引用 ——
 *   车机上跑 `arm-mingw32ce-nm` 数导入表就能看出来。
 *   而现场日志的表现是：每次都在【刚握完手】那一行断掉，后面一个字都没有。
 *
 *   紧接着握手成功之后要跑的代码里，第一个会被调用的、
 *   而且以前从来没在你这台车机上跑过的东西，就是这个 _vsnprintf。
 *   （握手之前的所有路径，走的都是 snprintf / sprintf / cl_log 的手写格式化，
 *    那些都已经在车上跑过成千上万次了。）
 *
 *   cllog.c 早就因为同一家族的函数在车上崩过 —— 当时把 _vsnprintf 换成
 *   手写格式化，日志立刻就正常了。既然有前科，这里没有任何理由再赌一次。
 *   手写 40 行，行为完全确定，还能顺手把线程安全问题一起解掉。 */
static void status_vfmt(char *out, int cap, const char *fmt, va_list ap)
{
    int o = 0;

    for (; *fmt && o < cap - 1; fmt++) {
        if (*fmt != '%') { out[o++] = *fmt; continue; }
        fmt++;

        if (*fmt == 's') {
            const char *v = va_arg(ap, const char *);
            if (!v) v = "(null)";
            while (*v && o < cap - 1) out[o++] = *v++;
        } else if (*fmt == 'd' || *fmt == 'u' || *fmt == 'x') {
            char t[16];
            int  n = 0, k;
            unsigned int u;
            const char  *D;
            unsigned int base;

            if (*fmt == 'd') {
                int sv = va_arg(ap, int);
                if (sv < 0) {
                    if (o < cap - 1) out[o++] = '-';
                    u = (unsigned int)(0u - (unsigned int)sv);
                } else {
                    u = (unsigned int)sv;
                }
                D = "0123456789";
                base = 10;
            } else if (*fmt == 'u') {
                u = va_arg(ap, unsigned int);
                D = "0123456789";
                base = 10;
            } else {
                u = va_arg(ap, unsigned int);
                D = "0123456789abcdef";
                base = 16;
            }
            do { t[n++] = D[u % base]; u /= base; } while (u && n < 15);
            for (k = n - 1; k >= 0 && o < cap - 1; k--) out[o++] = t[k];
        } else if (*fmt == '%') {
            if (o < cap - 1) out[o++] = '%';
        } else if (*fmt == 0) {
            break;
        } else {
            if (o < cap - 1) out[o++] = '?';
        }
    }
    out[o] = 0;
}

static void set_status(const char *fmt, ...)
{
    char    tmp[256];
    va_list ap;
    int     i;

    va_start(ap, fmt);
    status_vfmt(tmp, (int)sizeof(tmp), fmt, ap);
    va_end(ap);

    /* 会话线程和转发线程都会调它，所以拷贝到共享缓冲时上锁。
     * 先在【栈上】把整句拼好，锁里只做一次定长拷贝 —— 锁的持有时间极短。 */
    adbp_lock_init();
    ADBP_LOCK();
    for (i = 0; i < (int)sizeof(g_status) - 1 && tmp[i]; i++)
        g_status[i] = tmp[i];
    g_status[i] = 0;
    ADBP_UNLOCK();

    /* 转发器说的每句话都进日志。这一层最容易出问题（ADB 命令、通道、端口），
     * 现场看不出来，只有日志能带回来。主机测试里 ADBP_LOG 是空的。 */
    ADBP_LOG("转发器 | %s", tmp);
}

static void set_nonblock(SOCKET s)
{
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
}

static void close_conn(int i)
{
    if (!g_conn[i].used)
        return;

    /* ⚠⚠ 这里原来有一颗地雷，必须记下来：
     *
     *     if (g_conn[i].chan > 0)
     *         ADBP_LOCK();                        ← 宏展开成 EnterCriticalSection(...)
     *         adb_close_chan(&g_adb, ...);
     *         ADBP_UNLOCK();                      ← 没有大括号！
     *
     *   ADBP_LOCK() 是宏，展开后是一句普通语句，所以 if 只管住了
     *   EnterCriticalSection 那一句。结果是：
     *     · chan <= 0 时根本没进锁，却照样 LeaveCriticalSection；
     *     · 对一把「自己没持有的」临界区调用 LeaveCriticalSection，
     *       在 WinCE 上会把它的锁计数/持有者搞乱 —— 之后两把线程可能同时
     *       进锁，g_adb 的通道表就被写坏，表象是「数据莫名少了/通道莫名关了」，
     *       和当年那次「加了锁反而更快崩」是同一类毛病，极难查。
     *   现在按注释里本来就想表达的意思改：整个函数体在锁里。 */
    ADBP_LOCK();
    if (g_conn[i].chan > 0)
        adb_close_chan(&g_adb, g_conn[i].chan);
    if (g_conn[i].sock != INVALID_SOCKET)
        closesocket(g_conn[i].sock);
    g_conn[i].used = 0;
    g_conn[i].sock = INVALID_SOCKET;
    g_conn[i].chan = 0;
    ADBP_UNLOCK();
}

static void conn_push_to_phone(int i, const unsigned char *d, int n)
{
    int bad = 0;

    /* ⚠ 绝不能在这里提前 return —— 忘了 UNLOCK 就是永久死锁，
     *   转发线程会卡在下一轮 ADBP_LOCK 上，表现是「连上之后彻底不动」。
     *   所有出口都走下面的统一解锁。 */
    ADBP_LOCK();
    if (adb_send(&g_adb, g_conn[i].chan, d, n) < 0)
        bad = 1;
    else
        g_tx_bytes += (unsigned long)n;
    ADBP_UNLOCK();

    if (bad)
        close_conn(i);           /* 它自己会再加锁，可重入，不会死锁 */
}

/* 转发被拒时，尝试把手机端拉起来。
 * 为什么要做这件事：如果手机上的 CarLife/Jovi InCar 没在运行，手机本地
 * 7240 端口就没人监听，adbd 会直接拒绝我们的 OPEN。EasyConnected 是靠
 * 往手机推一个 carman 程序并执行来解决的；我们用 ADB 的 shell 服务更省事：
 * 先列包名找到 CarLife，再用 monkey 启动它。 */
/* 让 Android 自己告诉我们这个包的【启动 Activity】叫什么。
 *
 * 这比写死包名和 Activity 名靠谱得多：vivo / OPPO / 小米 的组件名各不相同，
 * 而且 com.baidu.carlife 在多数手机上是【无界面组件】，没有 LAUNCHER 图标，
 * 所以 `monkey -c android.intent.category.LAUNCHER` 会直接失败
 * （原来就只用这一条命令，等于赌手机端有启动图标）。
 *
 * cmd package resolve-activity 是 Android 7+ 自带的，各家 ROM 都有。 */
static int resolve_activity(const char *pkg, char *out, int cap)
{
    char cmd[320];
    char buf[1024];
    char *p, *q;

    if (!pkg || !out || cap <= 0) return -1;
    out[0] = 0;

    snprintf(cmd, sizeof(cmd) - 1, "cmd package resolve-activity --brief %s", pkg);
    buf[0] = 0;
    /* 自己加锁做纵深防御：现在只从 launch_body 调（那边已持锁，可重入无害），
     * 但万一日后别处调用，也不会漏锁。 */
    ADBP_LOCK();
    {
        int rr = adb_run_shell(&g_adb, cmd, buf, (int)sizeof(buf) - 1, 6000);
        ADBP_UNLOCK();
        if (rr != 0)
            return -1;
    }

    /* 输出形如：com.baidu.carlife/com.baidu.carlife.CarlifeActivity */
    p = strchr(buf, '/');
    if (!p) return -1;
    p++;
    q = p;
    while (*q && *q != '\r' && *q != '\n' && *q != ' ' && *q != '\t') q++;
    if (q == p || (int)(q - p) >= cap) return -1;
    memcpy(out, p, (size_t)(q - p));
    out[q - p] = 0;
    return 0;
}

/* 确认这个包【真的在跑】。
 * 这一步把「已尝试启动」变成「确认真起来了」——
 * 车机没法实测，所以任何"我发过命令了"都不算数，必须有事实。 */
static int pkg_running(const char *pkg)
{
    char cmd[320];
    char buf[512];
    const char *p;

    if (!pkg) return -1;
    snprintf(cmd, sizeof(cmd) - 1, "pidof %s", pkg);
    buf[0] = 0;
    ADBP_LOCK();
    {
        int rr = adb_run_shell(&g_adb, cmd, buf, (int)sizeof(buf) - 1, 3000);
        ADBP_UNLOCK();
        if (rr != 0)
            return -1;                   /* 问不出来 —— 不算"没在跑" */
    }
    p = buf;
    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
    /* pidof 有数字输出 = 进程在 */
    return (*p >= '0' && *p <= '9') ? 1 : 0;
}

/* ⚠ 不变量：本函数【只能】由 adbp_launch_phone_app 调用 —— 那里整体持着
 *   ADB 锁。直接调它会让 ADB 访问逃出锁外（这就是当初那个双线程竞态）。 */
static int launch_body(char *detail, int cap)
{
    char  buf[8192];
    char  pkg[208];
    char  act[256];
    char  cmd[600];
    int   r;
    int   running;

    if (detail && cap > 0) detail[0] = 0;

    ADBP_LOG("向手机发 shell: pm list packages");
    r = adb_run_shell(&g_adb, "pm list packages", buf, (int)sizeof(buf), 6000);
    if (r != 0) {
        ADBP_LOG("   失败，返回 %d", r);
        snprintf(g_note, sizeof(g_note) - 1, "列包名失败（adb_run_shell 返回 %d）", r);
        if (detail) snprintf(detail, (size_t)cap, "%s", g_note);
        return -1;
    }

    /* 把清单留一份，现场可以直接看手机里智能车载到底叫什么 */
    snprintf(g_pkgs, sizeof(g_pkgs) - 1, "%s", buf);
    g_pkgs[sizeof(g_pkgs) - 1] = 0;

    if (adb_find_carlife_pkg(buf, pkg, (int)sizeof(pkg)) != 0) {
        snprintf(g_note, sizeof(g_note) - 1,
                 "手机里没找到智能车载的包（认得 %d 字节包名清单，首字节 0x%02X）",
                 (int)strlen(buf), (int)(unsigned char)buf[0]);
        if (detail) snprintf(detail, (size_t)cap, "%s", g_note);
        return -1;
    }

    /* ── ① 先问 Android 要启动 Activity，再 am start 指过去 ──
     * 这条路对"无界面组件"也有效，是最通用的一条。 */
    act[0] = 0;
    ADBP_LOG("向手机发 shell: cmd package resolve-activity --brief %s", pkg);
    if (resolve_activity(pkg, act, (int)sizeof(act)) == 0) {
        ADBP_LOG("   拿到启动项: %s", act);
        snprintf(cmd, sizeof(cmd) - 1, "am start -n %s/%s", pkg, act);
        ADBP_LOG("向手机发 shell: %s", cmd);
        buf[0] = 0;
        adb_run_shell(&g_adb, cmd, buf, (int)sizeof(buf) - 1, 8000);
        ADBP_LOG("   手机回: %s", buf[0] ? buf : "(空)");
    } else {
        ADBP_LOG("   没问到启动项，改用 monkey");
    }

    /* ── ② 确认在不在跑；不在就用 monkey 兜一次 ── */
    running = pkg_running(pkg);
    if (running != 1) {
        snprintf(cmd, sizeof(cmd) - 1,
                 "monkey -p %s -c android.intent.category.LAUNCHER 1", pkg);
        ADBP_LOG("向手机发 shell: %s", cmd);
        buf[0] = 0;
        adb_run_shell(&g_adb, cmd, buf, (int)sizeof(buf) - 1, 8000);
        ADBP_LOG("   手机回: %s", buf[0] ? buf : "(空)");
    }

    /* ── ③ 再确认一次，把结果写进 detail 给人看 ──
     *
     * ⚠⚠ 返回值语义（踩过坑，务必看清）：
     *    0  = 「启动动作已经做过了，你可以去重试端口了」
     *    而【不是】「我确认它已经在跑了」。
     *
     *    踩的坑：一开始写成 pidof 确认在跑才算成功，于是……
     *      · 很多手机（以及主机测试里的假手机）根本不允许 shell 查询别的进程，
     *        pidof 返回空 → 判为没起来 → 返回 -1
     *      · 调用方 open_with_retry 看到 -1 就【直接放弃重试】
     *      · 而重试才是真正把连接做起来的动作
     *    结果：启动命令发了、手机端也真起来了，转发器却再也不去重试端口，
     *    车机上表现就是「一直连不上」。
     *
     *    这个坑是主机端到端测试抓出来的：
     *      ❌ ★ 启动后重试 tcp:7240 成功   tcp:7240 出现 1 次
     *
     *    所以：验证结果只写进 detail 给人看，绝不拿它决定要不要重试。 */
    running = pkg_running(pkg);
    if (running == 1)
        snprintf(g_note, sizeof(g_note) - 1, "已确认 %s 在手机端运行，重试端口", pkg);
    else if (running == 0)
        snprintf(g_note, sizeof(g_note) - 1,
                 "%s 已拉起（手机不允许查进程，无法进一步确认），重试端口", pkg);
    else
        snprintf(g_note, sizeof(g_note) - 1,
                 "%s 已拉起（无法查询进程状态），重试端口", pkg);

    if (detail) snprintf(detail, (size_t)cap, "%s", g_note);
    return 0;                       /* 启动动作已完成 → 调用方应当重试 */
}

/* 对外的入口：加锁版。函数体在 launch_body 里。
 * 这一整段会做 3~4 次 shell 往返（最坏几十秒），全程持锁 ——
 * 期间转发线程不碰 ADB。这是刻意的：这段时间手机端还没起来，
 * 本来也没有数据要转发。 */
int adbp_launch_phone_app(char *detail, int cap)
{
    int r;
    adbp_lock_init();
    ADBP_LOCK();
    r = launch_body(detail, cap);
    ADBP_UNLOCK();
    return r;
}


const char *adbp_phone_packages(void)
{
    return g_pkgs;
}

/* 借已建立的 ADB 通道在手机上跑一条 shell 命令。
 * ⚠ 一定要加锁：转发线程和会话线程共用同一个 g_adb，通道表是按槽位下标算的，
 *   两边同时动会把通道分配搞坏（见文件开头那段说明）。 */
int adbp_shell(const char *cmd, char *out, int cap, int timeout_ms)
{
    char tmp[1024];
    int  r, i, o = 0;

    if (!cmd || !out || cap <= 0) return -1;
    out[0] = 0;
    tmp[0] = 0;

    adbp_lock_init();
    ADBP_LOCK();
    r = adb_run_shell(&g_adb, cmd, tmp, (int)sizeof(tmp) - 1, timeout_ms);
    ADBP_UNLOCK();
    if (r != 0) return r;

    /* 去掉行尾的 \r \n 和多余空白 —— 输出要当普通一行日志用，
     * 夹带换行会把日志的时间戳列冲乱。 */
    for (i = 0; tmp[i] && o < cap - 1; i++) {
        char c = tmp[i];
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        out[o++] = c;
    }
    out[o] = 0;

    /* 顺手把尾部空格修掉 */
    while (o > 0 && out[o - 1] == ' ') out[--o] = 0;
    return 0;
}

const char *adbp_last_note(void)
{
    return g_note;
}

/* 打开一条转发；被拒就先试着把手机端拉起来，然后重试一次。
 * 必须由持有设备的那个线程调用（内部会收发 ADB 数据）。 */
static int open_with_retry(const char *service)
{
    int id;
    adbp_lock_init();
    ADBP_LOCK();
    id = adb_open(&g_adb, service);
    ADBP_UNLOCK();

    if (id >= 0)
        return id;
    if (g_tried_launch)
        return id;
    g_tried_launch = 1;

    set_status("手机端没在跑，正在尝试拉起 ...");
    {
        char detail[256];
        if (adbp_launch_phone_app(detail, (int)sizeof(detail)) == 0) {
            set_status("%s，等它起来 ...", detail);
            ADBP_SLEEP(2500);                /* 给它一点启动时间 */
            ADBP_LOCK();
            id = adb_open(&g_adb, service);
            ADBP_UNLOCK();
        } else {
            set_status("%s", detail);
        }
    }
    return id;
}

static ADBP_THREAD_RET ADBP_API adbp_thread(ADBP_THREAD_ARG arg)
{
    unsigned char *buf = (unsigned char *)malloc(ADBP_BUF);
    (void)arg;

    /* 线程一进来就先记一笔 —— 有了它，日志就能分清
     * 「卡在起线程之前」和「线程起来了但里面卡住」这两种完全不同的情况。 */
    ADBP_LOG("   [转发线程] 已启动，要转发 %d 条服务", g_nsvc);

    if (!buf) {
        ADBP_LOG("   [转发线程] 内存不足（要 %d 字节）", (int)ADBP_BUF);
        set_status("内存不足，转发器无法启动");
        g_stop = 1;
        g_running = 0;
        return 0;
    }
    ADBP_LOG("   [转发线程] 缓冲已分配 %d 字节", (int)ADBP_BUF);

    while (!g_stop) {
        fd_set rf;
        struct timeval tv;
        int    i, r, maxfd = -1;

        /* ① 先看本地有没有数据要发往手机 */
        FD_ZERO(&rf);
        for (i = 0; i < g_nsvc; i++) {
            if (g_svc[i].lsn == INVALID_SOCKET) continue;
            FD_SET(g_svc[i].lsn, &rf);
            if ((int)g_svc[i].lsn > maxfd) maxfd = (int)g_svc[i].lsn;
        }
        for (i = 0; i < ADBP_MAX_CONN; i++) {
            if (!g_conn[i].used) continue;
            FD_SET(g_conn[i].sock, &rf);
            if ((int)g_conn[i].sock > maxfd) maxfd = (int)g_conn[i].sock;
        }
        if (maxfd < 0) {
            ADBP_SLEEP(10);
            continue;
        }

        /* ⚠ nfds 必须传「最大 fd + 1」，不能图省事写 0。
         *   WinSock 会忽略这个参数，但 POSIX 是靠它决定要检查哪些位的 ——
         *   传 0 等于「一个 fd 都不用看」，select 会立刻返回且什么都不报告，
         *   于是新连接永远 accept 不到（表象是客户端 connect 成功但没反应，
         *   因为内核完成了握手、应用层却没接手）。
         *   同样的坑在 carlife.c 里也踩过一次，两处都已修。 */
        tv.tv_sec = 0;
        tv.tv_usec = 10000;                    /* 10 毫秒一轮 */
        r = select(maxfd + 1, &rf, NULL, NULL, &tv);

        if (r > 0) {
            /* 新连接 */
            for (i = 0; i < g_nsvc; i++) {
                if (g_svc[i].lsn == INVALID_SOCKET) continue;
                if (!FD_ISSET(g_svc[i].lsn, &rf)) continue;
                for (;;) {
                    SOCKET c = accept(g_svc[i].lsn, NULL, NULL);
                    int    k, slot = -1;
                    if (c == INVALID_SOCKET) break;
                    for (k = 0; k < ADBP_MAX_CONN; k++)
                        if (!g_conn[k].used) { slot = k; break; }
                    if (slot < 0) { closesocket(c); break; }
                    set_nonblock(c);
                    g_conn[slot].used = 1;
                    g_conn[slot].sock = c;
                    g_conn[slot].svc  = i;
                    g_conn[slot].chan = open_with_retry(g_svc[i].service);
                    if (g_conn[slot].chan < 0) {
                        closesocket(c);
                        g_conn[slot].used = 0;
                        g_conn[slot].sock = INVALID_SOCKET;
                        set_status("转发 %s 被拒（手机端没在监听这个端口）",
                                   g_svc[i].service);
                    } else {
                        set_status("已转发 %s → 本机 127.0.0.1:%u",
                                   g_svc[i].service, g_svc[i].lport);
                    }
                }
            }

            /* 本地→手机 */
            for (i = 0; i < ADBP_MAX_CONN; i++) {
                if (!g_conn[i].used) continue;
                if (!FD_ISSET(g_conn[i].sock, &rf)) continue;
                r = recv(g_conn[i].sock, (char *)buf, ADBP_BUF, 0);
                if (r == 0) {
                    close_conn(i);             /* 本地关了 */
                } else if (r > 0) {
                    conn_push_to_phone(i, buf, r);
                } else {
                    /* 非阻塞 socket 上 EWOULDBLOCK 是正常的，不算错 */
                    if (WSAGetLastError() != WSAEWOULDBLOCK)
                        close_conn(i);
                }
            }
        }

        /* ② 收设备这一侧（非阻塞，收不到就算了） */
        ADBP_LOCK();
        r = adb_pump(&g_adb, 0);
        ADBP_UNLOCK();
        if (r < 0) {
            set_status("ADB 通道断了（错误 %d）", r);
            g_stop = 1;
            break;
        }

        /* ③ 设备→本地 */
        for (i = 0; i < ADBP_MAX_CONN; i++) {
            if (!g_conn[i].used) continue;
              for (;;) {
                  int n;
                  /* 取数据要加锁；往本地 socket 送数据【不锁】——
                   * 送数据可能阻塞，把它圈进锁里会把「拉起手机端」那几十秒拖得更长。 */
                  ADBP_LOCK();
                  n = adb_recv(&g_adb, g_conn[i].chan, buf, ADBP_BUF);
                  ADBP_UNLOCK();
                  if (n <= 0) break;
                  if (send(g_conn[i].sock, (const char *)buf, n, 0) <= 0) {
                      close_conn(i);
                      break;
                  }
                  g_rx_bytes += (unsigned long)n;
              }
              if (g_conn[i].used) {
                  int cls;
                  ADBP_LOCK();
                  cls = adb_chan_closed(&g_adb, g_conn[i].chan);
                  ADBP_UNLOCK();
                  if (cls)
                      close_conn(i);
              }
        }
    }

    free(buf);
    g_running = 0;
    return 0;
}

/* 核心：用给定的设备 I/O 启动转发。
 * 单独拿出来是为了能在电脑上用 socket 假装成设备，端到端验证转发逻辑。 */
int adbp_start_with_io(ADB_IO io, const char * const *services, int n_services,
                       unsigned short *local_ports, char *reason, int reason_cap)
{
#ifndef ADBP_HOST_TEST
    WSADATA wsa;
#endif
    int     i;

    if (reason && reason_cap > 0) reason[0] = 0;
    adbp_lock_init();

    g_stop = 0;
    g_running = 0;
    g_tx_bytes = g_rx_bytes = 0;
    g_nsvc = 0;
    /* ⚠ 每次启动都要清掉「本进程已经试过拉起手机端」这颗旗标。
     *   open_transport 会反复调 adbp_start（最多 60 次），旗标留着的话，
     *   后面几轮即使手机端真没起来也不会再拉。 */
    g_tried_launch = 0;

    for (i = 0; i < ADBP_MAX_CONN; i++) {
        g_conn[i].used = 0;
        g_conn[i].sock = INVALID_SOCKET;
        g_conn[i].chan = 0;
    }
    for (i = 0; i < ADBP_MAX_SVC; i++) {
        g_svc[i].used = 0;
        g_svc[i].lsn  = INVALID_SOCKET;
    }

#ifndef ADBP_HOST_TEST
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (reason) snprintf(reason, (size_t)reason_cap, "Winsock 初始化失败");
        return -1;
    }
#endif
    if (n_services <= 0 || n_services > ADBP_MAX_SVC) {
        if (reason) snprintf(reason, (size_t)reason_cap, "服务数量不对");
        return -2;
    }

    /* ① 连上并完成认证 */
    {
        int r;
#ifndef ADBP_HOST_TEST
        adb_set_trace(adb_trace_log);      /* 握手的每一步都进日志 */
#endif
        r = adb_connect(&g_adb, io);
        if (r != 0) {
            if (reason) {
                switch (r) {
                case -3:
                    snprintf(reason, (size_t)reason_cap,
                             "手机拒绝了 ADB 授权。"
                             "请在手机上点「允许 USB 调试」后重试");
                    break;
                case -4:
                    /* 包发出去了、手机一个字都没回。
                     * 这是「手机上没开 USB 调试」最典型的表现。 */
                    snprintf(reason, (size_t)reason_cap,
                             "手机没应答（USB 调试可能没打开，"
                             "或没点「允许 USB 调试」）");
                    break;
                case -5:
                    snprintf(reason, (size_t)reason_cap,
                             "ADB 设备读写失败（设备名认到了，但数据进不去）");
                    break;
                default:
                    snprintf(reason, (size_t)reason_cap,
                             "ADB 握手失败（错误 %d）", r);
                    break;
                }
                reason[reason_cap - 1] = 0;
            }
            /* ⚠ 这里【刻意不关设备】。
             *   现场日志显示：每 2 秒一轮「开→失败→关」之后，
             *   ADB1: 会接连十几次返回 110(ERROR_OPEN_FAILED)，
             *   也就是这个驱动的关闭是脏的，关一次要几十秒才缓过来。
             *   句柄留着，下一轮直接复用；只有设备确认消失（拔线）
             *   或者连续失败很多次时才真的重开一次，避免句柄僵死。 */
            g_fail_streak++;
#ifdef ADBP_HOST_TEST
            if (g_fail_streak >= 4) {
                adbp_close_device();
                g_fail_streak = 0;
            }
#else
            if (adbio_ce_fatal() || g_fail_streak >= 4) {
                ADBP_LOG("   设备句柄重开（连续失败 %d 次 / 设备消失 %d）",
                         g_fail_streak, adbio_ce_fatal());
                adbp_close_device();
                g_fail_streak = 0;
            }
#endif
            return -4;
        }
        g_fail_streak = 0;
        /* ⚠ 这一行是分界线：现场上一次的日志就断在这之前。
         *   从这里往后每一步都补了日志 —— 万一又断，断在哪一步一目了然。 */
        ADBP_LOG("   [转发] ADB 握手成功，开始建本地监听");
    }

    /* ③ 每个服务：建立本地监听 + 打开一条转发通道 */
    ADBP_LOG("   [转发] 要建 %d 条本地监听", n_services);
    for (i = 0; i < n_services; i++) {
        SOCKET s;
        struct sockaddr_in sa;
        int sl = 0;

        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) {
            ADBP_LOG("   [转发] 第 %d 条建 socket 失败（错误 %d）",
                     i + 1, (int)WSAGetLastError());
            if (reason) snprintf(reason, (size_t)reason_cap, "建 socket 失败");
            goto fail;
        }
        ADBP_LOG("   [转发] 第 %d 条 socket 已建好", i + 1);
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port   = 0;                      /* 让系统挑端口，避免撞车 */
        sa.sin_addr.s_addr = htonl(0x7F000001UL);   /* 只听本机 */
        if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            ADBP_LOG("   [转发] 第 %d 条 bind 失败（错误 %d）",
                     i + 1, (int)WSAGetLastError());
            if (reason) snprintf(reason, (size_t)reason_cap, "bind 失败");
            closesocket(s);
            goto fail;
        }
        if (listen(s, 2) != 0) {
            ADBP_LOG("   [转发] 第 %d 条 listen 失败（错误 %d）",
                     i + 1, (int)WSAGetLastError());
            if (reason) snprintf(reason, (size_t)reason_cap, "listen 失败");
            closesocket(s);
            goto fail;
        }
        ADBP_LOG("   [转发] 第 %d 条 listen 成功", i + 1);
        set_nonblock(s);

        /* 取回系统分配的实际端口 */
        {
            struct sockaddr_in got;
            int gl = sizeof(got);
            if (getsockname(s, (struct sockaddr *)&got, &gl) == 0)
                g_svc[i].lport = ntohs(got.sin_port);
        }

        g_svc[i].used = 1;
        g_svc[i].lsn  = s;
        sl = (int)strlen(services[i]);
        if (sl > 63) sl = 63;
        memcpy(g_svc[i].service, services[i], (size_t)sl);
        g_svc[i].service[sl] = 0;
        if (local_ports)
            local_ports[i] = g_svc[i].lport;
        g_nsvc = i + 1;
        ADBP_LOG("   [转发] %s → 本机 127.0.0.1:%u 已在监听",
                 g_svc[i].service, (unsigned)g_svc[i].lport);
    }
    ADBP_LOG("   [转发] 本地监听全部就绪，准备起转发线程");

    /* ④ 起线程 */
#ifdef ADBP_HOST_TEST
    g_thread = hostplat_spawn(adbp_thread, NULL);
#else
    g_thread = CreateThread(NULL, 0, adbp_thread, NULL, 0, &g_tid);
#endif
    ADBP_LOG("   [转发] CreateThread 返回 %lu", (unsigned long)g_thread);
    if (!g_thread) {
        ADBP_LOG("   [转发] 建线程失败（错误 %lu）", (unsigned long)GetLastError());
        if (reason) snprintf(reason, (size_t)reason_cap, "建线程失败");
        goto fail;
    }
    g_running = 1;
    set_status("已连接手机，%d 条端口转发就绪", g_nsvc);
    ADBP_LOG("   [转发] 全部就绪，返回成功");
    return 0;

fail:
    for (i = 0; i < ADBP_MAX_SVC; i++) {
        if (g_svc[i].lsn != INVALID_SOCKET) {
            closesocket(g_svc[i].lsn);
            g_svc[i].lsn = INVALID_SOCKET;
        }
        g_svc[i].used = 0;
    }
    adbp_close_device();
    g_nsvc = 0;
    return -5;
}

/* 车机入口：打开 ADB 设备，然后交给核心。 */
#ifndef ADBP_HOST_TEST
int adbp_start(const char * const *services, int n_services,
               unsigned short *local_ports, char *reason, int reason_cap)
{
    WCHAR devname[64];
    char  devreason[256];

    if (reason && reason_cap > 0) reason[0] = 0;

    if (adbio_ce_open(devname, 64, devreason, (int)sizeof(devreason)) != 0) {
        if (reason) {
            snprintf(reason, (size_t)reason_cap, "%s", devreason);
            reason[reason_cap - 1] = 0;
        }
        return -3;
    }
    g_own_device = 1;
    return adbp_start_with_io(adbio_ce_io(), services, n_services,
                              local_ports, reason, reason_cap);
}
#endif

void adbp_stop(void)
{
    int i;

    g_stop = 1;
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = 0;
    }
    for (i = 0; i < ADBP_MAX_CONN; i++)
        close_conn(i);
    for (i = 0; i < ADBP_MAX_SVC; i++) {
        if (g_svc[i].lsn != INVALID_SOCKET) {
            closesocket(g_svc[i].lsn);
            g_svc[i].lsn = INVALID_SOCKET;
        }
        g_svc[i].used = 0;
    }
    adbp_close_device();
    g_nsvc = 0;
    g_running = 0;
}

int adbp_running(void)
{
    return g_running;
}

const char *adbp_status(void)
{
    return g_status;
}

void adbp_stats(unsigned long *to_phone, unsigned long *from_phone)
{
    if (to_phone)   *to_phone   = g_tx_bytes;
    if (from_phone) *from_phone = g_rx_bytes;
}
