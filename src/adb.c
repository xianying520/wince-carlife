/* adb.c — 见 adb.h 的说明。 */
#include "adb.h"
#include "rsa.h"
#include "adbkey.h"

#include <string.h>
#include <stdlib.h>

/* ── 小端打包 / 解包（ADB 全部小端，别和 CarLife 的大端搞混）── */
static void put_le32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static unsigned int get_le32(const unsigned char *p)
{
    return (unsigned int)p[0]
         | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16)
         | ((unsigned int)p[3] << 24);
}

/* 一定要读满 len 字节，否则算失败 —— 流式设备一次读不全很正常 */
static int read_full(ADB *a, unsigned char *buf, int len, int timeout_ms)
{
    int got = 0;
    while (got < len) {
        int r = a->io.read(a->io.ctx, buf + got, len - got, timeout_ms);
        if (r <= 0)
            return -1;
        got += r;
    }
    return got;
}

static int write_full(ADB *a, const unsigned char *buf, int len)
{
    int put = 0;
    while (put < len) {
        int r = a->io.write(a->io.ctx, buf + put, len - put);
        if (r <= 0)
            return -1;
        put += r;
    }
    return put;
}

/* 发一个包。magic = command 取反，这是 ADB 的固定规矩。 */
static int send_msg(ADB *a, unsigned int cmd, unsigned int a0,
                    unsigned int a1, const unsigned char *data, int len)
{
    unsigned char h[24];

    put_le32(h +  0, cmd);
    put_le32(h +  4, a0);
    put_le32(h +  8, a1);
    put_le32(h + 12, (unsigned int)len);
    put_le32(h + 16, 0);                       /* crc：ADB 已经不再校验，填 0 */
    put_le32(h + 20, cmd ^ 0xFFFFFFFFu);       /* magic */

    if (write_full(a, h, 24) < 0)
        return -1;
    if (len > 0 && write_full(a, data, len) < 0)
        return -1;
    return 0;
}

/* 收一个包。data 缓冲由调用者提供，容量 cap；实际长度写回 *len。
 * 载荷比 cap 大时算协议错（正常不会）。 */
static int recv_msg(ADB *a, unsigned int *cmd, unsigned int *a0,
                    unsigned int *a1, unsigned char *data, int cap,
                    int *len, int timeout_ms)
{
    unsigned char h[24];
    unsigned int dlen, magic;

    if (read_full(a, h, 24, timeout_ms) < 0)
        return -1;

    *cmd  = get_le32(h +  0);
    *a0   = get_le32(h +  4);
    *a1   = get_le32(h +  8);
    dlen  = get_le32(h + 12);
    magic = get_le32(h + 20);

    /* magic 校验：能在很大程度上挡掉"设备其实没在说 ADB 协议"这种错，
     * 比如打开的是别的流驱动 —— 那种情况下会看到乱七八糟的字节。 */
    if (magic != (*cmd ^ 0xFFFFFFFFu))
        return -2;
    if ((int)dlen > cap)
        return -2;

    if (dlen > 0 && read_full(a, data, (int)dlen, timeout_ms) < 0)
        return -1;

    *len = (int)dlen;
    return 0;
}

