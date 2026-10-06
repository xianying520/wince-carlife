/* rsa.c — 见 rsa.h 的说明。 */
#include "rsa.h"

#include <string.h>

/* ══════════════════════════════════════════════════════════════
 * 第一部分：SHA-1
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    unsigned int       h[5];
    unsigned long long bits;      /* 消息总位数 */
    unsigned char      buf[64];
    int                n;         /* buf 里已缓存的字节数 */
} SHA1_CTX;

#define ROL32(x, k) (((x) << (k)) | ((x) >> (32 - (k))))

static void sha1_block(SHA1_CTX *c, const unsigned char *p)
{
    unsigned int w[80], a, b, d, e, f, k, t, cc;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((unsigned int)p[i * 4] << 24) | ((unsigned int)p[i * 4 + 1] << 16) |
               ((unsigned int)p[i * 4 + 2] << 8) | (unsigned int)p[i * 4 + 3];
    for (i = 16; i < 80; i++)
        w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];

    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & cc) | ((~b) & d);        k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else             { f = b ^ cc ^ d;                   k = 0xCA62C1D6; }
        t = ROL32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = ROL32(b, 30); b = a; a = t;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_init(SHA1_CTX *c)
{
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
    c->bits = 0;
    c->n    = 0;
}

static void sha1_update(SHA1_CTX *c, const unsigned char *p, int len)
{
    int i;
    c->bits += (unsigned long long)len * 8;
    for (i = 0; i < len; i++) {
        c->buf[c->n++] = p[i];
        if (c->n == 64) {
            sha1_block(c, c->buf);
            c->n = 0;
        }
    }
}

static void sha1_final(SHA1_CTX *c, unsigned char out[20])
{
    unsigned long long bits = c->bits;
    unsigned char pad = 0x80;
    unsigned char zero = 0;
    unsigned char lenb[8];
    int i;

    /* 补 0x80，再补 0 直到位置对齐到 56，最后补 8 字节长度。
     * 这里刻意写得直白：逐字节补，边补边看位置 —— 而不是先算数学公式，
     * 因为公式算错时行为是"偶尔对偶尔错"，极难查。 */
    sha1_update(c, &pad, 1);
    while (c->n != 56)
        sha1_update(c, &zero, 1);
    for (i = 0; i < 8; i++)
        lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha1_update(c, lenb, 8);

    for (i = 0; i < 5; i++) {
        out[i * 4]     = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(c->h[i]);
    }
}

void sha1(const unsigned char *data, int len, unsigned char out[20])
{
    SHA1_CTX c;
    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}

/* ══════════════════════════════════════════════════════════════
 * 第二部分：2048 位大整数（32 位 limb，小端存放）
 *
 * 只做模幂，不做除法。模约减全部靠蒙哥马利乘法完成 —— 因为写一个大整数
 * 长除法很容易出错，而蒙哥马利只需要乘法和减法，正确性容易保证。
 * ══════════════════════════════════════════════════════════════ */

#define BN_LIMBS 64          /* 64 * 32 = 2048 位 */

typedef struct { unsigned int w[BN_LIMBS]; } BN;

static void bn_zero(BN *a)
{
    memset(a, 0, sizeof(*a));
}

static void bn_from_be(BN *a, const unsigned char *p, int nbytes)
{
    int i;
    bn_zero(a);
    for (i = 0; i < nbytes; i++) {
        int bytepos = nbytes - 1 - i;              /* 从最低字节开始 */
        a->w[i / 4] |= (unsigned int)p[bytepos] << (8 * (i % 4));
    }
}

static void bn_to_be(const BN *a, unsigned char *p, int nbytes)
{
    int i;
    for (i = 0; i < nbytes; i++) {
        int bytepos = nbytes - 1 - i;
        p[bytepos] = (unsigned char)(a->w[i / 4] >> (8 * (i % 4)));
    }
}

static int bn_cmp(const BN *a, const BN *b)
{
    int i;
    for (i = BN_LIMBS - 1; i >= 0; i--) {
        if (a->w[i] != b->w[i])
            return a->w[i] > b->w[i] ? 1 : -1;
    }
    return 0;
}

