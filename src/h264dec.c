/* h264dec.c — 见 h264dec.h 的说明 */
#include <string.h>
#include <stdlib.h>
#include "h264dec.h"
#include "h264bsd_decoder.h"

/* 累积缓冲：一条 CarLife 视频消息通常几 KB~几十 KB，
 * 这里给足余量，同时不至于在老车机的小内存里占太多 */
#define ACC_CAP   (256*1024)
#define PEND_CAP  (8*1024)

struct H264DEC {
    storage_t     *st;
    int            inited;

    unsigned char *acc;      /* 累积缓冲（Annex-B 形态）*/
    int            accLen;   /* 有效字节 */
    int            accUse;   /* 已被 h264bsd 吃掉的字节 */

    unsigned char *pend;     /* AVCC 模式下没凑齐一个 NAL 的尾巴 */
    int            pendLen;

    int            fmt;
    int            w, h;     /* 裁剪后的真实尺寸 */
    int            mbw, mbh; /* 16 像素对齐的尺寸 */
    int            profile;
    int            frames;
    int            errors;
    u32            picId;
    int            profileWarned;
    char           log[128];
};

/* ---------- 小工具（不依赖 printf，coredll 上没有可靠的 printf 家族）---------- */

static void num2str(int v, char *out)
{
    char t[12]; int i = 0, j = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) t[i++] = '0';
    while (v > 0 && i < 11) { t[i++] = (char)('0' + v % 10); v /= 10; }
    if (neg) out[j++] = '-';
    while (i > 0) out[j++] = t[--i];
    out[j] = 0;
}

static void setlog(H264DEC *d, const char *a, int num, const char *b)
{
    char *p = d->log;
    int n;
    if (!d) return;
    for (n = 0; a && a[n] && n < 60; n++) p[n] = a[n];
    p += n;
    if (num != -9999) { num2str(num, p); while (*p) p++; }
    for (n = 0; b && b[n] && (p - d->log) < 110; n++) p[n] = b[n];
    p += n;
    *p = 0;
}

/* ---------- 编码猜测 ---------- */

int h264dec_guess_codec(const unsigned char *p, int n)
{
    if (!p || n < 3) return 0;
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xD8) return 2;          /* JPEG */
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return 1;
    if (p[0] == 0 && p[1] == 0 && p[2] == 1) return 1;
    {   /* 裸 NAL：首字节的 nal_unit_type 落在 1..12 */
        int t = p[0] & 0x1F;
        if (t >= 1 && t <= 12) return 1;
    }
    return 0;
}

/* ---------- 切帧方式识别 ----------
 * 判据来自 H.264 的 NAL 头结构：forbidden_zero(1)|nal_ref_idc(2)|nal_unit_type(5)
 * 合法 type 是 1..12。AVCC 的长度前缀在 NAL 很小时首字节往往是 0x00,
 * 对应 type=0 —— 非法，所以能干净地区分开。 */
static int detect_format(const unsigned char *p, int n)
{
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return H264DEC_FMT_ANNEXB;
    if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1)               return H264DEC_FMT_ANNEXB;
    {   int t = p[0] & 0x1F;
        if (t >= 1 && t <= 12) return H264DEC_FMT_RAW; }
    if (n >= 4) {
        unsigned L = ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
                     ((unsigned)p[2] << 8) | (unsigned)p[3];
        if (L > 0 && L <= (unsigned)n) return H264DEC_FMT_AVCC;
    }
    return H264DEC_FMT_ANNEXB;   /* 兜底：按起始码解析 */
}

const char *h264dec_format_name(int f)
{
    switch (f) {
    case H264DEC_FMT_ANNEXB: return "AnnexB(起始码)";
    case H264DEC_FMT_AVCC:   return "AVCC(长度前缀)";
    case H264DEC_FMT_RAW:    return "裸NAL";
    default:                 return "未识别";
    }
}

const char *h264dec_profile_name(int p)
{
    switch (p) {
    case 66:  return "Baseline(可解)";
    case 77:  return "Main(不支持)";
    case 88:  return "Extended(不支持)";
    case 100: return "High(不支持)";
    case 110: return "High10(不支持)";
    case 122: return "High422(不支持)";
    case 244: return "High444(不支持)";
    case 0:   return "未知";
    default:  return "其他";
    }
}

/* ---------- 追加到累积缓冲 ---------- */
static int acc_push(H264DEC *d, const unsigned char *p, int n)
{
    if (n <= 0) return 1;
    if (d->accLen + n > ACC_CAP) {
        setlog(d, "累积缓冲满,丢弃 ", d->accLen, " 字节");
        d->accLen = 0; d->accUse = 0;
        d->errors++;
        return 0;
    }
    memcpy(d->acc + d->accLen, p, (size_t)n);
    d->accLen += n;
    return 1;
}

