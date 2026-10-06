/* test_adbproxy.c — 用真实转发器代码 + 假手机，在电脑上端到端验证
 *
 * 做法：ADB 设备这一侧用一条普通 socket 顶替（转发器的设备读写本来就是
 * 函数指针，所以不用改业务代码）。然后本地连上转发出来的端口，像 CarLife
 * 客户端那样发数据，检查它有没有被原样送到"手机"、手机的回应有没有被送回来。
 *
 * 为什么值得做：转发器的核心是一个 select 循环，光读代码看不出事件漏收、
 * 通道关闭卡死、以及"手机端没起来时自动拉起再重试"这条路径对不对。
 */
#include "clhost.h"
#include "hostplat.h"
#include "adbproxy.h"
#include "adb.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static int g_devfd = -1;

static int dev_read(void *ctx, unsigned char *buf, int len, int timeout_ms)
{
    int fd = *(int *)ctx;
    fd_set rf;
    struct timeval tv;
    int r;

    (void)rf;
    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    r = select(fd + 1, &rf, NULL, NULL, &tv);
    if (r == 0) return 0;                       /* 超时 */
    if (r < 0)  return -1;
    r = (int)recv(fd, buf, (size_t)len, 0);
    if (r == 0) return -1;                      /* 对端关了 */
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    return r;
}

static int dev_write(void *ctx, const unsigned char *buf, int len)
{
    int fd = *(int *)ctx, off = 0;
    while (off < len) {
        int r = (int)send(fd, buf + off, (size_t)(len - off), 0);
        if (r <= 0) return -1;
        off += r;
    }
    return off;
}

static int connect_local(int port)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(0x7F000001UL);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    /* 接收超时：万一"手机"不回话，测试要失败退出，不能把 CI 挂死 */
    {
        struct timeval tv;
        tv.tv_sec  = 20;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    return fd;
}

int main(int argc, char **argv)
{
    static const char *svc[3] = { "tcp:7240", "tcp:8240", "tcp:9340" };
    unsigned short lports[3];
    char reason[256];
    struct sockaddr_in sa;
    ADB_IO io;
    int r, fd;

    /* 输出必须不缓冲：否则中途出错时看不到已经打出来的东西 */
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) { printf("用法: %s <假手机端口>\n", argv[0]); return 2; }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons((unsigned short)atoi(argv[1]));
    sa.sin_addr.s_addr = htonl(0x7F000001UL);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        printf("DEV_CONNECT=失败\n");
        return 1;
    }
    g_devfd = fd;

    io.read  = dev_read;
    io.write = dev_write;
    io.ctx   = &g_devfd;

    memset(lports, 0, sizeof(lports));
    r = adbp_start_with_io(io, svc, 3, lports, reason, (int)sizeof(reason));
    printf("ADBP_START=%d\n", r);
    if (r != 0) {
        printf("REASON=%s\n", reason);
        return 1;
    }
    printf("LPORT_CMD=%u\n", lports[0]);
    printf("LPORT_VID=%u\n", lports[1]);
    printf("LPORT_TOUCH=%u\n", lports[2]);

    /* 像 CarLife 客户端那样连上被转发出来的控制端口 */
    {
        char buf[256];
        int  cfd = connect_local((int)lports[0]);
        printf("LOCAL_CONNECT=%s\n", cfd >= 0 ? "成功" : "失败");
        if (cfd < 0) { adbp_stop(); return 1; }

        if (send(cfd, "TOUCH-HELLO-FROM-HU", 19, 0) != 19)
            printf("LOCAL_SEND=失败\n");
        else
            printf("LOCAL_SEND=成功\n");

        /* 等"手机"把内容回显回来 —— 这条路径就是 CarLife 触摸回传的完整链路 */
        r = (int)recv(cfd, buf, sizeof(buf) - 1, 0);
        if (r > 0) {
            buf[r] = 0;
            printf("LOCAL_RECV=%s\n", buf);
            printf("LOCAL_RECV_LEN=%d\n", r);
        } else {
            printf("LOCAL_RECV=无\n");
        }
        close(cfd);
    }

    {
        /* 直接拿同一份清单调一次查找函数，把"是查找错了还是流程没走到"
         * 这件事一刀切开 */
        char pk[208];
        int rf = adb_find_carlife_pkg(adbp_phone_packages(), pk, (int)sizeof(pk));
        printf("DIRECT_FIND=%d\n", rf);
        if (rf == 0) printf("DIRECT_PKG=%s\n", pk);
        printf("LAUNCH_NOTE=%s\n", adbp_last_note());
    }

    {
        const char *pk = adbp_phone_packages();
        int plen = pk ? (int)strlen(pk) : -1;
        printf("PHONE_PKGS_LEN=%d\n", plen);
        if (pk && plen > 0) {
            char head[121];
            int n = plen > 120 ? 120 : plen;
            memcpy(head, pk, (size_t)n);
            head[n] = 0;
            printf("PHONE_PKGS_HEAD=%.120s\n", head);
        }
    }

    {
        unsigned long tx = 0, rx = 0;
        adbp_stats(&tx, &rx);
        printf("STATS_TX=%lu\n", tx);
        printf("STATS_RX=%lu\n", rx);
    }
    printf("转发状态: %s\n", adbp_status());

    adbp_stop();
    close(fd);
    printf("DONE\n");
    return 0;
}
