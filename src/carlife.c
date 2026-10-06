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

/* 通用发包：按 hdr_len 决定用 8 字节还是 12 字节包头。
 *   hdr_len == 8  : [0..1]size(BE16) [2..3]保留   [4..7] 消息ID(BE32)
 *   hdr_len == 12 : [0..3]size(BE32) [4..7]时间戳 [8..11]消息ID(BE32) */
static int send_packet(SOCKET s, unsigned long msg_id,
                       const unsigned char *body, int body_len, int hdr_len)
{
    unsigned char hdr[CL_HDR_MEDIA];
    int r;

    memset(hdr, 0, sizeof(hdr));
    if (hdr_len == CL_HDR_MEDIA) {
        put_be32(hdr, (unsigned long)body_len);
        put_be32(hdr + 8, msg_id);
    } else {
        put_be16(hdr, (unsigned int)body_len);
        put_be32(hdr + 4, msg_id);
    }

    r = send_all(s, hdr, hdr_len, 3000);
    if (r != CL_OK)
        return r;
    if (body_len > 0) {
        r = send_all(s, body, body_len, 3000);
        if (r != CL_OK)
            return r;
    }
    return CL_OK;
}

int cl_send_cmd(SOCKET s, unsigned long msg_id,
                const unsigned char *body, int body_len)
{
    return send_packet(s, msg_id, body, body_len, CL_HDR_CMD);
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

/* ══ protobuf 最小编码器 ══
 * CarLife 的消息体是 protobuf，但本阶段用到的报文都很简单，
 * 手写 varint 即可，不必引入 protobuf 库。
 *   tag 字节 = (字段号 << 3) | 线格式 ；varint(int32) 的线格式 = 0 */
static int pb_varint(unsigned char *out, unsigned long v)
{
    int n = 0;
    do {
        unsigned char b = (unsigned char)(v & 0x7f);
        v >>= 7;
        if (v)
            b |= 0x80;
        out[n++] = b;
    } while (v && n < 10);
    return n;
}

static int pb_int32(unsigned char *out, int field, int value)
{
    int n = 0;
    out[n++] = (unsigned char)((field << 3) & 0xff);
    n += pb_varint(out + n, (unsigned long)value);
    return n;
}

/* CarlifeVideoEncoderInfo{width=1, height=2, frameRate=3} —— 全是 required int32 */
int cl_send_video_encoder_init(SOCKET s, int w, int h, int fps)
{
    unsigned char body[32];
    int n = 0;

    n += pb_int32(body + n, 1, w);
    n += pb_int32(body + n, 2, h);
    n += pb_int32(body + n, 3, fps);

    return cl_send_cmd(s, CL_MSG_VIDEO_ENCODER_INIT, body, n);
}

/* VIDEO_ENCODER_START 没有参数，数据体为空 */
int cl_send_video_encoder_start(SOCKET s)
{
    return cl_send_cmd(s, CL_MSG_VIDEO_ENCODER_START, (const unsigned char *)0, 0);
}

int cl_recv_video(SOCKET s, unsigned long *timestamp, unsigned long *vtype,
                  unsigned char *buf, int cap, int *len, int timeout_ms)
{
    unsigned char hdr[CL_HDR_MEDIA];
    int want, r;

    r = recv_all(s, hdr, CL_HDR_MEDIA, timeout_ms);
    if (r != CL_OK)
        return r;

    want = (int)get_be32(hdr);
    if (timestamp)
        *timestamp = get_be32(hdr + 4);
    if (vtype)
        *vtype = get_be32(hdr + 8);

    if (want > cap)
        want = cap;
    if (want < 0)
        want = 0;

    if (want > 0) {
        r = recv_all(s, buf, want, timeout_ms);
        if (r != CL_OK)
            return r;
    }
    *len = want;
    return CL_OK;
}

/* JPEG 请求无参数 */
int cl_send_video_encoder_jpeg(SOCKET s)
{
    return cl_send_cmd(s, CL_MSG_VIDEO_ENCODER_JPEG, (const unsigned char *)0, 0);
}

/* 靠帧头签名猜编码格式 —— 现场最便宜也最有效的诊断 */
const WCHAR *cl_guess_codec(const unsigned char *p, int len)
{
    if (len >= 2 && p[0] == 0xff && p[1] == 0xd8)
        return L"JPEG  (FFD8)";
    if (len >= 4 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x01)
        return L"H.264 (00000001)";
    if (len >= 3 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x01)
        return L"H.264 (000001)";
    if (len >= 4 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x00)
        return L"全零(可能无数据)";
    return L"未知格式";
}

/* CarlifeTouchAction{action=1, x=2, y=3} —— 全是 int32 */
int cl_send_touch_action(SOCKET s, int action, int x, int y, int hdr_len)
{
    unsigned char body[32];
    int n = 0;

    n += pb_int32(body + n, 1, action);
    n += pb_int32(body + n, 2, x);
    n += pb_int32(body + n, 3, y);

    return send_packet(s, CL_MSG_TOUCH_ACTION, body, n, hdr_len);
}

/* CarlifeCarHardKeyCode{keycode=1} */
int cl_send_hard_key(SOCKET s, int keycode, int hdr_len)
{
    unsigned char body[16];
    int n = 0;

    n += pb_int32(body + n, 1, keycode);
    return send_packet(s, CL_MSG_TOUCH_CAR_HARD_KEY, body, n, hdr_len);
}

/* 生成候选手机地址：本机同网段 .1/.129/.100 + 常见 USB 网络共享地址。
 * 返回写入 out 的个数。调用前必须先 WSAStartup。 */
int cl_candidate_ips(unsigned long *out, int max)
{
    char host[128];
    struct hostent *he;
    int n = 0, i;
    static const unsigned char tail[3] = { 1, 129, 100 };
    static const unsigned char fix[6][4] = {
        { 192, 168,  42, 129 }, { 192, 168,  42,   1 }, { 192, 168, 43,   1 },
        { 192, 168, 137,   1 }, { 192, 168,   0,   1 }, { 192, 168,  1,   1 }
    };

    host[0] = 0;
    if (gethostname(host, sizeof(host)) != 0)
        host[0] = 0;

    if (host[0]) {
        he = gethostbyname(host);
        if (he && he->h_addr_list && he->h_addr_list[0]) {
            unsigned char b[4];
            memcpy(b, he->h_addr_list[0], 4);
            for (i = 0; i < 3 && n < max; i++) {
                unsigned long c;
                b[3] = tail[i];
                memcpy(&c, b, 4);
                out[n++] = c;
            }
        }
    }
    for (i = 0; i < 6 && n < max; i++) {
        unsigned long c;
        memcpy(&c, fix[i], 4);
        out[n++] = c;
    }
    return n;
}
