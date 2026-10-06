/* test_adb.c — 主机上跑真实的 adb.c，连 Python 写的假 adbd。
 *
 * 读写用 socket 接进去（车机上接的是 ADB1: 设备），所以被测的是同一份协议代码。
 */
#include "clhost.h"
#include "../src/adb.h"

#include <signal.h>
#include <sys/socket.h>

static int g_fd = -1;

static int io_read(void *ctx, unsigned char *buf, int len, int timeout_ms)
{
    int fd = *(int *)ctx;
    fd_set rf;
    struct timeval tv;
    int r;

    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    r = select(fd + 1, &rf, NULL, NULL, &tv);
    if (r <= 0)
        return 0;                      /* 超时：ADB 语义里 0 就是超时 */
    r = (int)recv(fd, (char *)buf, (size_t)len, 0);
    if (r <= 0)
        return -1;                     /* 对端关了或出错 */
    return r;
}

static int io_write(void *ctx, const unsigned char *buf, int len)
{
    int fd = *(int *)ctx;
    int n = (int)send(fd, (const char *)buf, (size_t)len, 0);
    return n <= 0 ? -1 : n;
}

int main(int argc, char **argv)
{
    ADB      a;
    ADB_IO   io;
    struct sockaddr_in sa;
    int      port = argc > 1 ? atoi(argv[1]) : 7998;
    int      id, r, i, got = 0;
    unsigned char rx[512];
    const char *payload = "hello-from-hu";

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);

    g_fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(0x7F000001UL);

    if (connect(g_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        printf("CONNECT_FAIL errno=%d\n", errno);
        return 1;
    }

    io.read  = io_read;
    io.write = io_write;
    io.ctx   = &g_fd;

    printf("══ 用真实 adb.c 连假 adbd ══\n");

    r = adb_connect(&a, io);
    printf("AUTH_RESULT=%d\n", r);
    if (r != 0) {
        printf("认证失败，后面的不用测了\n");
        return 1;
    }
    printf("MAXDATA=%u\n", adb_maxdata(&a));

    id = adb_open(&a, "tcp:7240");
    printf("OPEN_RESULT=%d\n", id);
    if (id < 0)
        return 1;

    r = adb_send(&a, id, (const unsigned char *)payload, (int)strlen(payload));
    printf("SEND_RESULT=%d\n", r);
    if (r != 0)
        return 1;

    /* 假 adbd 会把收到的内容原样发回来，我们等它到达 */
    for (i = 0; i < 20 && got == 0; i++) {
        r = adb_pump(&a, 500);
        if (r < 0) { printf("PUMP_ERR=%d\n", r); break; }
        r = adb_recv(&a, id, rx, (int)sizeof(rx) - 1);
        if (r > 0) { got = r; break; }
    }

    if (got > 0) {
        rx[got] = 0;
        printf("ECHO=%s\n", (char *)rx);
        printf("ECHO_LEN=%d\n", got);
    } else {
        printf("ECHO=none\n");
    }

    fflush(stdout);
    return got > 0 ? 0 : 1;
}
