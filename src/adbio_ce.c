/* adbio_ce.c — 见 adbio_ce.h。 */
#include "adbio_ce.h"
#include "cllog.h"

#include <string.h>
#include <stdio.h>

static HANDLE g_h = INVALID_HANDLE_VALUE;
static WCHAR  g_name[64];

/* ── 诊断计数 ──
 * 现场只有一次机会，所以每个失败点都要留下数字证据：
 * 到底是「写不进去」、「读不出来」、还是「读得到但此刻没数据」。
 * 这三种以前全都塌成同一个 -1，现场根本分不清。 */
static unsigned long g_in_bytes;      /* 累计读到的字节 */
static unsigned long g_out_bytes;     /* 累计写出的字节 */
static unsigned long g_err_read;      /* ReadFile 真失败次数 */
static unsigned long g_err_write;     /* WriteFile 真失败次数 */
static unsigned long g_no_data;       /* ReadFile 成功但 0 字节（暂时没数据） */
static unsigned long g_last_err;      /* 最近一次失败的错误码 */
static unsigned long g_wr_zero;       /* WriteFile 成功但写了 0 字节的次数 */
static int           g_fatal;         /* 设备已确认消失，句柄该丢了 */

/* 这个错误码是不是「设备真的没了」。
 * 只有这几种才允许把句柄判死；其余的（尤其是「现在没数据」）都必须继续等。 */
