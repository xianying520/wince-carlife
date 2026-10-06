/* 逐步复刻 cl_connect 的每一步并打印，用来定位失败究竟在哪一步。
 * 不改动 carlife.c —— 只在外面复刻同样的调用序列。 */
#include "clhost.h"

#include <sys/socket.h>

int main(int argc, char **argv)
{
    SOCKET s;
    struct sockaddr_in sa;
    struct timeval tv;
    fd_set wf;
    u_long nb = 1;
    int r, soerr = -1;
    socklen_t sl = sizeof(soerr);

    unsigned short port = (unsigned short)(argc > 1 ? atoi(argv[1]) : 7999);

    printf("══ 逐步复刻 cl_connect（端口 %u）══\n", (unsigned)port);

    errno = 0;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    printf("  1) socket() = %d  errno=%d (%s)\n", s, errno, strerror(errno));
    if (s < 0) return 1;

    errno = 0;
    r = ioctlsocket(s, FIONBIO, &nb);
    printf("  2) ioctl(FIONBIO) = %d  errno=%d (%s)  FIONBIO=0x%lx\n",
           r, errno, strerror(errno), (unsigned long)FIONBIO);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(port);
    sa.sin_addr.s_addr = htonl(0x7F000001UL);

    errno = 0;
    r = connect(s, (struct sockaddr *)&sa, sizeof(sa));
    printf("  3) connect() = %d  errno=%d (%s)\n", r, errno, strerror(errno));
    printf("     WSAGetLastError() = %d\n", WSAGetLastError());
    printf("     WSAEWOULDBLOCK    = %d\n", WSAEWOULDBLOCK);
    printf("     两者相等？ %s  ← 这是决定【是否继续等待】的关键判断\n",
           WSAGetLastError() == WSAEWOULDBLOCK ? "是（会继续 select）" : "否（会被当成真失败）");

    if (r == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        tv.tv_sec  = 2;
        tv.tv_usec = 0;
        errno = 0;
        r = select((int)s + 1, NULL, &wf, NULL, &tv);   /* 修复后：nfds 必须正确 */
        printf("  4) select() = %d  errno=%d (%s)\n", r, errno, strerror(errno));
        printf("     tv 剩余 = %ld.%06ld\n", (long)tv.tv_sec, (long)tv.tv_usec);
    }

    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0)
        printf("  5) SO_ERROR = %d (%s)\n", soerr, strerror(soerr));

    close(s);
    return 0;
}
