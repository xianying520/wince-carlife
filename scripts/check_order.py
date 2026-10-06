#!/usr/bin/env python3
"""自查：C 源码里有没有"函数用在了定义之前"。

为什么需要它：C 遇到这种情况只会给一条含糊的隐式声明警告，然后把后面的
static 定义当成"与之前不一致"而报错 —— 报错位置在定义处，真正的问题却在
调用处，很容易看错方向。这个脚本直接指出是哪个函数、用在哪，一眼定位。

（这个坑真实踩过：往 hwdump.c 里插新函数时插到了 has_ci 定义之前。）
"""
import glob
import re
import sys

bad_total = 0

for f in sorted(glob.glob("src/*.c") + glob.glob("host/*.c")):
    src = open(f, encoding="utf-8", errors="replace").read()
    defs = {m.group(1): m.start()
            for m in re.finditer(r"^static\s+[\w \*]+\b(\w+)\s*\(", src, re.M)}
    protos = {m.group(1)
              for m in re.finditer(r"^static\s+[\w \*]+\b(\w+)\s*\([^;]*\);\s*$",
                                   src, re.M)}
    bad = []
    for name, pos in defs.items():
        uses = [m.start() for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", src)
                if m.start() < pos]
        if uses and name not in protos:
            line = src[:uses[0]].count("\n") + 1
            bad.append(f"{name}（第 {line} 行调用，第 {src[:pos].count(chr(10))+1} 行才定义）")
    if bad:
        bad_total += len(bad)
        print(f"❌ {f}:")
        for b in bad:
            print(f"     {b}")
    else:
        print(f"✅ {f}")

print()
if bad_total:
    print(f"❌ 共 {bad_total} 处需要加前置声明")
    sys.exit(1)
print("✅ 没有函数用在定义之前")
