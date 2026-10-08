#include "display.h"

#include <stdlib.h>     /* malloc/free 显式包含：不靠 windows.h 间接带进来 */
#include <string.h>

int disp_init(DISP *d, int sw, int sh)
{
    int need;

    if (sw <= 0 || sh <= 0 || sw > 4096 || sh > 4096)
        return -1;

    need = sw * sh * 4;
    if (d->fb && d->fbcap >= need) {
        d->sw = sw;
        d->sh = sh;
        return 0;
    }

    if (d->fb) {
        free(d->fb);
        d->fb = 0;
        d->fbcap = 0;
    }

    d->fb = (unsigned char *)malloc((size_t)need);
    if (!d->fb)
        return -1;

    d->fbcap = need;
    d->sw = sw;
    d->sh = sh;
    memset(d->fb, 0, (size_t)need);
    return 0;
}

void disp_free(DISP *d)
{
    if (d->fb) {
        free(d->fb);
        d->fb = 0;
    }
    d->fbcap = 0;
    d->sw = d->sh = 0;
}

void disp_set_rgb24(DISP *d, const unsigned char *rgb, int w, int h)
{
    int i, n;
    unsigned char *p;

    if (!d->fb || w != d->sw || h != d->sh)
        return;
    n = w * h;
    p = d->fb;
    for (i = 0; i < n; i++) {
        /* BGRA 顺序（Windows DIB 是 BGR）*/
        p[0] = rgb[i * 3 + 2];
        p[1] = rgb[i * 3 + 1];
        p[2] = rgb[i * 3 + 0];
        p[3] = 0;
        p += 4;                     /* ⚠ 必须前进。漏掉这一行的话，每一轮都在
                                     *   覆盖同一个 4 字节 —— 结果整块屏黑、只有
                                     *   左上角一个像素有颜色。
                                     *   这个 bug 极难靠肉眼发现：用纯色/纯灰的
                                     *   测试图时，写一个和写全部的结果一模一样，
                                     *   所有"看起来正常"的测试都会通过。只有
                                     *   逐像素断言不同值的图才逼得出来。 */
    }
    /* 源缓冲来自 nanojpeg，用完即可释放，这里不动它 */
}

/* BGRA 直接搬运。
 * h264bsd 输出的 u32 是 0xAARRGGBB，小端存下来就是 B,G,R,A —— 和 Windows DIB
 * 32bpp 的字节序一致，所以不需要任何逐像素转换，整行 memcpy 即可。
 * 这一点在 CI 上用红/绿/蓝三张纯色图逐像素断言过（专门抓通道顺序反掉的错）。 */
void disp_set_bgra(DISP *d, const unsigned char *bgra, int w, int h)
{
    int y;

    if (!d->fb || w != d->sw || h != d->sh)
        return;
    for (y = 0; y < h; y++)
        memcpy(d->fb + (size_t)y * w * 4, bgra + (size_t)y * w * 4,
               (size_t)w * 4);
}

void disp_set_gray8(DISP *d, const unsigned char *gray, int w, int h)
{
    int i, n;
    unsigned char *p;

    if (!d->fb || w != d->sw || h != d->sh)
        return;
    n = w * h;
    p = d->fb;
    for (i = 0; i < n; i++) {
        unsigned char g = gray[i];
        p[0] = g;
        p[1] = g;
        p[2] = g;
        p[3] = 0;
        p += 4;                     /* 同上，必须前进 */
    }
}

void disp_paint(DISP *d, HDC hdc, const RECT *client)
{
    BITMAPINFOHEADER bi;
    int cw, ch;

    if (!d->fb || d->sw <= 0 || d->sh <= 0)
        return;

    cw = client->right - client->left;
    ch = client->bottom - client->top;
    if (cw <= 0 || ch <= 0)
        return;

    memset(&bi, 0, sizeof(bi));
    bi.biSize        = sizeof(BITMAPINFOHEADER);
    bi.biWidth       = d->sw;
    /* 负高度 = 自上而下，和我们的缓冲方向一致，省一次翻转 */
    bi.biHeight      = -d->sh;
    bi.biPlanes      = 1;
    bi.biBitCount    = 32;
    bi.biCompression = BI_RGB;

    StretchDIBits(hdc,
                  client->left, client->top, cw, ch,
                  0, 0, d->sw, d->sh,
                  d->fb, (BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY);
    d->dw = cw;
    d->dh = ch;
}

int disp_map_touch(const DISP *d, const RECT *client, int cx, int cy,
                   int *px, int *py)
{
    int cw, ch, x, y;

    if (d->sw <= 0 || d->sh <= 0)
        return -1;

    cw = client->right - client->left;
    ch = client->bottom - client->top;
    if (cw <= 0 || ch <= 0)
        return -1;

    x = cx - client->left;
    y = cy - client->top;
    if (x < 0 || y < 0 || x >= cw || y >= ch)
        return -1;

    /* 按比例换算到手机画面坐标 */
    *px = (int)((long)x * d->sw / cw);
    *py = (int)((long)y * d->sh / ch);

    if (*px < 0) *px = 0;
    if (*py < 0) *py = 0;
    if (*px > d->sw - 1) *px = d->sw - 1;
    if (*py > d->sh - 1) *py = d->sh - 1;
    return 0;
}
