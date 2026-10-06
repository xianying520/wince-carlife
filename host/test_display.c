/* test_display.c — 验证画面呈现这一层
 *
 * 这一层管的正是"车里能不能用"的两个直接感受：
 *   · 像素格式对不对（RGB/BGR 弄反 → 整个画面颜色是错的）
 *   · 触摸坐标换算对不对（错了 → 点哪儿都不准）
 * 两者都是纯逻辑，可以在电脑上精确验证。
 */
#include "clhost.h"
#include "gdishim.h"
#include "display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GDISHIM_REC g_gdishim;

static int fails = 0;

static void check(const char *name, int ok, const char *extra)
{
    printf("  %s %s%s%s\n", ok ? "OK  " : "FAIL", name,
           extra && extra[0] ? "  " : "", extra ? extra : "");
    if (!ok) fails++;
}

int main(void)
{
    DISP d;
    memset(&d, 0, sizeof(d));

    printf("── disp_init ──\n");
    check("32x16 分配成功", disp_init(&d, 32, 16) == 0, "");
    check("尺寸记对了", d.sw == 32 && d.sh == 16, "");
    check("缓冲够大", d.fbcap >= 32 * 16 * 4, "");

    printf("── RGB24 → BGRA（颜色顺序）──\n");
    {
        /* 两个像素：纯红 (255,0,0) 和纯蓝 (0,0,255) */
        unsigned char rgb[6];
        unsigned char *f = d.fb;
        memset(f, 0xAB, (size_t)d.fbcap);       /* 先填脏数据 */

        /* 造一整幅 32x16：每个像素都是同一个值，便于验证 */
        {
            int i;
            static unsigned char full[32 * 16 * 3];
            for (i = 0; i < 32 * 16; i++) {
                full[i * 3 + 0] = 255;          /* R */
                full[i * 3 + 1] = 0;            /* G */
                full[i * 3 + 2] = 0;            /* B */
            }
            disp_set_rgb24(&d, full, 32, 16);
        }
        rgb[0] = f[0]; rgb[1] = f[1]; rgb[2] = f[2]; rgb[3] = f[3];
        /* 纯红在 BGRA 里应该是 B=0 G=0 R=255 A=0 */
        check("纯红 → B=0 G=0 R=255 A=0",
              rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 255 && rgb[3] == 0,
              "");
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "实得 B=%d G=%d R=%d A=%d",
                     rgb[0], rgb[1], rgb[2], rgb[3]);
            check("（颜色顺序没有反）", rgb[2] == 255, buf);
        }
        {
            char buf[200];
            int k, bad = 0, firstbad = -1;
            for (k = 0; k < 32 * 16; k++)
                if (f[k * 4 + 2] != 255) {
                    bad++;
                    if (firstbad < 0) firstbad = k;
                }
            snprintf(buf, sizeof(buf),
                     "fb=%p f=%p sw=%d sh=%d cap=%d | f[2]=%d f[2046]=%d | "
                     "R!=255 的像素 %d 个，第一个是第 %d 个",
                     (void *)d.fb, (void *)f, d.sw, d.sh, d.fbcap,
                     f[2], f[2046], bad, firstbad);
            check("最后一个像素也写到了", f[(32 * 16 - 1) * 4 + 2] == 255, buf);
        }
    }

    printf("── 尺寸不符时拒绝写入（防越界）──\n");
    {
        unsigned char *f = d.fb;
        unsigned char before = f[0];
        static unsigned char small[8 * 8 * 3];
        memset(small, 0x77, sizeof(small));
        disp_set_rgb24(&d, small, 8, 8);        /* 尺寸不对 */
        check("拒绝写入、缓冲未被改动", f[0] == before, "");

        memset(f, 0xAB, (size_t)d.fbcap);
        disp_set_gray8(&d, small, 8, 8);
        check("灰度路径同样拒绝", f[0] == 0xAB, "");
    }

    printf("── 灰度 → BGRA ──\n");
    {
        static unsigned char g[32 * 16];
        unsigned char *f = d.fb;
        memset(g, 200, sizeof(g));
        disp_set_gray8(&d, g, 32, 16);
        check("灰度值铺到 B/G/R 三个通道",
              f[0] == 200 && f[1] == 200 && f[2] == 200 && f[3] == 0, "");
    }

    printf("── 铺到屏幕（StretchDIBits 参数）──\n");
    {
        RECT cl;
        cl.left = 0; cl.top = 22; cl.right = 320; cl.bottom = 240 + 22;
        memset(&g_gdishim, 0, sizeof(g_gdishim));
        disp_paint(&d, (HDC)0, &cl);
        check("确实画了一次", g_gdishim.calls == 1, "");
        check("目标 = 整个客户区（左上角没跑偏）",
              g_gdishim.dest_x == 0 && g_gdishim.dest_y == 22 &&
              g_gdishim.dest_w == 320 && g_gdishim.dest_h == 240, "");
        check("源 = 整幅手机画面",
              g_gdishim.src_w == 32 && g_gdishim.src_h == 16, "");
        check("32bpp / 负高度（自上而下，与我们缓冲方向一致）",
              g_gdishim.bitcount == 32 && g_gdishim.bi_height == -16, "");
        check("画的就是我们那块缓冲", g_gdishim.bits == (const void *)d.fb, "");
    }

    printf("── 触摸坐标换算 ──\n");
    {
        RECT cl;
        int px = -1, py = -1;
        cl.left = 0; cl.top = 0; cl.right = 320; cl.bottom = 160;

        check("左上角 → (0,0)",
              disp_map_touch(&d, &cl, 0, 0, &px, &py) == 0 &&
              px == 0 && py == 0, "");

        check("正中心 → (16,8)",
              disp_map_touch(&d, &cl, 160, 80, &px, &py) == 0 &&
              px == 16 && py == 8, "");

        /* 320/32 = 10，160/16 = 10 → 每 10 个屏幕像素对应 1 个手机像素 */
        check("x=155 → 15（按比例向下取整）",
              disp_map_touch(&d, &cl, 155, 10, &px, &py) == 0 && px == 15 && py == 1,
              "");

        px = py = -1;
        check("右下角边界外 → 拒绝",
              disp_map_touch(&d, &cl, 320, 160, &px, &py) == -1, "");
        check("负数坐标 → 拒绝",
              disp_map_touch(&d, &cl, -1, 5, &px, &py) == -1, "");

        /* 客户区有偏移时（比如上方留了状态栏），必须减掉偏移 */
        cl.left = 10; cl.top = 30; cl.right = 330; cl.bottom = 190;
        check("客户区带偏移时也要算对",
              disp_map_touch(&d, &cl, 170, 110, &px, &py) == 0 &&
              px == 16 && py == 8, "");
        check("偏移区之外的坐标按客户区判定",
              disp_map_touch(&d, &cl, 5, 110, &px, &py) == -1, "");
    }

    printf("── 换分辨率后触摸换算跟着变 ──\n");
    {
        RECT cl;
        int px = -1, py = -1;
        cl.left = 0; cl.top = 0; cl.right = 640; cl.bottom = 480;
        check("切到 640x480 分配成功", disp_init(&d, 640, 480) == 0, "");
        check("换算用新尺寸",
              disp_map_touch(&d, &cl, 320, 240, &px, &py) == 0 &&
              px == 320 && py == 240, "");
    }

    disp_free(&d);
    check("释放后清空", d.fb == 0 && d.sw == 0 && d.sh == 0, "");

    printf("\n");
    if (fails) {
        printf("显示层验证失败：%d 项\n", fails);
        return 1;
    }
    printf("显示层验证通过（像素格式 + 铺屏参数 + 触摸换算）\n");
    return 0;
}
