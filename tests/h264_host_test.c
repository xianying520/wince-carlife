/* tests/h264_host_test.c
 *
 * 在电脑上验证 src/h264dec.c。
 *
 * 为什么必须这么测：车机没法试了，唯一能验证的地方就是这里。
 * 每个码流跑【三种切帧形态】：
 *   A) Annex-B 原样，按 1KB 分块喂（模拟 CarLife 一条消息一段）
 *   B) AVCC：4 字节大端长度 + NAL，同样分块喂
 *   C) 裸 NAL：一个 NAL 当一条消息
 * 三种都必须解出同样的画面。
 *
 * 断言三条：
 *   1. 必须出帧
 *   2. 尺寸 == 预期（480x270 那个 case 专门逼出裁剪路径，因为 270 不是 16 的倍数）
 *   3. 中心像素 == 预期颜色（给容差）
 *      红/绿/蓝分开测 —— 能抓出 BGRA / RGBA 通道顺序搞反
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h264dec.h"

#define MAX_NAL   256
#define OUTCAP    (1920 * 1088 * 4)
#define CHUNK     1024

static int g_fail = 0;

/* ---------- 工具 ---------- */

static unsigned char *slurp(const char *path, int *outLen)
{
    FILE *f = fopen(path, "rb");
    unsigned char *d;
    long n;
    if (!f) { printf("   无法打开 %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    d = (unsigned char *)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    *outLen = (int)n;
    return d;
}

/* 按起始码切出 NAL（返回的 off/len 不含起始码本身） */
static int find_nals(const unsigned char *d, int n, int *off, int *len, int max)
{
    int i = 0, cnt = 0, cur = -1;
    while (i + 3 <= n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (cur >= 0 && i > cur && cnt < max) {
                off[cnt] = cur; len[cnt] = i - cur; cnt++;
            }
            i += 3; cur = i;
        } else i++;
    }
    if (cur >= 0 && n > cur && cnt < max) { off[cnt] = cur; len[cnt] = n - cur; cnt++; }
    return cnt;
}

/* ---------- 一次解码跑 ---------- */

typedef struct {
    int frames, w, h, profile, fmt;
    unsigned char px[4];      /* 中心像素 BGRA */
    int pxValid;
} RESULT;

static void note_pixel(RESULT *R, const unsigned char *outbuf)
{
    int idx;
    if (R->w <= 0 || R->h <= 0) return;
    idx = (R->h / 2) * R->w + (R->w / 2);
    memcpy(R->px, outbuf + (size_t)idx * 4, 4);
    R->pxValid = 1;
}

/* mode: 0=AnnexB原样分块, 1=AVCC分块, 2=裸NAL逐条 */
static int run_one(const char *path, int mode, RESULT *out, const char **why)
{
    unsigned char *raw = NULL, *seq = NULL, *outbuf = NULL;
    int rawLen = 0, off[MAX_NAL], len[MAX_NAL], nnal = 0, seqLen = 0;
    H264DEC *d = NULL;
    int ok = 0, i, pos;

    memset(out, 0, sizeof(*out));
    *why = "";

    raw = slurp(path, &rawLen);
    if (!raw) { *why = "读文件失败"; goto done; }
    nnal = find_nals(raw, rawLen, off, len, MAX_NAL);
    if (nnal <= 0) { *why = "没找到 NAL"; goto done; }

    d = h264dec_open();
    outbuf = (unsigned char *)malloc(OUTCAP);
    if (!d) { *why = "h264dec_open 失败"; goto done; }
    if (!outbuf) { *why = "输出缓冲分配失败"; goto done; }

    if (mode == 1) {                       /* 组装 AVCC 序列 */
        int total = 0;
        for (i = 0; i < nnal; i++) total += 4 + len[i];
        seq = (unsigned char *)malloc((size_t)total);
        if (!seq) { *why = "序列缓冲分配失败"; goto done; }
        for (i = 0; i < nnal; i++) {
            seq[seqLen++] = (unsigned char)((len[i] >> 24) & 0xFF);
            seq[seqLen++] = (unsigned char)((len[i] >> 16) & 0xFF);
            seq[seqLen++] = (unsigned char)((len[i] >> 8) & 0xFF);
            seq[seqLen++] = (unsigned char)(len[i] & 0xFF);
            memcpy(seq + seqLen, raw + off[i], (size_t)len[i]);
            seqLen += len[i];
        }
    }

    if (mode == 2) {                       /* 逐条裸 NAL */
        for (i = 0; i < nnal; i++) {
            int w = 0, h = 0;
            int r = h264dec_feed(d, raw + off[i], len[i], outbuf, OUTCAP, &w, &h);
            if (r == H264DEC_GOT_FRAME) { out->frames++; out->w = w; out->h = h; }
        }
    } else {                               /* 分块喂 */
        const unsigned char *src = (mode == 0) ? raw : seq;
        int srcLen = (mode == 0) ? rawLen : seqLen;
        for (pos = 0; pos < srcLen; pos += CHUNK) {
            int chunk = srcLen - pos; int w = 0, h = 0; int r;
            if (chunk > CHUNK) chunk = CHUNK;
            r = h264dec_feed(d, src + pos, chunk, outbuf, OUTCAP, &w, &h);
            if (r == H264DEC_GOT_FRAME) { out->frames++; out->w = w; out->h = h; }
        }
    }

    if (out->frames <= 0) { *why = h264dec_log(d); goto done; }
    out->profile = h264dec_profile(d);
    out->fmt     = h264dec_format(d);
    note_pixel(out, outbuf);
    ok = 1;

done:
    if (d) h264dec_close(d);
    free(outbuf);
    free(seq);
    free(raw);
    return ok;
}

/* 期望颜色：主通道亮、其余暗 */
static int color_ok(const unsigned char px[4], int er, int eg, int eb)
{
    int B = px[0], G = px[1], R = px[2];
    if (er) return (R >= 140 && G <= 110 && B <= 110);
    if (eg) return (G >= 140 && R <= 110 && B <= 110);
    if (eb) return (B >= 140 && R <= 110 && G <= 110);
    return 0;
}

/* ---------- 主程序 ---------- */

typedef struct { const char *file; int w, h, er, eg, eb; } CASE;

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "samples";
    static const CASE cases[] = {
        { "red_480x272.h264",   480, 272, 1, 0, 0 },
        { "green_480x272.h264", 480, 272, 0, 1, 0 },
        { "blue_480x272.h264",  480, 272, 0, 0, 1 },
        { "red_480x270.h264",   480, 270, 1, 0, 0 },   /* 逼出裁剪路径 */
    };
    static const char *mn[3] = { "AnnexB分块", "AVCC分块", "裸NAL逐条" };
    char path[512];
    size_t ci;
    int m;

    printf("=== h264dec 主机验证 ===\n\n");

    for (ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        const CASE *c = &cases[ci];
        snprintf(path, sizeof(path), "%s/%s", dir, c->file);
        printf("  码流 %s  (期望 %dx%d)\n", c->file, c->w, c->h);

        for (m = 0; m < 3; m++) {
            RESULT R;
            const char *why = "";
            if (!run_one(path, m, &R, &why)) {
                printf("     %-10s  失败: %s\n", mn[m], why[0] ? why : "未知");
                g_fail++;
                continue;
            }
            if (R.w != c->w || R.h != c->h) {
                printf("     %-10s  尺寸错: 得到 %dx%d, 期望 %dx%d\n",
                       mn[m], R.w, R.h, c->w, c->h);
                g_fail++;
                continue;
            }
            if (!R.pxValid || !color_ok(R.px, c->er, c->eg, c->eb)) {
                printf("     %-10s  颜色错: 中心 BGRA=(%d,%d,%d,%d)\n",
                       mn[m], R.px[0], R.px[1], R.px[2], R.px[3]);
                g_fail++;
                continue;
            }
            printf("     %-10s  OK   出帧=%d(共3帧,末帧留待下一帧触发)  %dx%d  profile=%d(%s)  切帧=%s\n",
                   mn[m], R.frames, R.w, R.h,
                   R.profile, h264dec_profile_name(R.profile),
                   h264dec_format_name(R.fmt));
        }
        printf("\n");
    }

    if (g_fail) { printf("!! 共 %d 项失败\n", g_fail); return 1; }
    printf("全部通过 —— 三种切帧形态都解出了正确尺寸和正确颜色\n");
    return 0;
}