static int err_is_fatal(DWORD e)
{
    return (e == ERROR_DEV_NOT_EXIST || e == ERROR_BAD_UNIT ||
            e == ERROR_INVALID_HANDLE || e == ERROR_ACCESS_DENIED);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  读线程 + 环形缓冲  —— 本文件最要紧的一块
 *
 *  ⚠⚠ 为什么非这样不可（这是现场日志实证出来的，不是设计洁癖）：
 *
 *    现场日志（carlife-log.txt）里每一轮都是同一个形状：
 *        [123.4] 转发线程已启动
 *        [189.6] getprop sys.usb.config = [mtp,adb]      ← 第一条 shell 用了 66 秒
 *        [189.6] 后面 5 条 getprop + 3 条 pm list 全部【秒失败】
 *        [195.0] 转发器 | ADB 通道断了（错误 -2）
 *
 *    66 秒不是手机慢 —— 是【我们自己把它卡住了】：
 *      · 这台车机的 ADB 驱动 ReadFile 是【阻塞】的，而且它自己要等很久才返回；
 *      · 转发线程每 10 毫秒就调一次 adb_pump(&g_adb, 0)，
 *        而那句调用【是握着 ADB 锁的】（adbproxy.c 里 ADBP_LOCK/UNLOCK 夹着它）；
 *      · 传入的「超时 0」根本不管用 —— ReadFile 照样阻塞几十秒。
 *    于是转发线程一握锁就是几十秒，会话线程要发的 shell 命令全被挡在锁外。
 *    等那次阻塞读终于返回，USB 管道已经被拖进错误状态：
 *    之后每次写都「成功但写了 0 字节」（所以"写失败 0 次"却秒退 -1），
 *    最后整条流对不齐 → 错误 -2。
 *
 *  改成现在这样之后：
 *    · 读线程【独占设备做阻塞读】，读到就往环形缓冲放；
 *    · 应用层的 ce_read 只从环形缓冲取 —— 【永远是有界的】，不可能再阻塞几十秒；
 *    · 读线程【不碰 ADB 锁】，所以锁的持有时间不再受设备影响。
 * ══════════════════════════════════════════════════════════════════════════ */
#define RCAP (512 * 1024)

static unsigned char     g_rbuf[RCAP];
static volatile int      g_rlen;            /* 缓冲里还剩多少字节 */
static volatile int      g_rhead;
static volatile int      g_rstop;           /* 让读线程退出 */
static volatile int      g_rdead;           /* 读线程已经退出 */
static volatile int      g_rdrop;           /* 因为缓冲满丢过的次数 */
static volatile int      g_pending_close;   /* 读线程还卡在 ReadFile，句柄交给它关 */
/* ⚠ 读线程【只】用这个句柄。绝不能让它去用 g_h ——
 *   延迟关闭那条路径上，g_h 可能已经被换成了下一轮的新句柄，
 *   那种时候线程一 CloseHandle 就把【新】句柄关掉了，后果极难查。 */
static HANDLE            g_rh = INVALID_HANDLE_VALUE;
static HANDLE            g_rthr;
static DWORD             g_rtid;
static HANDLE            g_rev;             /* 有数据时置位（自动复位） */
static CRITICAL_SECTION  g_rlock;
static int               g_rlock_ready;

static void ring_lock_init(void)
{
    if (!g_rlock_ready) {
        InitializeCriticalSection(&g_rlock);
        g_rlock_ready = 1;
    }
}

static void ring_reset(void)
{
    ring_lock_init();
    EnterCriticalSection(&g_rlock);
    g_rhead = 0;
    g_rlen  = 0;
    LeaveCriticalSection(&g_rlock);
    g_rdrop = 0;
}

static int ring_get(unsigned char *out, int cap)
{
    int n = 0;
    if (!g_rlock_ready) return 0;
    EnterCriticalSection(&g_rlock);
    if (g_rlen > 0) {
        int first;
        n = g_rlen;
        if (n > cap) n = cap;
        first = RCAP - g_rhead;
        if (first > n) first = n;
        memcpy(out, g_rbuf + g_rhead, (size_t)first);
        if (n > first)
            memcpy(out + first, g_rbuf, (size_t)(n - first));
        g_rhead = (g_rhead + n) % RCAP;
        g_rlen -= n;
    }
    LeaveCriticalSection(&g_rlock);
    return n;
}

static void ring_put(const unsigned char *d, int n)
{
    int i = 0;
    while (i < n) {
        int free_, chunk;
        EnterCriticalSection(&g_rlock);
        free_  = RCAP - g_rlen;
        chunk  = n - i;
        if (chunk > free_) chunk = free_;
        if (chunk > 0) {
            int tail  = (g_rhead + g_rlen) % RCAP;
            int first = RCAP - tail;
            if (first > chunk) first = chunk;
            memcpy(g_rbuf + tail, d + i, (size_t)first);
            if (chunk > first)
                memcpy(g_rbuf, d + i + first, (size_t)(chunk - first));
            g_rlen += chunk;
        }
        LeaveCriticalSection(&g_rlock);

        if (chunk <= 0) {          /* 满了：等应用层取走 */
            g_rdrop++;
            Sleep(2);
            continue;
        }
        i += chunk;
    }
    if (g_rev)
        SetEvent(g_rev);
}

/* 读线程：唯一碰设备做读的地方。
 * 它【只】做「读 → 放进环形缓冲」，不碰 ADB 锁、不做任何协议解析。 */
static DWORD WINAPI reader_thread(LPVOID p)
{
    unsigned char tmp[4096];
    HANDLE        h = g_rh;            /* 本线程专用的句柄，中途不会被换掉 */
    (void)p;

    while (!g_rstop && h != INVALID_HANDLE_VALUE) {
        DWORD got = 0;

        if (ReadFile(h, tmp, (DWORD)sizeof(tmp), &got, NULL)) {
            if (got > 0)
                ring_put(tmp, (int)got);
            else
                Sleep(5);            /* 驱动是「非阻塞」的话走这里，别空转 */
        } else {
            DWORD e = GetLastError();
            g_last_err = e;
            if (err_is_fatal(e)) {
                g_fatal = 1;
                g_err_read++;
                break;
            }
            /* 非致命：这台驱动读不到东西时会阻塞很久，走到这里说明
             * 它是立刻返回的（另一种驱动风格）。歇一下再读，别把 CPU 打满。 */
            Sleep(20);
        }
    }

    g_rdead = 1;
    if (g_rev)
        SetEvent(g_rev);             /* 叫醒还等在那儿的人 */

    /* 外面在等我们退出、而且已经把逻辑句柄清掉了 —— 现在安全了，关掉它 */
    if (g_pending_close) {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);           /* 关的是【我这个】句柄 */
        g_rh = INVALID_HANDLE_VALUE;
        g_pending_close = 0;
        g_rdead = 0;
    }
    return 0;
}

static void reader_start(void)
{
    ring_reset();
    g_rh = g_h;
    if (!g_rev)
        g_rev = CreateEventW(NULL, FALSE, FALSE, NULL);   /* 自动复位；失败也不致命 */
    g_rstop = 0;
    g_rdead = 0;
    g_rthr  = CreateThread(NULL, 0, reader_thread, NULL, 0, &g_rtid);
}

