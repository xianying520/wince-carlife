#!/usr/bin/env python3
"""编译并运行「设备层回归测试」：真实 src/adbio_ce.c 对假车机 ADB 驱动。

为什么要有它：过去四趟车踩的坑全在 ADB/设备这一层，而它们
【在电脑上跑一遍就能发现】。这个脚本就是让它们以后在 CI 上被发现，
而不是又花一趟车。

⚠ 被测代码里那句 ReadFile 是【真的会阻塞】的（假驱动照实现场行为）。
  万一哪天有人把读线程那层拆了，这个测试会【挂住】而不是失败 ——
  所以这里必须带看门狗：超时直接判失败，并打印卡在哪一项后面。
"""
import os
import subprocess
import sys
import time

SRC = "host/test_ce_dev.c"
BIN = "/tmp/test_ce_dev"
TIMEOUT = 60

def main():
    cc = os.environ.get("CC", "gcc")
    cmd = [cc, "-O1", "-g", "-Wall", "-Ihost", "-Isrc",
           "-o", BIN, SRC, "src/adbio_ce.c", "src/adb.c", "src/rsa.c", "-lpthread"]
    print("  ── 编译 ──")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.stderr.strip():
        for line in r.stderr.strip().splitlines():
            print("   ", line)
    if r.returncode != 0:
        print("❌ 编译失败")
        return 1

    print("  ── 运行（看门狗 %d 秒）──" % TIMEOUT)
    t0 = time.time()
    try:
        p = subprocess.Popen([BIN], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True)
        out, _ = p.communicate(timeout=TIMEOUT)
        elapsed = time.time() - t0
    except subprocess.TimeoutExpired:
        p.kill()
        out, _ = p.communicate()
        print(out or "")
        print("❌❌ 被测程序【卡住了】（超过 %d 秒没结束）" % TIMEOUT)
        print("    这就是「阻塞读把上层卡死」那一类 bug —— 读线程那一层没了或者坏了。")
        return 1

    for line in out.splitlines():
        print("  " + line)
    print("  用时 %.1f 秒" % elapsed)
    if p.returncode != 0:
        print("❌ 设备层回归测试未通过")
        return 1
    print("✅ 设备层回归测试通过（含「缓冲灌满后关设备不会卡住」）")
    return 0

if __name__ == "__main__":
    sys.exit(main())
