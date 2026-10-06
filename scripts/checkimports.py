#!/usr/bin/env python3
"""列出 PE 的导入 DLL，并标出车机上可能不存在的。

用途：CeGCC 编出来的程序若导入了 GCC 运行库之类的 DLL，
车机上没有就会「一点就闪退」——必须出发前发现。

用法: python3 scripts/checkimports.py 文件.exe
退出码 0 = 所有导入 DLL 都是车机上已知存在的。
"""
import struct, sys

# 车机上已实证存在的（早期探针在真机上 LoadLibrary 成功）
KNOWN_OK = {
    "coredll.dll", "ws2.dll", "iphlpapi.dll", "ddraw.dll", "commctrl.dll",
    "toolhelp.dll", "aygshell.dll", "commdlg.dll", "ole32.dll", "oleaut32.dll",
    "wininet.dll", "msimg32.dll", "ceshell.dll", "cryptoapi.dll", "secur32.dll",
}

def rva2off(sections, rva):
    for va, vsize, praw, rawsize in sections:
        if va <= rva < va + max(vsize, rawsize):
            return praw + (rva - va)
    return None

def main():
    if len(sys.argv) < 2:
        print(__doc__); return 2
    path = sys.argv[1]
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe+4] != b"PE\0\0":
        print(f"❌ {path} 不是 PE"); return 1
    nsec    = struct.unpack_from("<H", d, pe + 6)[0]
    optsize = struct.unpack_from("<H", d, pe + 20)[0]
    opt     = pe + 24
    if struct.unpack_from("<H", d, opt)[0] != 0x10B:
        print(f"❌ {path} 不是 PE32"); return 1

    ndd = struct.unpack_from("<I", d, opt + 92)[0]
    if ndd < 2:
        print(f"  {path}: 没有导入表"); return 0
    imp_rva, _ = struct.unpack_from("<II", d, opt + 96 + 8)

    sec_base = opt + optsize
    sections = []
    for i in range(nsec):
        o = sec_base + i * 40
        vsize, va, rawsize, praw = struct.unpack_from("<IIII", d, o + 8)
        sections.append((va, vsize, praw, rawsize))

    off = rva2off(sections, imp_rva)
    if off is None:
        print(f"  {path}: 导入表 RVA 定位失败"); return 1

    dlls = []
    i = 0
    while True:
        o = off + i * 20
        if o + 20 > len(d):
            break
        oft, tds, fwd, name_rva, ft = struct.unpack_from("<IIIII", d, o)
        if oft == 0 and name_rva == 0 and ft == 0:
            break
        no = rva2off(sections, name_rva)
        if no is None:
            break
        end = d.index(b"\0", no)
        dlls.append(d[no:end].decode("latin-1"))
        i += 1

    bad = [x for x in dlls if x.lower() not in KNOWN_OK]
    print(f"  {path}")
    print(f"    导入 {len(dlls)} 个 DLL: {', '.join(dlls) if dlls else '（无）'}")
    if bad:
        print(f"    ⚠ 车机上未必存在的: {', '.join(bad)}")
        print(f"    → 若程序在车机上闪退，很可能就是这个原因")
    else:
        print(f"    ✅ 全部是车机上已知存在的 DLL")
    return 0

if __name__ == "__main__":
    sys.exit(main())
