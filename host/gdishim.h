/* gdishim.h — 让 display.c 能在电脑上编译运行的最小 GDI 垫片
 *
 * 只做类型与函数名等价映射，不改动任何业务逻辑 —— 和 clhost.h 一个路子。
 *
 * StretchDIBits 不是空实现，而是把参数【记下来】。这样测试不仅能验证像素
 * 转换对不对，还能验证"铺到屏幕上的位置和尺寸对不对"—— 后者错了同样表现
 * 为画面错位或只占一角。
 */
#ifndef GDISHIM_H
#define GDISHIM_H

#include <string.h>

typedef void *HDC;

typedef struct {
    long left, top, right, bottom;
} RECT;

typedef struct {
    unsigned int   biSize;
    long           biWidth, biHeight;
    unsigned short biPlanes, biBitCount;
    unsigned int   biCompression, biSizeImage;
    long           biXPelsPerMeter, biYPelsPerMeter;
    unsigned int   biClrUsed, biClrImportant;
} BITMAPINFOHEADER;

typedef struct {
    BITMAPINFOHEADER bmiHeader;
    unsigned int     bmiColors[1];
} BITMAPINFO;

#define BI_RGB         0u
#define DIB_RGB_COLORS 0u
#define SRCCOPY        0x00CC0020u

typedef struct {
    int         calls;
    int         dest_x, dest_y, dest_w, dest_h;
    int         src_x, src_y, src_w, src_h;
    int         bitcount;
    long        bi_width, bi_height;
    const void *bits;
} GDISHIM_REC;

extern GDISHIM_REC g_gdishim;

static inline int StretchDIBits(HDC hdc, int dx, int dy, int dw, int dh,
                                int sx, int sy, int sw, int sh,
                                const void *bits, const BITMAPINFO *bi,
                                unsigned int usage, unsigned int rop)
{
    (void)hdc; (void)usage; (void)rop;
    g_gdishim.calls++;
    g_gdishim.dest_x = dx; g_gdishim.dest_y = dy;
    g_gdishim.dest_w = dw; g_gdishim.dest_h = dh;
    g_gdishim.src_x = sx;  g_gdishim.src_y = sy;
    g_gdishim.src_w = sw;  g_gdishim.src_h = sh;
    g_gdishim.bits = bits;
    if (bi) {
        g_gdishim.bitcount  = (int)bi->bmiHeader.biBitCount;
        g_gdishim.bi_width  = bi->bmiHeader.biWidth;
        g_gdishim.bi_height = bi->bmiHeader.biHeight;
    }
    return 1;
}

#endif /* GDISHIM_H */
