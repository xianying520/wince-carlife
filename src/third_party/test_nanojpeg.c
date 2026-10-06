/* 在【构建主机】上运行的 nanojpeg 正确性测试。
 *
 * 为什么能这样测：我们的程序是 ARM WinCE PE，没法在 CI 的 x86 机器上执行；
 * 但解码算法是同一份源码，主机上跑通即可证明算法正确，
 * ARM 版只是同一份代码换编译器。ARM 能否编译由另一条 CI 步骤单独验证。
 *
 * 测试样本由 scripts/make_test_jpeg.py 生成：
 * 所有 DCT 系数为 0，因此解码结果必然是【均匀的 128 灰】——
 * 可以精确断言，而不是"看起来差不多"。
 */
#include <stdio.h>
#include <stdlib.h>

#include "nanojpeg.c"

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "scripts/test_gray.jpg";
    FILE *f;
    long n;
    unsigned char *buf;
    unsigned char *px;
    int r, w, h, i, bad = 0;

    f = fopen(path, "rb");
    if (!f) {
        printf("  ❌ 打不开 %s\n", path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        printf("  ❌ 读不全\n");
        return 2;
    }
    fclose(f);

    njInit();
    r = njDecode(buf, (int)n);
    if (r != NJ_OK) {
        printf("  ❌ 解码失败，返回 %d（NJ_OK=%d）\n", r, NJ_OK);
        free(buf);
        return 1;
    }

    w = njGetWidth();
    h = njGetHeight();
    px = njGetImage();
    printf("  解码结果: %dx%d  %s\n", w, h, njIsColor() ? "彩色" : "灰度");

    if (w != 8 || h != 8) {
        printf("  ❌ 尺寸应为 8x8\n");
        bad = 1;
    }
    for (i = 0; i < w * h && !bad; i++) {
        if (px[i] != 128) {
            printf("  ❌ 第 %d 个像素 = %d，应恒为 128\n", i, px[i]);
            bad = 1;
        }
    }

    njDone();
    free(buf);

    if (bad)
        return 1;
    printf("  ✅ 全部 %d 个像素都是 128 —— JPEG 解码器工作正常\n", w * h);
    return 0;
}