static const unsigned char START4[4] = { 0, 0, 0, 1 };

/* AVCC：4 字节大端长度 + NAL，可能一条消息里多个，也可能被切断 */
static int ingest_avcc(H264DEC *d, const unsigned char *p, int n)
{
    int i = 0;
    /* 先把上一条没凑齐的尾巴接上 */
    unsigned char tmp[PEND_CAP + 64];
    if (d->pendLen > 0) {
        if (d->pendLen + n > (int)sizeof(tmp)) { d->pendLen = 0; return 0; }
        memcpy(tmp, d->pend, (size_t)d->pendLen);
        memcpy(tmp + d->pendLen, p, (size_t)n);
        n += d->pendLen;
        p = tmp;
        d->pendLen = 0;
    }
    while (i + 4 <= n) {
        unsigned L = ((unsigned)p[i] << 24) | ((unsigned)p[i+1] << 16) |
                     ((unsigned)p[i+2] << 8) | (unsigned)p[i+3];
        if (L == 0 || L > 4u * 1024u * 1024u) {           /* 明显不是长度，当裸 NAL */
            if (!acc_push(d, START4, 4)) return 0;
            if (!acc_push(d, p + i, n - i)) return 0;
            if (!acc_push(d, START4, 4)) return 0;
            return 1;
        }
        if ((int)L > n - i - 4) break;                    /* 这个 NAL 还没收齐 */
        if (!acc_push(d, START4, 4)) return 0;
        if (!acc_push(d, p + i + 4, (int)L)) return 0;
        if (!acc_push(d, START4, 4)) return 0;   /* 终止符，见下面的说明 */
        i += 4 + (int)L;
    }
    if (i < n) {                                          /* 尾巴留到下次 */
        int rest = n - i;
        if (rest > PEND_CAP) rest = PEND_CAP;
        memcpy(d->pend, p + i, (size_t)rest);
        d->pendLen = rest;
    }
    return 1;
}

/* ---------- 开 / 关 ---------- */

H264DEC *h264dec_open(void)
{
    H264DEC *d = (H264DEC *)malloc(sizeof(H264DEC));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    d->acc  = (unsigned char *)malloc(ACC_CAP);
    d->pend = (unsigned char *)malloc(PEND_CAP);
    d->st   = h264bsdAlloc();
    if (!d->acc || !d->pend || !d->st) {
        if (d->acc)  free(d->acc);
        if (d->pend) free(d->pend);
        if (d->st)   h264bsdFree(d->st);
        free(d);
        return NULL;
    }
    /* noOutputReordering=1：要的是最低延迟，不等重排序 */
    if (h264bsdInit(d->st, 1) != 0) {
        h264bsdFree(d->st); free(d->acc); free(d->pend); free(d);
        return NULL;
    }
    d->inited = 1;
    setlog(d, "解码器就绪,等首帧", -9999, NULL);
    return d;
}

void h264dec_close(H264DEC *d)
{
    if (!d) return;
    if (d->inited && d->st) h264bsdShutdown(d->st);
    if (d->st) h264bsdFree(d->st);
    if (d->acc)  free(d->acc);
    if (d->pend) free(d->pend);
    free(d);
}

int  h264dec_width  (H264DEC *d) { return d ? d->w : 0; }
int  h264dec_height (H264DEC *d) { return d ? d->h : 0; }
int  h264dec_profile(H264DEC *d) { return d ? d->profile : 0; }
int  h264dec_format (H264DEC *d) { return d ? d->fmt : 0; }
int  h264dec_frames (H264DEC *d) { return d ? d->frames : 0; }
const char *h264dec_log(H264DEC *d) { return d ? d->log : ""; }

/* ---------- 主循环 ---------- */

