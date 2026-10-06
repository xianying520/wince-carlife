#!/usr/bin/env python3
"""验证 src/rsa.c 的 RSA-SHA1 签名实现。

设计要点：C 侧签名，Python 侧用 pow() 独立验签，两边不共用代码 ——
共用代码的话，同一个理解错两次会互相抵消，测了等于没测。
SHA-1 另外对照公开的标准测试向量。
"""
import hashlib
import subprocess
import sys

SHA1_VECTORS = {
    "abc": "a9993e364706816aba3e25717850c26c9cd0d89d",
    "": "da39a3ee5e6b4b0d3255bfef95601890afd80709",
    "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq":
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
}

fails = []


def check(name, ok, extra=""):
    mark = "✅" if ok else "❌"
    print(f"  {mark} {name}" + (f"  {extra}" if extra else ""))
    if not ok:
        fails.append(name)


def main():
    r = subprocess.run(
        ["gcc", "-O2", "-Wall", "-o", "/tmp/test_rsa",
         "host/test_rsa.c", "src/rsa.c"],
        capture_output=True, text=True)
    if r.returncode != 0:
        print("  ❌ 编译失败：")
        print(r.stderr[:2000])
        return 1
    print("  ✅ 编译通过")

    out = subprocess.run(["/tmp/test_rsa"], capture_output=True, text=True,
                         timeout=120).stdout
    vals = {}
    for line in out.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            vals[k] = v.strip()

    # ① SHA-1 对照标准向量
    names = {"abc": "abc", "empty": "", "multi": "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"}
    for key, msg in names.items():
        got = vals.get("SHA1 " + key, "")
        want = SHA1_VECTORS[msg]
        check(f"SHA-1({msg[:20] or '空串'!r}) 与标准向量一致", got == want,
              "" if got == want else f"得到 {got[:24]}… 期望 {want[:24]}…")

    # ② 独立验签
    n = int(vals["N"], 16)
    e = int(vals["E"])
    sig = bytes.fromhex(vals["SIG"])
    msg = bytes.fromhex(vals["SIGNED_MSG"])

    check("签名长度为 256 字节", len(sig) == 256, f"{len(sig)}")

    # 用公开指数还原，看能不能拿回 PKCS#1 v1.5 的填充块
    recovered = pow(int.from_bytes(sig, "big"), e, n).to_bytes(256, "big")
    digest = hashlib.sha1(msg).digest()
    prefix = bytes.fromhex("3021300906052b0e03021a05000414")
    want = b"\x00\x01" + b"\xff" * (256 - 3 - 35) + b"\x00" + prefix + digest

    check("验签：用公开指数还原出正确的填充块", recovered == want,
          "" if recovered == want else f"还原块前 8 字节 {recovered[:8].hex()}")
    check("填充块里的摘要是 msg 的真 SHA-1",
          recovered[-20:] == digest, recovered[-20:].hex())
    check("填充头 00 01 FF…FF 00 正确",
          recovered[:2] == b"\x00\x01" and recovered[2:2 + 218] == b"\xff" * 218
          and recovered[220] == 0)
    check("DigestInfo 前缀是 SHA-1 的标准值",
          recovered[221:236] == prefix)

    # ③ ADB 公钥 blob 的自洽性检查
    blob = bytes.fromhex(vals.get("PUBBLOB", ""))
    check("公钥 blob 长度 524", len(blob) == 524, str(len(blob)))
    if len(blob) == 524:
        words = int.from_bytes(blob[0:4], "little")
        n0inv = int.from_bytes(blob[4:8], "little")
        n_le = int.from_bytes(blob[8:264], "little")
        rr_le = int.from_bytes(blob[264:520], "little")
        ee = int.from_bytes(blob[520:524], "little")
        check("blob 里字数 = 64", words == 64, str(words))
        check("blob 里模数 = 我们的 n", n_le == n)
        check("blob 里指数 = 65537", ee == 65537, str(ee))
        check("blob 里 n0inv = -n^-1 mod 2^32",
              (n0inv * (n & 0xFFFFFFFF)) % (1 << 32) == (1 << 32) - 1,
              f"n0inv=0x{n0inv:08x}")
        r2 = (1 << 4096) % n
        check("blob 里 rr = 2^4096 mod n", rr_le == r2,
              "" if rr_le == r2 else "rr 不对 —— 蒙哥马利初始化有问题")

    print()
    if fails:
        print(f"❌ 共 {len(fails)} 项不符：")
        for f in fails:
            print("   -", f)
        return 1
    print("✅ RSA 签名模块全部通过（含独立验签）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
