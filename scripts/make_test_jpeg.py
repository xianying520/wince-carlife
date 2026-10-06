#!/usr/bin/env python3
"""生成一个「结果完全可预测」的最小基线 JPEG，用于验证解码器。

设计要点：
  * 所有 DCT 系数为 0（DC=0，AC 全 EOB）
    → 反量化后全 0 → 加 128 电平偏移 → **整幅图必然是均匀的 128 灰**
    这样解码结果可以被精确断言，而不是「看起来差不多」。
  * Huffman 表由本文件自带（JPEG 允许自定义 DHT，不必用标准表）
    → 只需「1 位长的 1 个码字」，实现极简。

用法: python3 scripts/make_test_jpeg.py [输出路径] [宽] [高]
输出默认 scripts/test_gray.jpg
"""
import struct
import sys


def seg(marker, payload):
    return bytes([0xFF, marker]) + struct.pack(">H", len(payload) + 2) + payload


def make_jpeg(w=8, h=8):
    out = bytearray(b"\xff\xd8")                                 # SOI

    # DQT：8 位精度、表号 0、全部为 1（反正系数都是 0，量化表无影响）
    out += seg(0xDB, bytes([0x00]) + bytes([1] * 64))

    # SOF0：精度 8、高、宽、1 个分量、id=1、采样 1x1、量化表 0
    out += seg(0xC0, bytes([8]) + struct.pack(">HH", h, w) +
               bytes([1, 1, 0x11, 0]))

    # DHT（DC 表 0）：1 个长度为 1 的码字，符号 0x00（DC 差值类别 0）
    out += seg(0xC4, bytes([0x00]) + bytes([1] + [0]*15) + bytes([0x00]))

    # DHT（AC 表 0）：1 个长度为 1 的码字，符号 0x00（EOB，块结束）
    out += seg(0xC4, bytes([0x10]) + bytes([1] + [0]*15) + bytes([0x00]))

    # SOS：1 个分量、DC/AC 表都用 0、Ss=0 Se=63 AhAl=0
    out += seg(0xDA, bytes([1, 1, 0x00, 0, 63, 0]))

    # 熵编码数据：每块 = DC(码字 '0') + AC-EOB(码字 '0') = 两位 '00'
    # 末尾补 1 到位边界 → 0b00111111 = 0x3F
    nblocks = ((w + 7) // 8) * ((h + 7) // 8)
    bits = "00" * nblocks
    bits += "1" * ((8 - len(bits) % 8) % 8)
    data = bytearray()
    for i in range(0, len(bits), 8):
        data.append(int(bits[i:i+8], 2))
    # JPEG 要求：数据里出现 0xFF 要跟一个 0x00
    stuffed = bytearray()
    for b in data:
        stuffed.append(b)
        if b == 0xFF:
            stuffed.append(0x00)
    out += bytes(stuffed)

    out += b"\xff\xd9"                                           # EOI
    return bytes(out), w, h


def main():
    dst = sys.argv[1] if len(sys.argv) > 1 else "scripts/test_gray.jpg"
    w = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    h = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    blob, w, h = make_jpeg(w, h)
    open(dst, "wb").write(blob)
    print(f"✅ 已生成 {dst}  {len(blob)} 字节  {w}x{h}")
    print(f"   解码后预期：每个像素都是 128（均匀灰），共 {w*h} 个像素")
    return 0


if __name__ == "__main__":
    sys.exit(main())