int h264dec_feed(H264DEC *d, const unsigned char *data, int len,
                 unsigned char *out_bgra, int out_cap, int *out_w, int *out_h)
{
    int result = H264DEC_OK;

    if (!d || !d->inited) return H264DEC_ERR_DATA;
    if (!data || len <= 0) return H264DEC_OK;

    /* 1) 首帧识别切帧方式，之后锁定 */
    if (d->fmt == H264DEC_FMT_UNKNOWN) {
        d->fmt = detect_format(data, len);
        setlog(d, "首帧切帧=", -9999, h264dec_format_name(d->fmt));
    }

    /* 2) 并入累积缓冲（AVCC 要转成 Annex-B，其余原样） */
    if (d->fmt == H264DEC_FMT_AVCC) {
        if (!ingest_avcc(d, data, len)) return H264DEC_ERR_MEM;
    } else if (d->fmt == H264DEC_FMT_RAW) {
        /* 一条消息就是一个完整 NAL：前面补起始码让它能起头，
         * 后面也补一个 —— h264bsd 靠「下一个起始码」判断 NAL 在哪里结束，
         * 末尾没有终止符它会挂进「本 NAL 未完成」状态，下一帧就对不上了。 */
        if (!acc_push(d, START4, 4))            return H264DEC_ERR_MEM;
        if (!acc_push(d, data, len))            return H264DEC_ERR_MEM;
        if (!acc_push(d, START4, 4))            return H264DEC_ERR_MEM;
    } else {
        if (!acc_push(d, data, len))            return H264DEC_ERR_MEM;
    }

    /* 3) 循环喂。h264bsd 一次吃一个 NAL，readBytes 回告吃了多少。
     *    这里刻意用「同一个缓冲 + 只推进 accUse」的写法：
     *    h264bsd 内部靠指针相等来判断「上次没吃完的是同一块」，
     *    每帧都 memmove 会把指针变掉、踩进它的重入分支。 */
    for (;;) {
        u32 readBytes = 0;
        u32 r;
        int avail = d->accLen - d->accUse;

        if (avail <= 0) break;

        r = h264bsdDecode(d->st, d->acc + d->accUse, (u32)avail, d->picId, &readBytes);

        if (r == H264BSD_PARAM_SET_ERROR || r == H264BSD_ERROR) {
            d->errors++;
            setlog(d, "解码错误,已丢弃 ", d->errors, " 次");
            d->accLen = 0; d->accUse = 0;
            return (result == H264DEC_OK) ? H264DEC_ERR_DATA : result;
        }
        if (r == H264BSD_MEMALLOC_ERROR) {
            d->accLen = 0; d->accUse = 0;
            return H264DEC_ERR_MEM;
        }

        if (readBytes == 0) {
            /* 这个 NAL 还没收齐，等下次的字节。指针保持不变 → h264bsd 认得 */
            break;
        }
        d->accUse += (int)readBytes;

        if (r == H264BSD_HDRS_RDY) {
            u32 cf = 0, cl = 0, cw = 0, ct = 0, ch = 0;
            d->mbw = (int)h264bsdPicWidth(d->st)  * 16;
            d->mbh = (int)h264bsdPicHeight(d->st) * 16;
            h264bsdCroppingParams(d->st, &cf, &cl, &cw, &ct, &ch);
            if (cf && cw > 0 && ch > 0) { d->w = (int)cw; d->h = (int)ch; }
            else                        { d->w = d->mbw; d->h = d->mbh; }
            d->profile = (int)h264bsdProfile(d->st);
            setlog(d, "SPS: ", d->w, "x");
            {   /* 把高度接上去 */
                char *p = d->log; while (*p) p++;
                num2str(d->h, p); while (*p) p++;
                *p = 0;
            }
            if (d->profile != 66 && d->profile != 0 && !d->profileWarned) {
                d->profileWarned = 1;
                setlog(d, "profile 不支持: ", d->profile, h264dec_profile_name(d->profile));
            }
        }

        if (r == H264BSD_PIC_RDY) {
            u32 pid = 0, idr = 0, nerr = 0;
            u32 *pic = NULL, *last = NULL;
            int lastW = 0, lastH = 0;

            /* 一次可能积压多帧，取最新的那一帧 */
            while ((pic = h264bsdNextOutputPictureBGRA(d->st, &pid, &idr, &nerr)) != NULL) {
                last = pic; lastW = d->mbw; lastH = d->mbh;
            }
            if (last) {
                int cw = d->w > 0 ? d->w : lastW;
                int ch = d->h > 0 ? d->h : lastH;
                if (cw > lastW) cw = lastW;
                if (ch > lastH) ch = lastH;
                if (out_bgra && out_cap >= cw * ch * 4) {
                    const unsigned char *src = (const unsigned char *)last;
                    int y;
                    for (y = 0; y < ch; y++)
                        memcpy(out_bgra + (size_t)y * cw * 4,
                               src + (size_t)y * lastW * 4,
                               (size_t)cw * 4);
                    if (out_w) *out_w = cw;
                    if (out_h) *out_h = ch;
                    d->frames++;
                    setlog(d, "出帧 #", d->frames, NULL);
                    result = H264DEC_GOT_FRAME;
                } else if (out_bgra) {
                    setlog(d, "输出缓冲太小,需要 ", cw * ch * 4, " 字节");
                    return H264DEC_ERR_MEM;
                }
            }
        }

        if (d->accUse >= d->accLen) { d->accLen = 0; d->accUse = 0; }
    }

    /* 全部吃完就归零，便于下次从 buf[0] 开始 */
    if (d->accUse > 0 && d->accUse >= d->accLen) { d->accLen = 0; d->accUse = 0; }

    return result;
}
