/* display.h — 手机画面在车机屏幕上的呈现
 *
 * 设计取舍：不做 CreateDIBSection + 内存 DC + BitBlt 那一套，
 * 直接维护一块 32bpp BGRA 像素缓冲，用 StretchDIBits 一次画上去。
 * 理由：少一个 GDI 对象就少一个老驱动上的坑，而且省一次拷贝。
 *
 * 像素格式选 32bpp 而不是 24bpp：24bpp 的每行要按 4 字节对齐补位，
 * 宽度不是 4 的倍数时容易算错；32bpp 每像素天然对齐，不会出这种错。
 */
#ifndef DISPLAY_H
#define DISPLAY_H

#ifdef DISP_HOST_TEST
/* 电脑上跑时用等价垫片 —— 业务逻辑一行不改，两边共用同一份代码 */
#include "clhost.h"
#include "gdishim.h"
#else
#include <windows.h>
#endif

typedef struct {
    int sw, sh;                   /* 源（手机画面）尺寸 */
    unsigned char *fb;            /* BGRA 像素缓冲，自上而下，sw*sh*4 字节 */
    int fbcap;                    /* 缓冲容量，用于防止尺寸变化时越界 */
    int dw, dh;                   /* 最近一次绘制到的目标尺寸 */
} DISP;

/* 按源尺寸分配缓冲。返回 CL_OK 语义：0 成功，-1 失败 */
int  disp_init(DISP *d, int sw, int sh);
void disp_free(DISP *d);

/* nanojpeg 给的是 RGB24（自上而下、无行填充）→ 转成 BGRA 写进缓冲。
 * 尺寸与缓冲不符时直接拒绝，绝不越界写。 */
void disp_set_rgb24(DISP *d, const unsigned char *rgb, int w, int h);

/* 灰度图（单分量）也支持 —— 有些手机会推灰度 JPEG */
void disp_set_gray8(DISP *d, const unsigned char *gray, int w, int h);

/* BGRA（每像素 4 字节，字节序 B,G,R,A）—— 直接来自 H.264 解码器。
 * h264bsd 的输出字节序和 Windows DIB 的 32bpp 完全一致，所以这里就是一次搬运。 */
void disp_set_bgra(DISP *d, const unsigned char *bgra, int w, int h);

/* 把缓冲按比例铺满客户区 */
void disp_paint(DISP *d, HDC hdc, const RECT *client);

/* 窗口坐标 → 手机画面坐标（触摸回传要用）。
 * 返回 0 表示落在画面内，-1 表示落在画面外（应忽略）。 */
int disp_map_touch(const DISP *d, const RECT *client, int cx, int cy,
                   int *px, int *py);

#endif /* DISPLAY_H */
