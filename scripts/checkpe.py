#!/usr/bin/env python3
"""检查一个 PE 是不是可用的 WinCE 6.0 ARM GUI 程序。
用法: python3 scripts/checkpe.py 文件.exe [--quiet]
退出码 0 = 合格，1 = 不合格。"""
import struct, sys

def main():
    if len(sys.argv) < 2:
        print(__doc__); return 2
    path = sys.argv[1]
    quiet = "--quiet" in sys.argv
    # 干净退出，让调用方拿到明确的真值
    try:
        d = open(path, "rb").read()
    except OSError as e:
        print(f"  ❌ 读不到 {path}: {e}"); return 1
    if len(d) < 0x100:
        print(f"  ❌ {path} 太小 ({len(d)} 字节)"); return 1
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if pe + 0x60 > len(d) or d[pe:pe+4] != b"PE\0\0":
        print(f"  ❌ {path} 不是 PE"); return 1
    mach, nsec = struct.unpack_from("<HH", d, pe + 4)
    magic  = struct.unpack_from("<H", d, pe + 24)[0]
    subsys = struct.unpack_from("<H", d, pe + 24 + 68)[0]
    ok = (mach == 0x01C2 and magic == 0x10B and subsys == 9)
    if not quiet:
        print(f"  {'✅' if ok else '❌'} {len(d):>7} 字节  Machine=0x{mach:04x}  "
              f"Magic=0x{magic:x}  节={nsec}  Subsystem={subsys}")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
