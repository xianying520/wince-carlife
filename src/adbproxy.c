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
#include "adbio_ce.h"

#include <windows.h>
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
static HANDLE    g_thread;
static DWORD     g_tid;
static volatile int g_stop;
static volatile int g_running;
static char      g_status[256];
static unsigned long g_tx_bytes, g_rx_bytes;

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

static DWORD WINAPI adbp_thread(LPVOID arg)
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
                    g_conn[slot].chan = adb_open(&g_adb, g_svc[i].service);
                    if (g_conn[slot].chan < 0) {
                        closesocket(c);
                        g_conn[slot].used = 0;
                        g_conn[slot].sock = INVALID_SOCKET;
                        set_status("转发 %s 失败（手机拒绝或通道已满）",
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

int adbp_start(const char * const *services, int n_services,
               unsigned short *local_ports, char *reason, int reason_cap)
{
    WSADATA wsa;
    ADB_IO  io;
    WCHAR   devname[64];
    char    devreason[256];
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

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (reason) snprintf(reason, (size_t)reason_cap, "Winsock 初始化失败");
        return -1;
    }
    if (n_services <= 0 || n_services > ADBP_MAX_SVC) {
        if (reason) snprintf(reason, (size_t)reason_cap, "服务数量不对");
        return -2;
    }

    /* ① 打开 ADB 设备 */
    if (adbio_ce_open(devname, 64, devreason, (int)sizeof(devreason)) != 0) {
        if (reason) {
            snprintf(reason, (size_t)reason_cap, "%s", devreason);
            reason[reason_cap - 1] = 0;
        }
        return -3;
    }

    /* ② 连上并完成认证 */
    io = adbio_ce_io();
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
                             "ADB 认证失败（错误 %d，设备 %ls）", r, devname);
                reason[reason_cap - 1] = 0;
            }
            adbio_ce_close();
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
    g_thread = CreateThread(NULL, 0, adbp_thread, NULL, 0, &g_tid);
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
    adbio_ce_close();
    g_nsvc = 0;
    return -5;
}

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
    adbio_ce_close();
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
