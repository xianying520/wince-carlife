/* carlife.c — CarLife 车机端协议层实现（最小集：连接 + 包头 + 握手）
 *
 * 包头格式（已从参考实现两侧代码确认，全部大端序 BE）：
 *   CMD/CTRL 通道 8 字节：
 *     [0..1] size(BE16)  [2..3] 保留  [4..7] 消息ID(BE32)
 *   其余通道 12 字节：
 *     [0..3] size(BE32)  [4..7] 时间戳(BE32)  [8..11] 消息ID(BE32)
 *
 * 握手报文是 protobuf 编码，但极简单，手写即可，不需要 protobuf 库：
 *   CarlifeProtocolVersion{majorVersion=1, minorVersion=0}
 *     => 08 01 10 00        (tag=(1<<3)|0=0x08, tag=(2<<3)|0=0x10)
 *   CarlifeProtocolVersionMatchStatus{matchStatus=N}
 *     => 08 <N>
 */

#include "carlife.h"
#include <string.h>

static void put_be16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)((v >> 8) & 0xff);
    p[1] = (unsigned char)(v & 0xff);
}

static void put_be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xff);
    p[1] = (unsigned char)((v >> 16) & 0xff);
    p[2] = (unsigned char)((v >> 8) & 0xff);
    p[3] = (unsigned char)(v & 0xff);
}

static unsigned int get_be16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

static unsigned long get_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
}

SOCKET cl_connect(unsigned long ip_be, int port, int timeout_ms)
{
    SOCKET s;
    struct sockaddr_in sa;
    struct timeval tv;
    fd_set wf;
    u_long nb = 1;
    int r;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;

    ioctlsocket(s, FIONBIO, &nb);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons((unsigned short)port);
    sa.sin_addr.s_addr = ip_be;

    r = connect(s, (struct sockaddr *)&sa, sizeof(sa));
    if (r == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        r = select(0, NULL, &wf, NULL, &tv);
        if (r <= 0) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    /* 转回阻塞模式，后续收发按阻塞 + select 超时处理 */
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    return s;
}

/* 阻塞发送，带 select 超时，保证整段发完 */
static int send_all(SOCKET s, const unsigned char *p, int len, int timeout_ms)
{
    int sent = 0;
    while (sent < len) {
        fd_set wf;
        struct timeval tv;
        int r;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        r = select(0, NULL, &wf, NULL, &tv);
        if (r <= 0)
            return CL_ERR_TIMEOUT;
        r = send(s, (const char *)(p + sent), len - sent, 0);
        if (r <= 0)
            return CL_ERR_SEND;
        sent += r;
    }
    return CL_OK;
}

/* 阻塞接收恰好 len 字节，带 select 超时 */
static int recv_all(SOCKET s, unsigned char *p, int len, int timeout_ms)
{
    int got = 0;
    while (got < len) {
        fd_set rf;
        struct timeval tv;
        int r;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        r = select(0, &rf, NULL, NULL, &tv);
        if (r <= 0)
            return CL_ERR_TIMEOUT;
        r = recv(s, (char *)(p + got), len - got, 0);
        if (r <= 0)
            return CL_ERR_RECV;
        got += r;
    }
    return CL_OK;
}

int cl_send_cmd(SOCKET s, unsigned long msg_id,
                const unsigned char *body, int body_len)
{
    unsigned char hdr[CL_HDR_CMD];
    int r;

    put_be16(hdr, (unsigned int)body_len);
    hdr[2] = 0;
    hdr[3] = 0;
    put_be32(hdr + 4, msg_id);

    r = send_all(s, hdr, CL_HDR_CMD, 3000);
    if (r != CL_OK)
        return r;
    if (body_len > 0) {
        r = send_all(s, body, body_len, 3000);
        if (r != CL_OK)
            return r;
    }
    return CL_OK;
}

int cl_recv_cmd(SOCKET s, unsigned long *msg_id,
                unsigned char *body, int cap, int *body_len, int timeout_ms)
{
    unsigned char hdr[CL_HDR_CMD];
    int want, r;

    r = recv_all(s, hdr, CL_HDR_CMD, timeout_ms);
    if (r != CL_OK)
        return r;

    *msg_id = get_be32(hdr + 4);
    want = (int)get_be16(hdr);
    if (want > cap)
        want = cap;
    if (want < 0)
        want = 0;

    if (want > 0) {
        r = recv_all(s, body, want, timeout_ms);
        if (r != CL_OK)
            return r;
    }
    *body_len = want;
    return CL_OK;
}

int cl_handshake(SOCKET s, int *match_status, unsigned long *reply_id)
{
    /* CarlifeProtocolVersion{majorVersion=1, minorVersion=0} */
    unsigned char body[4];
    unsigned char rbuf[256];
    unsigned long msg_id = 0;
    int blen = 0, r;

    body[0] = 0x08;
    body[1] = (unsigned char)CL_VER_MAJOR;
    body[2] = 0x10;
    body[3] = (unsigned char)CL_VER_MINOR;

    r = cl_send_cmd(s, CL_MSG_HU_PROTOCOL_VERSION, body, 4);
    if (r != CL_OK)
        return r;

    r = cl_recv_cmd(s, &msg_id, rbuf, (int)sizeof(rbuf), &blen, 4000);
    if (r != CL_OK)
        return r;

    if (reply_id)
        *reply_id = msg_id;

    /* 期望 CarlifeProtocolVersionMatchStatus{matchStatus=N} => 08 <N> */
    if (match_status) {
        *match_status = -1;
        if (blen >= 2 && rbuf[0] == 0x08)
            *match_status = (int)rbuf[1];
        else if (blen >= 1)
            *match_status = (int)rbuf[0];
    }
    return CL_OK;
}