/* 停读线程。⚠ 它可能正阻塞在 ReadFile 里（这台驱动就是这样），
 *   所以只能等一小会儿；等不到就【不关句柄】—— 关掉一个正在读的句柄
 *   是未定义行为，比泄漏句柄危险得多。让线程自己退出来时收尾。 */
static void reader_stop(void)
{
    if (!g_rthr)
        return;
    g_rstop = 1;
    if (WaitForSingleObject(g_rthr, 2000) == WAIT_OBJECT_0) {
        CloseHandle(g_rthr);
        g_rthr = 0;
    } else {
        g_rthr = 0;                  /* 交给线程自己收尾 */
    }
}

void adbio_ce_ring(int *used, int *cap, unsigned long *drops)
{
    if (used)  *used  = g_rlen;
    if (cap)   *cap   = RCAP;
    if (drops) *drops = g_rdrop;
}

int adbio_ce_reader_alive(void)
{
    return (g_rthr != 0 && !g_rdead);
}

/* 手写的宽字符串拷贝。
 * ⚠ 刻意不用 lstrcpynW / lstrcpyW：手上的 COREDLL 导出表里没有它们
 *   （只有 lstrcmpW / lstrcmpiW）。虽然本次链接通过了，但一旦真机上缺这个
 *   导出，程序会「一点就闪退」—— 这种代价太大，不值得为省一个循环去赌。
 *   手写 8 行的循环，行为完全确定。 */
