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

/* 一定要读满 len 字节 —— 流式设备一次读不全很正常。
 * 返回：len = 成功；0 = 超时；-1 = 出错或对端关闭。
 * ⚠ "超时"必须和"出错"分开：转发器是每 10 毫秒轮询一次的，
 *   如果超时也返回 -1，它会把"暂时没数据"误判成"设备挂了"，
 *   然后整个转发就停了 —— 这个坑必须避开。 */
static int read_full(ADB *a, unsigned char *buf, int len, int timeout_ms)
{
    int got = 0;
    while (got < len) {
        int r = a->io.read(a->io.ctx, buf + got, len - got, timeout_ms);
        if (r == 0)
            return 0;                  /* 超时 */
        if (r < 0)
            return -1;                 /* 出错 */
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
    int rr;

    rr = read_full(a, h, 24, timeout_ms);
    if (rr == 0)
        return 0;                      /* 超时 */
    if (rr < 0)
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

    if (dlen > 0) {
        rr = read_full(a, data, (int)dlen, timeout_ms);
        if (rr == 0) return 0;
        if (rr < 0)  return -1;
    }

    *len = (int)dlen;
    return 1;                          /* 收到一个完整包 */
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
        /* 收到就必须回 OKAY，否则对端不会再发 —— ADB 是严格应答式的。
         * ⚠ 应答里的 arg0 直接用收到的 a0，不要用 c->remote_id：
         *   在 OPEN 刚发出、OKAY 还没回来的那段时间里 remote_id 还没有值，
         *   用它会把对端自己的通道号填错（-1）。a0 就是对端的通道号，
         *   任何时候填它都对。 */
        send_msg(a, ADB_OKAY, a0, a1, 0, 0);
        return 0;
    }
    case ADB_CLSE: {
        ADB_CHAN *c = chan_by_local(a, a1);
        if (c && c->closed) {
            /* 这条已经关过了，说明收到的是对方的"关闭确认"，不能再回一个 ——
             * 两边都无脑回确认会变成互相来回打不完的 CLSE。 */
            return 0;
        }
        if (c)
            c->closed = 1;
        send_msg(a, ADB_CLSE, a0, a1, 0, 0);
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
        if (r <= 0)
            return -1;                 /* 超时也算失败：该来应答却一直不来 */

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

    r = send_msg(a, ADB_CNXN, ADB_VERSION, ADB_MAXDATA_REQ,
                 (const unsigned char *)banner, (int)(sizeof(banner) - 1));
    if (r < 0)
        return -1;

    for (;;) {
        r = recv_msg(a, &cmd, &a0, &a1, buf, (int)sizeof(buf), &len, 8000);
        if (r <= 0) return r == -2 ? -2 : -1;

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

    /* ⚠ 本地 id 必须恒等于「槽位下标 + 1」——这是本文件的一条不变量。
     *   chan_by_local() 就是按下标定位通道的，而对端推来的数据、关闭通知
     *   全都靠它分发。
     *   以前这里用 a->next_id++ 单独发号、槽位另找空闲的，两者会脱节：
     *   只要有一条通道先失败被释放、随后再开新通道，号就跑到前面去了
     *   （next_id 已经是 2，可空出来的是槽位 0），对端推来的数据于是落在
     *   一个"没人占用"的槽位上，被当成未知通道【静默丢弃】。
     *   表象是"转发明明是通的，数据却少了/命令没有输出"，极难查。
     *   这个坑是主机端到端测试跑出来的：假手机把包名清单紧跟在 OKAY 后发，
     *   而转发器此前刚被拒过一次通道 —— 两个条件凑齐才显形。 */
    local_id = idx + 1;

    /* ⚠ 先占住通道，再发 OPEN。
     *   手机端服务起来很快，OKAY 和数据常常前后脚到 —— 如果等收到 OKAY
     *   才把通道标记成"已使用"，这中间到达的数据会被 chan_by_local 当成
     *   未知通道【直接丢掉】，而 OKAY 又照常回给对端，对端完全察觉不到。
     *   结果就是"转发明明是通的，数据却少了/命令没输出"，极难查。
     *   这个坑是主机端到端测试跑出来的（假手机把包名清单紧跟在 OKAY 后面发）。 */
    {
        ADB_CHAN *c0 = &a->ch[idx];
        int sl = (int)strlen(service);
        memset(c0, 0, sizeof(*c0));
        c0->used = 1;                  /* 本地 id 不用存：就是下标 +1 */
        c0->remote_id = -1;            /* 还没有，收到 OKAY 再补 */
        if (sl > 63) sl = 63;
        memcpy(c0->service, service, (size_t)sl);
        c0->service[sl] = 0;
    }

    {
        /* ⚠ 服务名缓冲区要够大。原来这里是 char svc[64]、超过 62 字符就
         *   直接返回失败，而真实的 shell 命令很容易超：
         *     shell:monkey -p com.baidu.carlife -c android.intent.category.LAUNCHER 1
         *   就已经 67 个字符了 —— 于是"启动手机端"这一步悄悄失败，调用方
         *   还以为是成功了（adb_open 返回负值被忽略，只当命令没输出）。
         *   真机上 am start 之类的命令只会更长。这个坑是主机端到端测试里
         *   "假手机只收到一条 shell 命令"暴露出来的。
         *   顺带把返回值也检查掉，别再静默失败。 */
        char svc[512];
        int sl = (int)strlen(service);
        if (sl > 500) return -2;
        memcpy(svc, service, (size_t)sl);
        svc[sl] = 0;
        /* OPEN 的服务名按惯例带结尾 0 */
        if (send_msg(a, ADB_OPEN, (unsigned int)local_id, 0,
                     (const unsigned char *)svc, sl + 1) < 0) {
            memset(&a->ch[idx], 0, sizeof(a->ch[idx]));
            return -1;
        }
    }

    for (;;) {
        r = recv_msg(a, &cmd, &a0, &a1, buf, (int)sizeof(buf), &len, 8000);
        if (r <= 0) return r == -2 ? -2 : -1;

        if (cmd == ADB_OKAY && a1 == (unsigned int)local_id) {
            /* 通道在发 OPEN 时就占好了，这里只补上对端的通道号 ——
             * 期间可能已经有数据存进 c->rx 了，绝不能 memset 清掉。 */
            a->ch[idx].remote_id = (int)a0;
            return local_id;
        }
        if (cmd == ADB_CLSE && a1 == (unsigned int)local_id) {
            memset(&a->ch[idx], 0, sizeof(a->ch[idx]));   /* 被拒：把占位还回去 */
            return -3;                          /* 手机拒绝了这条转发 */
        }
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
        if (r == 0)
            break;                     /* 本轮没数据：正常情况，不是错误 */
        if (r == -1) { free(buf); return n > 0 ? n : -1; }
        if (r == -2) { free(buf); return -2; }

        if (cmd == ADB_WRTE || cmd == ADB_CLSE) {
            if (handle_msg(a, cmd, a0, a1, buf, len) < 0) {
                free(buf);
                return -1;
            }
            n++;
        }
        timeout_ms = 0;                /* 收到一个后不再阻塞等下一个 */
    }
    free(buf);
    return n;
}

/* ── shell 服务 ── */
int adb_run_shell(ADB *a, const char *cmd, char *out, int cap, int timeout_ms)
{
    char service[300];
    int  id, used = 0, waited = 0;
    unsigned char buf[4096];
    int  sl, n;

    if (!a || !cmd || !out || cap <= 0)
        return -2;
    out[0] = 0;

    sl = (int)strlen(cmd);
    if (6 + sl >= (int)sizeof(service))
        return -2;
    memcpy(service, "shell:", 6);
    memcpy(service + 6, cmd, (size_t)sl);
    service[6 + sl] = 0;

    id = adb_open(a, service);
    if (id < 0)
        return id;              /* 负值如实往外传，调用方必须检查 */

    /* 命令的输出会以 WRTE 陆续到达，命令结束时对端发 CLSE。
     * 这里一边收一边等，直到通道关闭或超时。 */
    while (waited < timeout_ms) {
        int r = adb_pump(a, 100);
        if (r < 0)
            break;
        for (;;) {
            n = adb_recv(a, id, buf, (int)sizeof(buf));
            if (n <= 0)
                break;
            if (used + n < cap - 1) {
                memcpy(out + used, buf, (size_t)n);
                used += n;
                out[used] = 0;
            }
        }
        if (adb_chan_closed(a, id))
            break;
        waited += 100;
    }

    adb_close_chan(a, id);
    return 0;
}

/* 大小写不敏感的子串查找。
 * ⚠ 这里必须是真的子串匹配，不能写成"每个字符都能在包名里找到" ——
 *   后者是子序列匹配，会把 com.example.facility 这种包名误判成 CarLife
 *   （c/a/r/l/i/f/e 全都在里面）。这个错我写完就自查出来了，
 *   并把那个误判案例固定进了测试。 */
static int ci_contains(const char *hay, const char *needle)
{
    int i, j;
    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j]; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}

int adb_find_carlife_pkg(const char *pm_output, char *out, int cap)
{
    /* 按优先级找：先找 CarLife 本身，再找 vivo 的 Jovi InCar。
     * pm 的输出形如 "package:com.baidu.carlife"。 */
    static const char *keys[] = {
        "com.baidu.carlife",
        "carlife",
        "joviincar",
        "jovi.incar",
        "incar",
        "carbit"
    };
    const char *p;
    int k;

    if (!pm_output || !out || cap <= 0)
        return -1;
    out[0] = 0;

    for (k = 0; k < (int)(sizeof(keys) / sizeof(keys[0])); k++) {
        p = pm_output;
        while ((p = strstr(p, "package:")) != 0) {
            const char *e;
            int len;
            p += 8;
            e = p;
            while (*e && *e != '\r' && *e != '\n') e++;
            len = (int)(e - p);

            /* 在包名里做大小写不敏感的子串匹配 */
            if (len > 0 && len < 200) {
                char name[208];
                if (len >= (int)sizeof(name)) { p = e; continue; }
                memcpy(name, p, (size_t)len);
                name[len] = 0;

                if (ci_contains(name, keys[k])) {
                    if (len >= cap) len = cap - 1;
                    memcpy(out, name, (size_t)len);
                    out[len] = 0;
                    return 0;
                }
            }
            p = e;
        }
    }
    return -1;
}

/* 主动关掉一条通道（本地 socket 断开时用） */
int adb_close_chan(ADB *a, int chan)
{
    ADB_CHAN *c;

    if (chan < 1 || chan > ADB_MAX_CHAN)
        return -2;
    c = &a->ch[chan - 1];
    if (!c->used)
        return 0;

    if (!c->closed && c->remote_id >= 0)
        send_msg(a, ADB_CLSE, (unsigned int)chan,
                 (unsigned int)c->remote_id, 0, 0);

    if (c->rx) free(c->rx);
    memset(c, 0, sizeof(*c));
    return 0;
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
