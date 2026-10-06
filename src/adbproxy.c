/* adbproxy.c — 见 adbproxy.h。
 *
 * 架构上只有一个线程碰 ADB 设备。为什么必须这样：
 * 设备是一个独占的字节流，而 ADB 是严格应答式的（发完 WRTE 必须等 OKAY，
 * 等 OKAY 的过程中又会读到别的通道推来的数据）。如果多个线程各读各的，
 * 就会互相把对方的包吃掉 —— 这种竞态极难查。所以干脆让一个线程全权负责
 * 设备读写，其余部分都用非阻塞 socket，同一个循环里驱动两边。
 */
#include "adbproxy.h"
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

/* 关设备：主机测试下没有设备可关，包一层以免引用到 WinCE 的符号。 */
#ifdef ADBP_HOST_TEST
#define ADBP_CLOSE_DEVICE()   do { g_own_device = 0; } while (0)
#else
#define ADBP_CLOSE_DEVICE()   do {                                     \
        ADBP_CLOSE_DEVICE();      \
    } while (0)
#endif
static volatile int g_stop;
static volatile int g_running;
static char      g_status[256];
static unsigned long g_tx_bytes, g_rx_bytes;
static char      g_pkgs[2048];        /* 手机包名清单（截断保留） */
static int       g_tried_launch;      /* 只尝试拉起一次，避免反复折腾 */

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(g_status, sizeof(g_status) - 1, fmt, ap);
    va_end(ap);
    g_status[sizeof(g_status) - 1] = 0;
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
    if (g_conn[i].chan > 0)
        adb_close_chan(&g_adb, g_conn[i].chan);
    if (g_conn[i].sock != INVALID_SOCKET)
        closesocket(g_conn[i].sock);
    g_conn[i].used = 0;
    g_conn[i].sock = INVALID_SOCKET;
    g_conn[i].chan = 0;
}

static void conn_push_to_phone(int i, const unsigned char *d, int n)
{
    if (adb_send(&g_adb, g_conn[i].chan, d, n) < 0) {
        close_conn(i);
        return;
    }
    g_tx_bytes += (unsigned long)n;
}

/* 转发被拒时，尝试把手机端拉起来。
 * 为什么要做这件事：如果手机上的 CarLife/Jovi InCar 没在运行，手机本地
 * 7240 端口就没人监听，adbd 会直接拒绝我们的 OPEN。EasyConnected 是靠
 * 往手机推一个 carman 程序并执行来解决的；我们用 ADB 的 shell 服务更省事：
 * 先列包名找到 CarLife，再用 monkey 启动它。 */
int adbp_launch_phone_app(char *detail, int cap)
{
    char  buf[8192];
    char  pkg[208];
    char  cmd[400];
    int   r;

    if (detail && cap > 0) detail[0] = 0;

    r = adb_run_shell(&g_adb, "pm list packages", buf, (int)sizeof(buf), 6000);
    if (r != 0) {
        if (detail) snprintf(detail, (size_t)cap, "列包名失败（%d）", r);
        return -1;
    }

    /* 把清单留一份，现场可以直接看手机里 CarLife 叫什么 */
    snprintf(g_pkgs, sizeof(g_pkgs) - 1, "%s", buf);
    g_pkgs[sizeof(g_pkgs) - 1] = 0;

    if (adb_find_carlife_pkg(buf, pkg, (int)sizeof(pkg)) != 0) {
        if (detail)
            snprintf(detail, (size_t)cap,
                     "手机里没找到 CarLife 相关的包（共收到 %d 字节包名清单）",
                     (int)strlen(buf));
        return -1;
    }

    snprintf(cmd, sizeof(cmd),
             "monkey -p %s -c android.intent.category.LAUNCHER 1", pkg);
    buf[0] = 0;
    adb_run_shell(&g_adb, cmd, buf, (int)sizeof(buf), 6000);

    if (detail)
        snprintf(detail, (size_t)cap, "已尝试启动手机端 %s", pkg);
    return 0;
}

const char *adbp_phone_packages(void)
{
    return g_pkgs;
}

/* 打开一条转发；被拒就先试着把手机端拉起来，然后重试一次。
 * 必须由持有设备的那个线程调用（内部会收发 ADB 数据）。 */