static void w_copy(WCHAR *dst, const WCHAR *src, int cap)
{
    int i;
    if (cap <= 0) return;
    for (i = 0; i < cap - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

/* 候选设备名。ADB1: 是这台车机实际在用的名字 —— 两条独立证据：
 *   ① 车机自带 CECarLife.exe 的字符串里就有 "ADB1:"；
 *   ② 车机自带 adb_driver.dll 的注册表值 Name = "ADB1:"。
 * 其余的是常见变体，多试几个不吃亏。 */
static const WCHAR *g_cands[] = {
    L"ADB1:", L"ADB0:", L"ADB2:", L"ADB3:", L"tADB1:", L"ADB:"
};

int adbio_ce_is_open(void)
{
    return g_h != INVALID_HANDLE_VALUE;
}

int adbio_ce_fatal(void)
{
    return g_fatal;
}

void adbio_ce_stats(unsigned long *in_bytes, unsigned long *out_bytes,
                    unsigned long *read_err, unsigned long *write_err,
                    unsigned long *no_data, unsigned long *last_err)
{
    if (in_bytes)  *in_bytes  = g_in_bytes;
    if (out_bytes) *out_bytes = g_out_bytes;
    if (read_err)  *read_err  = g_err_read;
    if (write_err) *write_err = g_err_write;
    if (no_data)   *no_data   = g_no_data;
    if (last_err)  *last_err  = g_last_err;
}

void adbio_ce_stats_reset(void)
{
    g_in_bytes = 0;
    g_out_bytes = 0;
    g_err_read = 0;
    g_err_write = 0;
    g_no_data = 0;
    g_last_err = 0;
    g_wr_zero = 0;
}

/* 读：只从环形缓冲取，【永远有界】。
 * 真正的阻塞读在 reader_thread 里，这里一秒都不会被设备卡住。 */
static int ce_read(void *ctx, unsigned char *buf, int len, int timeout_ms)
{
    DWORD t0;
    int   n;
    (void)ctx;

    if (g_h == INVALID_HANDLE_VALUE)
        return -1;
    if (len <= 0)
        return 0;

    t0 = GetTickCount();

    for (;;) {
        n = ring_get(buf, len);
        if (n > 0) {
            g_in_bytes += (unsigned long)n;
            return n;
        }

        /* 读线程挂了：如实告诉上层（它才知道「设备真的没了」） */
        if (g_rdead)
            return g_fatal ? -1 : 0;

        if (timeout_ms <= 0)
            return 0;                      /* 非阻塞语义：现在没数据就到这 */

        {
            int el = (int)(GetTickCount() - t0);
            int left;

            if (el >= timeout_ms)
                return 0;                  /* 超时 */

            /* 等「有数据」事件，但一次最多等 50 毫秒 ——
             * 这样读线程万一挂了也能很快被发现，不会傻等到超时。 */
            left = timeout_ms - el;
            if (left > 50) left = 50;
            if (g_rev)
                WaitForSingleObject(g_rev, (DWORD)left);
            else
                Sleep((DWORD)left);

            if (ring_get(buf, len) == 0)
                g_no_data++;               /* 记一笔「等了个空」 */
        }
    }
}

static int ce_write(void *ctx, const unsigned char *buf, int len)
{
    DWORD put = 0;
    int   tries = 0;
    (void)ctx;

    if (g_h == INVALID_HANDLE_VALUE)
        return -1;

    /* ⚠ WriteFile 会「成功但写 0 字节」——
     *   现场日志里那句「写失败 0 次」却每次秒退 -1 就是这个：
     *   驱动内部的发送缓冲满了，它不报错，只是不接收。
     *   以前一次不成就当失败返回 -1，于是上层看到的是一连串莫名其妙的失败。
     *   现在给它一点时间，最多重试 20 次（≈100 毫秒），并把「0 字节」单独计数。 */
    for (tries = 0; tries < 20; tries++) {
        put = 0;
        if (!WriteFile(g_h, (LPVOID)buf, (DWORD)len, &put, NULL)) {
            DWORD e = GetLastError();
            g_last_err = e;
            g_err_write++;
            if (err_is_fatal(e))
                g_fatal = 1;
            return -1;
        }
        if (put > 0) {
            g_out_bytes += put;
            return (int)put;
        }
        g_wr_zero++;                       /* 写进去了 0 字节，等一等再来 */
        if (len > 0)
            Sleep(5);
    }
    g_err_write++;
    return -1;
}

ADB_IO adbio_ce_io(void)
{
    ADB_IO io;
    io.read  = ce_read;
    io.write = ce_write;
    io.ctx   = 0;
    return io;
}

/* ── 注册表小工具（诊断用）── */
static int reg_has_key(const WCHAR *path)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    RegCloseKey(k);
    return 1;
}

static int reg_get_str(const WCHAR *path, const WCHAR *val, char *out, int cap)
{
    HKEY  k;
    DWORD cb, type = 0;
    WCHAR buf[256];
    int   i;

    if (!out || cap <= 0) return -1;
    out[0] = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return -1;
    buf[0] = 0;
    cb = (DWORD)(sizeof(buf) - sizeof(WCHAR));
    if (RegQueryValueExW(k, val, 0, &type, (BYTE *)buf, &cb) != ERROR_SUCCESS) {
        RegCloseKey(k);
        return -1;
    }
    RegCloseKey(k);
    for (i = 0; i < cap - 1 && buf[i] && i < 255; i++)
        out[i] = (char)(buf[i] & 0xFF);        /* 驱动名/前缀都是 ASCII */
    out[i] = 0;
    return i > 0 ? 0 : -1;
}

static void report_key(void (*emit)(const char *line), const char *label,
                       const WCHAR *path, int with_values)
{
    char line[300], v[220];
    int  has = reg_has_key(path);

    snprintf(line, sizeof(line), "   %s : %s", label, has ? "有" : "没有");
    emit(line);
    if (!has || !with_values) return;

    if (reg_get_str(path, L"Dll", v, (int)sizeof(v)) == 0) {
        snprintf(line, sizeof(line), "      Dll = %s", v);
        emit(line);
    }
    if (reg_get_str(path, L"Prefix", v, (int)sizeof(v)) == 0) {
        snprintf(line, sizeof(line), "      Prefix = %s", v);
        emit(line);
    }
    if (reg_get_str(path, L"Order", v, (int)sizeof(v)) == 0) {
        snprintf(line, sizeof(line), "      Order = %s", v);
        emit(line);
    }
}

/* 把 USB/ADB 的注册表现状逐行交出去。
 * 这一份是判断「ADB1: 为什么打不开」的根：驱动没注册 = 错误 55 的由来。 */
void adbio_ce_dump_usb_state(void (*emit)(const char *line))
{
    static char devs[1024];
    char line[1100];

    if (!emit) return;

    emit("   ── ADB 驱动注册表（HKLM\\Drivers\\USB）──");
    report_key(emit, "ClientDrivers", L"Drivers\\USB\\ClientDrivers", 0);
    report_key(emit, "ClientDrivers\\ADB_Class",
               L"Drivers\\USB\\ClientDrivers\\ADB_Class", 1);
    report_key(emit, "ClientDrivers\\ADB_Driver",
               L"Drivers\\USB\\ClientDrivers\\ADB_Driver", 1);
    report_key(emit, "LoadClients", L"Drivers\\USB\\LoadClients", 0);
    report_key(emit, "LoadClients\\Default\\Default",
               L"Drivers\\USB\\LoadClients\\Default\\Default", 0);
    report_key(emit, "LoadClients\\..\\255_66_1",
               L"Drivers\\USB\\LoadClients\\Default\\Default\\255_66_1", 0);
    report_key(emit, "LoadClients\\..\\255_66_1\\ADB_Class",
               L"Drivers\\USB\\LoadClients\\Default\\Default\\255_66_1\\ADB_Class", 1);

    emit("   ── 已加载的设备名（HKLM\\Drivers\\Active）──");
    devs[0] = 0;
    if (adbio_ce_list_devices(devs, (int)sizeof(devs)) > 0) {
        snprintf(line, sizeof(line), "   %s", devs);
        emit(line);
    } else {
        emit("   （一个都读不到）");
    }
}

int adbio_ce_open(WCHAR *out_name, int name_cap, char *reason, int reason_cap)
{
    int i, o = 0;
    char t[256];
    DWORD last = 0;

    if (reason && reason_cap > 0) reason[0] = 0;

    /* ⚠⚠ 上一轮的读线程如果还卡在 ReadFile 里，它退出时会去关【它自己那个】
     *   句柄（g_rh）。这本身是安全的，但这一轮若马上再开一个新句柄，
     *   两者就会同时存在 —— 这个驱动很可能不接受（又会是错误 110 那一套）。
     *   所以先等它收尾，等不到就如实报错，下一轮再来。 */
    if (g_pending_close) {
        int k;
        for (k = 0; k < 50 && g_pending_close; k++)
            Sleep(100);
        if (g_pending_close) {
            if (reason && reason_cap > 0) {
                snprintf(reason, (size_t)reason_cap - 1,
                         "上一个 ADB 句柄还在收尾（读线程未退出），本轮先跳过");
                reason[reason_cap - 1] = 0;
            }
            return -1;
        }
    }

    /* ⚠ 句柄黏住，不先关再开。
     *   现场日志里出现了非常清楚的一段：
     *     19.581 打开成功 → 握手失败 → 关闭
     *     21.6 ~ 56.5  ADB1: 连续 18 次返回 110(ERROR_OPEN_FAILED)
     *     58.576 又能打开了
     *   也就是这个驱动的「关」是脏的，关一次要几十秒才缓过来；
     *   而我们每 2 秒就给它来一轮开关，等于一直把它按在坏状态里。
     *   能复用就复用。真需要重开时走 adbio_ce_reopen()。 */
    if (g_h != INVALID_HANDLE_VALUE) {
        if (out_name && name_cap > 0)
            w_copy(out_name, g_name, name_cap);
        if (reason && reason_cap > 0)
            sprintf(reason, "复用已打开的 ADB 设备 %ls", g_name);
        return 0;
    }

    g_fatal = 0;

    for (i = 0; i < (int)(sizeof(g_cands) / sizeof(g_cands[0])); i++) {
        HANDLE h = CreateFileW(g_cands[i], GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);
        /* ⚠ 错误码必须【紧接着】CreateFileW 取出来。
         *   原来是在下面 cl_log(...) 之后才 GetLastError()，
         *   而 cl_log 自己会做 WriteFile/FlushFileBuffers ——
         *   它把这些错误码全冲掉了。现场日志里那一行
         *   「试过：ADB1:(错误258) ADB0:(错误258) …」六个全一样，
         *   正是这么来的：258 是刷盘的错，不是 CreateFile 的错。 */
        DWORD  cerr = (h != INVALID_HANDLE_VALUE) ? 0 : GetLastError();
        {
            /* 把「试了哪个 ADB 设备名、成没成」记进日志 ——
             * 这是车机上最容易卡住的一步，而光看屏幕只知道"打不开"。 */
            char nm[32];
            int q;
            for (q = 0; q < 31 && g_cands[i][q]; q++)
                nm[q] = (char)g_cands[i][q];
            nm[q] = 0;
            if (h != INVALID_HANDLE_VALUE)
                cl_log("ADB 设备已打开: %s", nm);
            else
                cl_log("ADB 设备 %s 打不开（错误 %lu）", nm,
                       (unsigned long)cerr);
        }
        if (h != INVALID_HANDLE_VALUE) {
            g_h = h;
            w_copy(g_name, g_cands[i], 64);
            /* 读线程从这里开始接管设备。注意 Sleep 要放在它后面：
             * 让线程先把「第一次阻塞读」发出去，我们再往下走。 */
            reader_start();
            /* 刚打开时给驱动一点时间把 USB 管道挂好。
             * 被 USB 枚举/管道建立挡掉第一包是这类驱动常见的小毛病，
             * 代价只有 150 毫秒，而且只在新开的那一次付。 */
            Sleep(150);
            if (out_name && name_cap > 0)
                w_copy(out_name, g_cands[i], name_cap);
            if (reason && reason_cap > 0)
                sprintf(reason, "已打开 ADB 设备 %ls", g_cands[i]);
            return 0;
        }
        last = cerr;
        {
            int left = (int)sizeof(t) - o;
            if (left <= 0) break;
            o += snprintf(t + o, (size_t)left, "%ls(错误%lu) ",
                          g_cands[i], (unsigned long)last);
        }
    }

    if (reason && reason_cap > 0) {
        snprintf(reason, (size_t)reason_cap - 1,
                 "打不开任何 ADB 设备，试过：%s", o > 0 ? t : "(无)");
        reason[reason_cap - 1] = 0;
    }
    return -1;
}

void adbio_ce_close(void)
{
    /* ⚠ 顺序不能反：必须先让读线程退出，再关句柄。
     *   读线程可能正阻塞在 ReadFile 里（这台驱动就是这样），
     *   关掉一个正在被读的句柄是未定义行为。
     *   reader_stop 只等 2 秒；等不到就把句柄交给线程自己收尾，
     *   宁可暂时留着这个句柄，也不去做危险的动作。 */
    if (g_rthr) {
        reader_stop();
        if (!g_rdead) {
            /* 线程还卡在 ReadFile 里 —— 句柄先不关，标记成「待关」，
             * 由线程退出时自己 CloseHandle，并在这里把逻辑句柄清掉。 */
            g_pending_close = 1;
        }
    }

    if (!g_pending_close) {
        if (g_h != INVALID_HANDLE_VALUE)
            CloseHandle(g_h);
        g_h  = INVALID_HANDLE_VALUE;
        g_rh = INVALID_HANDLE_VALUE;
    } else {
        g_h = INVALID_HANDLE_VALUE;    /* 逻辑上已关；真正的关闭交给读线程 */
    }
    g_rdead = 0;
    g_rstop = 1;
    g_name[0] = 0;
    g_fatal = 0;
}

void adbio_ce_reopen(void)
{
    adbio_ce_close();
}

/* 把 HKLM\Drivers\Active 里所有已加载的设备名列出来。
 * 这些名字就是 CreateFile 能用的流接口名 —— 万一 ADB 的设备名很特别，
 * 靠这份清单就能看出来它到底叫什么。 */
int adbio_ce_list_devices(char *out, int cap)
{
    int n = 0, used = 0;
    char t[128];

    if (!out || cap <= 0) return -1;
    out[0] = 0;

    for (;;) {
        WCHAR path[96], sub[16], nm[128];
        HKEY  k;
        DWORD cb = sizeof(nm) - sizeof(WCHAR), type = 0;

        wsprintfW(sub, L"%d", n);
        wsprintfW(path, L"Drivers\\Active\\%s", sub);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
            break;

        nm[0] = 0;
        if (RegQueryValueExW(k, L"Name", 0, &type, (BYTE *)nm, &cb) == ERROR_SUCCESS) {
            char a[128];
            int i;
            for (i = 0; i < 127 && nm[i]; i++)
                a[i] = (char)(nm[i] & 0xFF);      /* 设备名都是 ASCII */
            a[i] = 0;
            if (i > 0) {
                int w = snprintf(t, sizeof(t), "%s ", a);
                if (w > 0 && used + w < cap - 1) {
                    memcpy(out + used, t, (size_t)w);
                    used += w;
                    out[used] = 0;
                }
            }
        }
        RegCloseKey(k);
        n++;
        if (n > 200) break;
    }
    return used;
}