/* ── base64：AUTH 的公钥载荷要用 ── */
static int b64_encode(const unsigned char *in, int n, char *out, int cap)
{
    static const char *T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i = 0, o = 0;

    while (i + 2 < n) {
        unsigned int v = ((unsigned int)in[i] << 16) |
                         ((unsigned int)in[i + 1] << 8) | in[i + 2];
        if (o + 4 >= cap) return -1;
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];
        out[o++] = T[v & 63];
        i += 3;
    }
    if (i < n) {
        unsigned int v = (unsigned int)in[i] << 16;
        int rem = n - i;
        if (rem == 2)
            v |= (unsigned int)in[i + 1] << 8;
        if (o + 4 >= cap) return -1;
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (rem == 2) ? T[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

/* ── 通道缓冲 ── */
static int chan_append(ADB_CHAN *c, const unsigned char *d, int n)
{
    if (n <= 0) return 0;

    /* 先把已取走的部分丢掉，腾出空间 */
    if (c->rx_head > 0) {
        memmove(c->rx, c->rx + c->rx_head, (size_t)(c->rx_len - c->rx_head));
        c->rx_len -= c->rx_head;
        c->rx_head = 0;
    }

    if (c->rx_len + n > c->rx_cap) {
        int ncap = c->rx_cap ? c->rx_cap : 65536;
        unsigned char *nb;
        while (ncap < c->rx_len + n)
            ncap *= 2;
        nb = (unsigned char *)realloc(c->rx, (size_t)ncap);
        if (!nb) return -1;
        c->rx = nb;
        c->rx_cap = ncap;
    }
    memcpy(c->rx + c->rx_len, d, (size_t)n);
    c->rx_len += n;
    return 0;
}

static ADB_CHAN *chan_by_local(ADB *a, unsigned int local_id)
{
    if (local_id < 1 || local_id > ADB_MAX_CHAN)
        return 0;
    if (!a->ch[local_id - 1].used)
        return 0;
    return &a->ch[local_id - 1];
}

/* 处理一个已经收到的包。返回 0 正常，<0 致命。 */
static int handle_msg(ADB *a, unsigned int cmd, unsigned int a0,
                      unsigned int a1, const unsigned char *data, int len)
{
    switch (cmd) {
    case ADB_WRTE: {
        /* arg0 = 对端在这条通道上的 id，arg1 = 我们的 id */
        ADB_CHAN *c = chan_by_local(a, a1);
        if (c)
            chan_append(c, data, len);
        /* 收到就必须回 OKAY，否则对端不会再发 —— ADB 是严格应答式的 */
        send_msg(a, ADB_OKAY, c ? c->remote_id : a0, a1, 0, 0);
        return 0;
    }
    case ADB_CLSE: {
        ADB_CHAN *c = chan_by_local(a, a1);
        if (c)
            c->closed = 1;
        send_msg(a, ADB_CLSE, c ? c->remote_id : a0, a1, 0, 0);
        return 0;
    }
    case ADB_OKAY:
    default:
        return 0;      /* OKAY 由等待方自己消费，这里忽略 */
    }
}

/* 等一个针对指定本地通道的 OKAY。期间可能收到别的包（比如另一条通道
 * 推来的视频帧），必须顺手处理掉，否则会丢数据。 */
static int wait_okay(ADB *a, unsigned int local_id, int timeout_ms)
{
    unsigned int cmd, a0, a1;
    unsigned char buf[ADB_MAXDATA_REQ + 64];
    int len, r;

    for (;;) {
        r = recv_msg(a, &cmd, &a0, &a1, buf, (int)sizeof(buf), &len, timeout_ms);
        if (r < 0)
            return -1;

        if (cmd == ADB_OKAY) {
            if (a1 == local_id)
                return 0;           /* 我们要等的那个 */
            continue;               /* 别人的应答，忽略 */
        }
        if (cmd == ADB_WRTE || cmd == ADB_CLSE) {
            r = handle_msg(a, cmd, a0, a1, buf, len);
            if (r < 0) return -1;
            continue;
        }
        /* 认证阶段之外的 AUTH / 未知命令，当作可忽略 */
    }
}

static int send_rsa_public_key(ADB *a)
{
    unsigned char blob[524];
    char b64[1024];
    char payload[1200];
    int bl, n;

    bl = adb_public_key_blob(ADB_KEY_N, ADB_KEY_E, blob, (int)sizeof(blob));
    if (bl != 524)
        return -1;

    n = b64_encode(blob, bl, b64, (int)sizeof(b64));
    if (n < 0)
        return -1;

    /* 格式与 adb 的公钥文件一致：base64 + 空格 + 名字，末尾补 0。
     * 手机端就是按这个格式解析的，不能省掉末尾的 0。 */
    {
        const char *name = " adbkey@wince";
        int nl = (int)strlen(name);
        if (n + nl + 1 > (int)sizeof(payload))
            return -1;
        memcpy(payload, b64, (size_t)n);
        memcpy(payload + n, name, (size_t)nl);
        payload[n + nl] = 0;
        return send_msg(a, ADB_AUTH, ADB_AUTH_RSAPUBKEY, 0,
                        (const unsigned char *)payload, n + nl + 1);
    }
}

int adb_connect(ADB *a, ADB_IO io)
{
    /* 现代 adbd 的 banner 不带结尾 0，带上去反而可能被当成特征名的一部分 */
    static const char banner[] = "host::features=shell_v2,cmd,stat_v2";
    unsigned int cmd, a0, a1;
    unsigned char buf[1024];
    int len, r, tries = 0;

    memset(a, 0, sizeof(*a));
    a->io = io;
    a->maxdata = ADB_MAXDATA_REQ;
    a->next_id = 1;

    r = send_msg(a, ADB_CNXN, ADB_VERSION, ADB_MAXDATA_REQ,
                 (const unsigned char *)banner, (int)(sizeof(banner) - 1));
    if (r < 0)
        return -1;

    for (;;) {
        r = recv_msg(a, &cmd, &a0, &a1, buf, (int)sizeof(buf), &len, 8000);
        if (r == -1) return -1;
        if (r == -2) return -2;

        if (cmd == ADB_CNXN) {
            /* 对端在自己 CNXN 的 arg0 里给出它接受的最大载荷，取小者 */
            if (a0 != 0 && a0 < a->maxdata)
                a->maxdata = a0;
            return 0;
        }

        if (cmd == ADB_AUTH) {
            if (a0 != ADB_AUTH_TOKEN || len != 20)
                return -2;                     /* 只认 20 字节的 token */

            tries++;
            if (tries == 1) {
                /* 先试签名：手机已经记住我们这把钥匙时走这条，不会弹提示 */
                unsigned char sig[256];
                if (rsa_sign_sha1(ADB_KEY_N, ADB_KEY_D, buf, 20, sig) != 0)
                    return -2;
                if (send_msg(a, ADB_AUTH, ADB_AUTH_SIGNATURE, 0, sig, 256) < 0)
                    return -1;
            } else if (tries <= 4) {
                /* 手机不认识我们的钥匙：把公钥发过去。
                 * 手机会弹「允许 USB 调试吗」，用户点一次就好。 */
                if (send_rsa_public_key(a) < 0)
                    return -1;
            } else {
                return -3;                     /* 用户拒绝了，或手机一直不认 */
            }
            continue;
        }

        return -2;                             /* 认证阶段收到别的，协议不对 */
    }
}

int adb_open(ADB *a, const char *service)
{
    unsigned int cmd, a0, a1;
    unsigned char buf[1024];
    int local_id, len, r, i, idx = -1;

    for (i = 0; i < ADB_MAX_CHAN; i++) {
        if (!a->ch[i].used) { idx = i; break; }
    }
    if (idx < 0)
        return -2;

    local_id = (int)a->next_id++;

    {
        char svc[64];
        int sl = (int)strlen(service);
        if (sl > 62) return -2;
        memcpy(svc, service, (size_t)sl);
        svc[sl] = 0;
        /* OPEN 的服务名按惯例带结尾 0 */
        if (send_msg(a, ADB_OPEN, (unsigned int)local_id, 0,
                     (const unsigned char *)svc, sl + 1) < 0)
            return -1;
    }

    for (;;) {
        r = recv_msg(a, &cmd, &a0, &a1, buf, (int)sizeof(buf), &len, 8000);
        if (r == -1) return -1;
        if (r == -2) return -2;

        if (cmd == ADB_OKAY && a1 == (unsigned int)local_id) {
            ADB_CHAN *c = &a->ch[idx];
            memset(c, 0, sizeof(*c));
            c->used = 1;
            c->remote_id = (int)a0;
            c->rx = 0; c->rx_cap = 0; c->rx_len = 0; c->rx_head = 0;
            c->closed = 0;
            {
                int sl = (int)strlen(service);
                if (sl > 63) sl = 63;
                memcpy(c->service, service, (size_t)sl);
                c->service[sl] = 0;
            }
            return local_id;
        }
        if (cmd == ADB_CLSE && a1 == (unsigned int)local_id)
            return -3;                          /* 手机拒绝了这条转发 */
        if (cmd == ADB_WRTE || cmd == ADB_CLSE || cmd == ADB_OKAY) {
            if (handle_msg(a, cmd, a0, a1, buf, len) < 0)
                return -1;
            continue;
        }
        /* 其它包忽略 */
    }
}

int adb_send(ADB *a, int chan, const unsigned char *data, int len)
{
    ADB_CHAN *c;
    int off = 0;

    if (chan < 1 || chan > ADB_MAX_CHAN)
        return -2;
    c = &a->ch[chan - 1];
    if (!c->used || c->closed)
        return -2;

    while (off < len) {
        int chunk = len - off;
        if ((unsigned int)chunk > a->maxdata)
            chunk = (int)a->maxdata;

        if (send_msg(a, ADB_WRTE, (unsigned int)chan,
                     (unsigned int)c->remote_id, data + off, chunk) < 0)
            return -1;

        /* ADB 是应答式的：发完必须等 OKAY，否则对端不会继续收。
         * 用逐包应答而不是流水线，实现简单且不会出错 —— 少量吞吐损失
         * 在 USB 上无关紧要。 */
        if (wait_okay(a, (unsigned int)chan, 8000) < 0)
            return -1;

        off += chunk;
    }
    return 0;
}

int adb_pump(ADB *a, int timeout_ms)
{
    unsigned int cmd, a0, a1;
    unsigned char *buf;
    int len, r, n = 0;

    buf = (unsigned char *)malloc((size_t)a->maxdata + 64);
    if (!buf)
        return -2;

    for (;;) {
        r = recv_msg(a, &cmd, &a0, &a1, buf, (int)a->maxdata + 64, &len,
                     n == 0 ? timeout_ms : 0);
        if (r == -1) { free(buf); return n > 0 ? n : -1; }
        if (r == -2) { free(buf); return -2; }

        if (cmd == ADB_WRTE || cmd == ADB_CLSE) {
            if (handle_msg(a, cmd, a0, a1, buf, len) < 0) {
                free(buf);
                return -1;
            }
            n++;
        }
        /* 最多收一轮就返回，避免长时间占着不放（超时传 0 时立刻返回）*/
        if (timeout_ms == 0)
            break;
        {
            /* 收到一个包后不再阻塞等下一个 */
            timeout_ms = 0;
        }
    }
    free(buf);
    return n;
}

int adb_recv(ADB *a, int chan, unsigned char *out, int cap)
{
    ADB_CHAN *c;
    int avail, n;

    if (chan < 1 || chan > ADB_MAX_CHAN)
        return -2;
    c = &a->ch[chan - 1];
    if (!c->used)
        return -2;
    if (c->closed && c->rx_head >= c->rx_len)
        return -1;                              /* 已关且没剩数据 */

    avail = c->rx_len - c->rx_head;
    if (avail <= 0)
        return 0;

    n = avail < cap ? avail : cap;
    memcpy(out, c->rx + c->rx_head, (size_t)n);
    c->rx_head += n;
    return n;
}

int adb_chan_closed(ADB *a, int chan)
{
    if (chan < 1 || chan > ADB_MAX_CHAN)
        return 1;
    return a->ch[chan - 1].closed;
}

unsigned int adb_maxdata(const ADB *a)
{
    return a->maxdata;
}