/* a -= b（调用者保证 a >= b） */
static void bn_sub_in(BN *a, const BN *b)
{
    unsigned long long s;
    unsigned int borrow = 0;
    int i;
    for (i = 0; i < BN_LIMBS; i++) {
        s = (unsigned long long)a->w[i] - b->w[i] - borrow;
        a->w[i] = (unsigned int)s;
        borrow = (unsigned int)((s >> 32) & 1);
    }
}

/* x = (x * 2) mod n。因为 x < n，所以 2x < 2n，最多减一次 n。 */
static void bn_dbl_mod(BN *x, const BN *n)
{
    unsigned int t[BN_LIMBS + 1];
    unsigned int carry = 0, borrow = 0;
    unsigned long long s;
    int i;

    for (i = 0; i < BN_LIMBS; i++) {
        t[i]    = (x->w[i] << 1) | carry;
        carry   = x->w[i] >> 31;
    }
    t[BN_LIMBS] = carry;

    if (carry) {
        /* 溢出到第 2049 位：结果 = 2x - n。
         * 因为 2x < 2n 且 n >= 2^2047，所以减完一定落在 2048 位以内，
         * 最终借位与 carry 抵消，忽略即可。 */
        for (i = 0; i < BN_LIMBS; i++) {
            s = (unsigned long long)t[i] - n->w[i] - borrow;
            x->w[i] = (unsigned int)s;
            borrow = (unsigned int)((s >> 32) & 1);
        }
    } else {
        for (i = 0; i < BN_LIMBS; i++)
            x->w[i] = t[i];
        if (bn_cmp(x, n) >= 0)
            bn_sub_in(x, n);
    }
}

/* ══════════════════════════════════════════════════════════════
 * 第三部分：蒙哥马利乘法
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    BN           n;         /* 模数 */
    BN           rr;        /* R^2 mod n，R = 2^2048 */
    unsigned int n0inv;     /* -n^-1 mod 2^32 */
} MONT;

static void mont_init(MONT *m, const unsigned char *nbytes)
{
    unsigned int inv;
    int i;

    bn_from_be(&m->n, nbytes, 256);

    /* n0inv：牛顿迭代，每轮翻倍正确位数，5 轮够 32 位 */
    inv = 1;
    for (i = 0; i < 5; i++)
        inv *= 2u - m->n.w[0] * inv;
    m->n0inv = (unsigned int)(0u - inv);

    /* rr = 2^4096 mod n。用 4096 次「翻倍 + 条件约减」算出来 ——
     * 比写一个大整数长除法安全得多，而且只在初始化时跑一次。 */
    bn_zero(&m->rr);
    m->rr.w[0] = 1;
    for (i = 0; i < 4096; i++)
        bn_dbl_mod(&m->rr, &m->n);
}

/* CIOS 蒙哥马利乘法：r = a * b * R^-1 mod n */
static void mont_mul(BN *r, const BN *a, const BN *b, const MONT *m)
{
    unsigned int t[BN_LIMBS + 2];
    unsigned long long s, c;
    int i, j;

    memset(t, 0, sizeof(t));

    for (i = 0; i < BN_LIMBS; i++) {
        /* t += a * b->w[i] */
        c = 0;
        for (j = 0; j < BN_LIMBS; j++) {
            s = (unsigned long long)t[j]
              + (unsigned long long)a->w[j] * b->w[i]
              + c;
            t[j] = (unsigned int)s;
            c    = s >> 32;
        }
        s = (unsigned long long)t[BN_LIMBS] + c;
        t[BN_LIMBS]     = (unsigned int)s;
        t[BN_LIMBS + 1] = (unsigned int)(s >> 32);

        /* m_i = t[0] * n0inv mod 2^32，然后 t = (t + m_i*n) / 2^32 */
        {
            unsigned int mi = t[0] * m->n0inv;
            s = (unsigned long long)t[0] + (unsigned long long)mi * m->n.w[0];
            c = s >> 32;                    /* 低 32 位必为 0，正是要消掉的那一位 */
            for (j = 1; j < BN_LIMBS; j++) {
                s = (unsigned long long)t[j]
                  + (unsigned long long)mi * m->n.w[j]
                  + c;
                t[j - 1] = (unsigned int)s;
                c        = s >> 32;
            }
            s = (unsigned long long)t[BN_LIMBS] + c;
            t[BN_LIMBS - 1] = (unsigned int)s;
            t[BN_LIMBS]     = t[BN_LIMBS + 1] + (unsigned int)(s >> 32);
        }
    }

    for (i = 0; i < BN_LIMBS; i++)
        r->w[i] = t[i];

    /* 此时结果 < 2n，最多减一次。若 t[64] 非零，说明结果在 2^2048 之上，
     * 减 n 后的最终借位与 t[64] 相等并抵消，所以忽略借位是正确的。 */
    if (t[BN_LIMBS] != 0 || bn_cmp(r, &m->n) >= 0)
        bn_sub_in(r, &m->n);
}

