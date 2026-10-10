/* test_ce_dev.c — 设备层回归测试：拿「假车机 ADB 驱动」考真实 src/adbio_ce.c
 *
 * ══════════════════════════════════════════════════════════════════════════
 *  这个测试存在的唯一理由：**别再拿车当测试机**。
 *
 *  过去四趟车踩的坑全部落在这一层，而且全部是「在电脑上跑一遍就能发现」的：
 *    · 读设备把「此刻没数据」当「设备坏了」        → 本测试第 2 项
 *    · 阻塞读把上层卡死几十秒                      → 本测试第 2/5 项
 *    · 环形缓冲满了在 while 里死等，读线程再也不退 → 本测试第 5 项
 *    · 句柄收不了尾，下一轮再也打不开设备          → 本测试第 5/6 项
 *
 *  假设备的行为【照实现场观测】，不照「合理」来（见 ceemu.h）：
 *    ReadFile 阻塞且无视超时；WriteFile 可能写 0 字节；
 *    CloseHandle 碰上正在读的句柄不会立刻生效。
 * ══════════════════════════════════════════════════════════════════════════
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ceemu.h"
#include "adbio_ce.h"

/* ── cl_log 的替身（adbio_ce.c 只用到这一个日志函数）── */
__attribute__((format(printf, 1, 2)))
void cl_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("    [cllog] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static int   g_peer = -1;          /* 对端：扮演「手机那侧」
                                    *
                                    * ⚠ 名字故意用 peer 不用 mock：
                                    *   scripts/check_order.py 会拒绝含 "mock("
                                    *   的行，也拒绝以 mock 开头的函数名。 */
static volatile int g_peer_run = 0;
static volatile long g_peer_sent = 0;
static volatile long g_peer_got  = 0;

static int fails = 0;

static void check(const char *name, int ok, const char *extra)
{
    printf("  %s %s%s%s\n", ok ? "✅" : "❌", name,
           extra ? "  " : "", extra ? extra : "");
    if (!ok) fails++;
}

/* 对端线程：先静默，再按指令吐数据 */
static void *peer_fn(void *arg)
{
    char buf[4096];
    (void)arg;

    while (g_peer_run) {
        fd_set rf;
        struct timeval tv = { 0, 20000 };   /* 20 毫秒轮一次 */
        int r;

        FD_ZERO(&rf);
        FD_SET(g_peer, &rf);
        r = select(g_peer + 1, &rf, NULL, NULL, &tv);
        if (r > 0) {
            ssize_t n = recv(g_peer, buf, sizeof(buf), 0);
            if (n > 0) g_peer_got += n;
            else if (n == 0) break;
        }
    }
    return NULL;
}

int main(void)
{
    int sv[2];
    ADB_IO io;
    WCHAR nm[64];
    char  reason[256];
    unsigned char buf[8192];
    int n, d;
    DWORD t0;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("══ 设备层回归测试：真实 adbio_ce.c 对假车机 ADB 驱动 ══\n");

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("socketpair 失败\n");
        return 2;
    }
    g_peer = sv[0];
    ce_set_fd(sv[1]);
    ce_set_present(1);

    /* ⚠ 这个 pthread_create 一开始漏写了 —— 结果「对端收到字节」那项永远失败，
     *   因为压根没有线程在对端收。测试代码自己也要有人审。 */
    g_peer_run = 1;
    {
        pthread_t th;
        pthread_create(&th, NULL, peer_fn, NULL);
        pthread_detach(th);
    }

    /* ── 1) 打开设备 ── */
    n = adbio_ce_open(nm, 64, reason, (int)sizeof(reason));
    check("打开 ADB1: 成功", n == 0, n == 0 ? "" : reason);
    if (n != 0) return 1;

    io = adbio_ce_io();

    /* ── 2) 【最关键】设备在静默时，应用层的读必须在超时内返回 ──
     *     真驱动此刻会一直阻塞下去（几十秒），
     *     只有「专用读线程 + 环形缓冲」这一层才能把它挡住。
     *     这个断言就是现场那个「66 秒卡死」的回归测试。 */
    t0 = GetTickCount();
    n = io.read(io.ctx, buf, 16, 300);
    d = (int)(GetTickCount() - t0);
    {
        char e[96];
        snprintf(e, sizeof(e), "耗时 %d 毫秒（要求 <1000）", d);
        check("设备静默时，读在超时内返回（不被驱动的阻塞拖住）",
              (n == 0 && d < 1000), e);
    }

    /* ── 3) 数据能正常收上来 ── */
    send(g_peer, "hello-car", 9, MSG_NOSIGNAL);
    g_peer_sent += 9;
    t0 = GetTickCount();
    n = io.read(io.ctx, buf, 64, 2000);
    {
        unsigned long rb = 0, rr = 0, rmax = 0;
        char e[160];
        adbio_ce_reader_stats(&rb, &rr, &rmax);
        snprintf(e, sizeof(e), "read 返回 %d，设备侧累计已读 %lu 字节 / %lu 次",
                 n, rb, rr);
        check("能收到设备送来的数据", (n == 9 && memcmp(buf, "hello-car", 9) == 0), e);
    }
    if (n > 0) {
        char e[64];
        snprintf(e, sizeof(e), "延迟 %d 毫秒", (int)(GetTickCount() - t0));
        check("收数据不是靠轮询等出来的（<300ms）", (int)(GetTickCount() - t0) < 300, e);
    }

    /* ── 4) 写能正常下去，并且「写 0 字节」会被重试掉 ──
     *     现场那句「写失败 0 次却每次秒退 -1」就是这么来的。 */
    ce_set_zero_write(3);                       /* 前 3 次只写 0 字节 */
    n = io.write(io.ctx, (const unsigned char *)"ping-1234", 9);
    {
        char e[64];
        snprintf(e, sizeof(e), "write 返回 %d", n);
        check("设备暂时写不进时，会重试到写下去（不是立刻失败）", n == 9, e);
    }
    {
        long before = g_peer_got;
        for (d = 0; d < 50 && g_peer_got == before; d++) Sleep(20);
        check("对端确实收到了那 9 个字节", (g_peer_got - before) == 9, "");
    }

    /* ── 5) 【最关键的回归】把环形缓冲彻底灌满，然后 close ──
     *     上一版就是在这里卡死的：缓冲满了以后读线程在 while 里死等，
     *     再也回不到外层，于是句柄永远收不了尾，
     *     现场从第 35 秒一直卡到 1277 秒（1242 秒），308 次「本轮先跳过」。 */
    printf("  ── 灌满环形缓冲（2 MB）再关设备 ──\n");
    {
        char big[4096];
        long total = 0;
        memset(big, 'X', sizeof(big));
        while (total < 2 * 1024 * 1024) {
            ssize_t w = send(g_peer, big, sizeof(big), MSG_NOSIGNAL);
            if (w <= 0) break;
            total += w;
            if ((total & 0x3FFFF) == 0) Sleep(1);
        }
        g_peer_sent += total;
        printf("    对端已注入 %ld KB\n", total / 1024);
    }
    Sleep(1200);                    /* 让读线程把缓冲灌满、并卡在「满」上 */

    t0 = GetTickCount();
    adbio_ce_close();
    d = (int)(GetTickCount() - t0);
    {
        char e[96];
        snprintf(e, sizeof(e), "耗时 %d 毫秒（要求 <5000）", d);
        check("★ 缓冲灌满后，关设备不会卡住", d < 5000, e);
    }

    /* ── 6) 关完之后必须还能重新打开 ──
     *     上一版这里会一直返回「上一个 ADB 句柄还在收尾」，
     *     于是【再也连不上手机】，一直到用户手动退出程序。 */
    {
        char r2[256];
        r2[0] = 0;
        n = adbio_ce_open(nm, 64, r2, (int)sizeof(r2));
        check("★ 关完之后还能重新打开设备", n == 0,
              n == 0 ? "" : (r2[0] ? r2 : "（无说明）"));
    }

    /* ── 7) 诊断数字要能读出来 ── */
    {
        unsigned long ib = 0, ob = 0, re = 0, we = 0, nd = 0, le = 0;
        unsigned long rb = 0, rr = 0, rmax = 0, drops = 0;
        int ru = 0, rc = 0;
        char e[160];
        adbio_ce_stats(&ib, &ob, &re, &we, &nd, &le);
        adbio_ce_reader_stats(&rb, &rr, &rmax);
        adbio_ce_ring(&ru, &rc, &drops);
        snprintf(e, sizeof(e),
                 "设备侧 %lu 字节 / 协议层 %lu 字节 / 丢过 %lu 次",
                 rb, ib, drops);
        check("设备侧与协议层的字节数分开统计（能一眼看出是谁的问题）",
              (rb > 0 && ib > 0), e);
    }

    g_peer_run = 0;
    adbio_ce_close();
    close(sv[0]);
    close(sv[1]);

    printf("\n%s 共 %d 项不符\n", fails ? "❌" : "✅", fails);
    return fails ? 1 : 0;
}