static int open_with_retry(const char *service)
{
    int id = adb_open(&g_adb, service);

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
            id = adb_open(&g_adb, service);
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

    if (!buf) {
        set_status("内存不足，转发器无法启动");
        g_stop = 1;
        g_running = 0;
        return 0;
    }

    while (!g_stop) {
        fd_set rf;
        struct timeval tv;
        int    i, r;

        /* ① 先看本地有没有数据要发往手机 */
        FD_ZERO(&rf);
        for (i = 0; i < g_nsvc; i++)
            if (g_svc[i].lsn != INVALID_SOCKET) FD_SET(g_svc[i].lsn, &rf);
        for (i = 0; i < ADBP_MAX_CONN; i++)
            if (g_conn[i].used) FD_SET(g_conn[i].sock, &rf);

        tv.tv_sec = 0;
        tv.tv_usec = 10000;                    /* 10 毫秒一轮 */
        r = select(0, &rf, NULL, NULL, &tv);

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
        r = adb_pump(&g_adb, 0);
        if (r < 0) {
            set_status("ADB 通道断了（错误 %d）", r);
            g_stop = 1;
            break;
        }

        /* ③ 设备→本地 */
        for (i = 0; i < ADBP_MAX_CONN; i++) {
            if (!g_conn[i].used) continue;
            for (;;) {
                int n = adb_recv(&g_adb, g_conn[i].chan, buf, ADBP_BUF);
                if (n <= 0) break;
                if (send(g_conn[i].sock, (const char *)buf, n, 0) <= 0) {
                    close_conn(i);
                    break;
                }
                g_rx_bytes += (unsigned long)n;
            }
            if (g_conn[i].used && adb_chan_closed(&g_adb, g_conn[i].chan))
                close_conn(i);
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
    g_stop = 0;
    g_running = 0;
    g_tx_bytes = g_rx_bytes = 0;
    g_nsvc = 0;

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
        int r = adb_connect(&g_adb, io);
        if (r != 0) {
            if (reason) {
                if (r == -3)
                    snprintf(reason, (size_t)reason_cap,
                             "手机拒绝了 ADB 授权。"
                             "请在手机上点「允许 USB 调试」后重试");
                else
                    snprintf(reason, (size_t)reason_cap,
                             "ADB 认证失败（错误 %d）", r);
                reason[reason_cap - 1] = 0;
            }
            ADBP_CLOSE_DEVICE();
            return -4;
        }
    }

    /* ③ 每个服务：建立本地监听 + 打开一条转发通道 */
    for (i = 0; i < n_services; i++) {
        SOCKET s;
        struct sockaddr_in sa;
        int sl = 0;

        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) {
            if (reason) snprintf(reason, (size_t)reason_cap, "建 socket 失败");
            goto fail;
        }
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port   = 0;                      /* 让系统挑端口，避免撞车 */
        sa.sin_addr.s_addr = htonl(0x7F000001UL);   /* 只听本机 */
        if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            if (reason) snprintf(reason, (size_t)reason_cap, "bind 失败");
            closesocket(s);
            goto fail;
        }
        if (listen(s, 2) != 0) {
            if (reason) snprintf(reason, (size_t)reason_cap, "listen 失败");
            closesocket(s);
            goto fail;
        }
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
    }

    /* ④ 起线程 */
#ifdef ADBP_HOST_TEST
    g_thread = hostplat_spawn(adbp_thread, NULL);
#else
    g_thread = CreateThread(NULL, 0, adbp_thread, NULL, 0, &g_tid);
#endif
    if (!g_thread) {
        if (reason) snprintf(reason, (size_t)reason_cap, "建线程失败");
        goto fail;
    }
    g_running = 1;
    set_status("已连接手机，%d 条端口转发就绪", g_nsvc);
    return 0;

fail:
    for (i = 0; i < ADBP_MAX_SVC; i++) {
        if (g_svc[i].lsn != INVALID_SOCKET) {
            closesocket(g_svc[i].lsn);
            g_svc[i].lsn = INVALID_SOCKET;
        }
        g_svc[i].used = 0;
    }
    ADBP_CLOSE_DEVICE();
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
    ADBP_CLOSE_DEVICE();
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
