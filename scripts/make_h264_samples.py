#!/usr/bin/env python3
"""用 ffmpeg 生成 H.264 测试码流。

思路和本仓库的 make_test_jpeg.py 完全一致：**内容必须完全可预测**，
这样解码结果就能硬断言，而不是"看着还行"。

每个case都是【三帧】纯色图，编码成 Baseline profile。

为什么要三帧而不是一帧：h264bsd 只在【访问单元边界】才吐出上一帧
（源码里 *readBytes = 0 那个分支），所以单帧流解完也不会交帧。
三帧能稳定出 2 帧，足够断言。副作用是永远差一帧延迟，约 70~100ms，
对车机投屏无所谓。
  - 纯色 → 解码后每个像素都该是那个颜色（YUV420 有量化误差，给容差）
  - 红/绿/蓝分开测 → **能抓出 BGRA / RGBA 通道顺序搞反**
  - 尺寸挑一个能整除16的、一个不能的 → **能逼出裁剪路径**
"""
import os
import subprocess
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "samples"

# ⚠ 颜色必须用十六进制写死，不能用 "green" 这种名字：
#   ffmpeg 的 green = (0,128,0)，不是 (0,255,0)。用名字会以为解码错了，
#   实际是期望值写错了（踩过一次：G=128 被判成「颜色错」，其实完全正确）。
CASES = [
    ("red_480x272",   480, 272, "0xFF0000"),
    ("green_480x272", 480, 272, "0x00FF00"),
    ("blue_480x272",  480, 272, "0x0000FF"),
    ("red_480x270",   480, 270, "0xFF0000"),   # 270 不是 16 的倍数 → 必须裁剪
]

def main():
    os.makedirs(OUT, exist_ok=True)
    for name, w, h, color in CASES:
        dst = os.path.join(OUT, name + ".h264")
        cmd = [
            "ffmpeg", "-y", "-loglevel", "error",
            "-f", "lavfi", "-i", "color=c=%s:s=%dx%d:r=15" % (color, w, h),
            "-frames:v", "3",
            "-pix_fmt", "yuv420p",
            "-c:v", "libx264", "-profile:v", "baseline", "-level", "3.0",
            "-x264-params", "keyint=1:min-keyint=1:scenecut=0",
            "-f", "h264", dst,
        ]
        r = subprocess.run(cmd, capture_output=True)
        if r.returncode != 0:
            print("!! ffmpeg 失败 %s\n%s" % (name, r.stderr.decode("utf-8", "replace")))
            return 1
        d = open(dst, "rb").read()
        head = " ".join("%02x" % b for b in d[:8])
        print("  生成 %-16s %6d 字节  首字节 %s" % (name + ".h264", len(d), head))
    return 0

if __name__ == "__main__":
    sys.exit(main())
