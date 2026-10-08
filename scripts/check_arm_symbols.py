#!/usr/bin/env python3
"""检查 ARM 目标文件集里的符号是否闭合。

为什么需要这个：交叉编译时如果只做 `-c` 再 `ar` 打包，**从不做符号解析**，
所以「函数名写错」这类错会一路漏过去，直到车机上才炸。
（实锤：h264dec.c 里把 h264dec_profile_name 写成了 h264bsd_profile_name，
ARM 那一路全绿，主机链接时才暴露。）

用法：nm 的输出从 stdin 喂进来，或者给一个文件路径。
只关心本项目自己的符号（h264dec_* / h264bsd_*），
libc/coredll 那些外部符号不管。
"""
import re
import sys

PREFIXES = ("h264dec_", "h264bsd_")

def parse(lines):
    defined, undef = set(), set()
    for line in lines:
        parts = line.split()
        if not parts:
            continue
        if len(parts) >= 3:
            typ, name = parts[-2], parts[-1]
        elif len(parts) == 2:
            typ, name = parts[0], parts[1]
        else:
            continue
        if typ == "U":
            undef.add(name)
        elif typ in "TDBRGSVW":
            defined.add(name)
    return defined, undef

def main():
    if len(sys.argv) > 1:
        lines = open(sys.argv[1]).read().splitlines()
    else:
        lines = sys.stdin.read().splitlines()

    defined, undef = parse(lines)
    mine_undef = sorted(n for n in undef if n.startswith(PREFIXES))
    missing = sorted(n for n in mine_undef if n not in defined)

    print("  本项目已定义符号: %d 个" % len([n for n in defined if n.startswith(PREFIXES)]))
    print("  本项目未定义符号: %d 个" % len(mine_undef))
    if missing:
        print("  ❌ 缺符号（名字写错或漏编译）:")
        for m in missing:
            print("       %s" % m)
        return 1
    print("  ✅ 符号闭合 —— 本项目范围内没有悬空引用")
    return 0

if __name__ == "__main__":
    sys.exit(main())