/* r = a^e mod n，e 是 elen 字节的大端指数 */
static void mont_pow(BN *r, const BN *a, const unsigned char *e, int elen,
                     const MONT *m)
{
    BN acc, base, one;
    int i, bit;

    bn_zero(&one);
    one.w[0] = 1;

    /* acc = 1，但要转成蒙哥马利形式：1 * R^2 * R^-1 = R */
    mont_mul(&acc, &m->rr, &one, m);
    /* base = a 的蒙哥马利形式 */
    mont_mul(&base, a, &m->rr, m);

    /* 从最低位开始：是 1 就乘进去，然后底数自平方 */
    for (i = 0; i < elen * 8; i++) {
        bit = (e[elen - 1 - (i >> 3)] >> (i & 7)) & 1;
        if (bit)
            mont_mul(&acc, &acc, &base, m);
        mont_mul(&base, &base, &base, m);
    }

    /* 从蒙哥马利形式变回普通形式 */
    mont_mul(r, &acc, &one, m);
}

/* ══════════════════════════════════════════════════════════════
 * 第四部分：对外接口
 * ══════════════════════════════════════════════════════════════ */

/* PKCS#1 v1.5 里 SHA-1 的 DigestInfo 前缀（15 字节）+ 20 字节摘要 = 35 字节 */
static const unsigned char SHA1_DIGESTINFO_PREFIX[15] = {
    0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2B, 0x0E, 0x03, 0x02, 0x1A,
    0x05, 0x00, 0x04, 0x14
};

int rsa_sign_sha1(const unsigned char *n, const unsigned char *d,
                  const unsigned char *msg, int msglen,
                  unsigned char *sig)
{
    MONT          m;
    BN            em, res;
    unsigned char digest[20];
    unsigned char block[256];
    int           i;

    if (!n || !d || !sig)
        return -1;

    sha1(msg, msglen, digest);

    /* 组装 EM = 00 01 FF FF ... FF 00 || DigestInfo */
    block[0] = 0x00;
    block[1] = 0x01;
    for (i = 2; i < 256 - 35 - 1; i++)
        block[i] = 0xFF;
    block[256 - 35 - 1] = 0x00;
    memcpy(block + 256 - 35, SHA1_DIGESTINFO_PREFIX, 15);
    memcpy(block + 256 - 20, digest, 20);

    bn_from_be(&em, block, 256);

    mont_init(&m, n);
    mont_pow(&res, &em, d, 256, &m);
    bn_to_be(&res, sig, 256);

    return 0;
}

int adb_public_key_blob(const unsigned char *n, unsigned int e,
                        unsigned char *out, int cap)
{
    MONT          m;
    unsigned char tmp[256];
    int           i;

    if (cap < 524)
        return -1;

    mont_init(&m, n);

    /* ① 模数的字数（小端 4 字节）= 64 */
    out[0] = (unsigned char)BN_LIMBS; out[1] = 0; out[2] = 0; out[3] = 0;

    /* ② n0inv（小端） */
    for (i = 0; i < 4; i++)
        out[4 + i] = (unsigned char)(m.n0inv >> (8 * i));

    /* ③ 模数，小端 256 字节 */
    for (i = 0; i < 256; i++)
        out[8 + i] = n[255 - i];

    /* ④ rr = R^2 mod n，小端 256 字节 */
    bn_to_be(&m.rr, tmp, 256);
    for (i = 0; i < 256; i++)
        out[8 + 256 + i] = tmp[255 - i];

    /* ⑤ 公开指数，小端 4 字节 */
    for (i = 0; i < 4; i++)
        out[8 + 512 + i] = (unsigned char)(e >> (8 * i));

    return 524;
}
