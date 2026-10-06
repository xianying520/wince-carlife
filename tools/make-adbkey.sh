#!/bin/sh
# 生成 ADB 认证用的 RSA-2048 密钥，并转成 C 头文件。
#
# 为什么要硬编码私钥而不是在车机上现场生成：
#   车机算力弱、又没有随机源；而硬编码意味着手机的授权提示只需要点一次，
#   以后插上就能用 —— 对用户来说这是体验上的硬性差别。
#   这个私钥的作用仅仅是"让这台车机被允许用 ADB 连你自己的手机"，
#   泄露它并不能拿到手机上的任何数据（还需要手机端授权）。
set -e
OUT_H="${1:-src/adbkey.h}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

openssl genrsa -out "$TMP/adbkey.pem" 2048 2>/dev/null

# modulus -> 256 字节大端
openssl rsa -in "$TMP/adbkey.pem" -noout -modulus \
  | sed 's/^Modulus=//' | tr -d '\n' > "$TMP/n.hex"
# privateExponent
openssl rsa -in "$TMP/adbkey.pem" -text -noout \
  | awk '/privateExponent:/{f=1;next} /publicExponent:|prime1:|prime2:|exponent1:|exponent2:|coefficient:/{f=0} f' \
  | tr -d ' :\n' > "$TMP/d.hex"

python3 - "$TMP/n.hex" "$TMP/d.hex" "$TMP/adbkey.pem" "$OUT_H" <<'PY'
import sys, hashlib, base64
nh, dh, pem, out = sys.argv[1:5]
n = bytes.fromhex(open(nh).read().strip())
d = bytes.fromhex(open(dh).read().strip())
n = b"\x00"*(256-len(n)) + n
d = b"\x00"*(256-len(d)) + d
assert len(n)==256 and len(d)==256, (len(n), len(d))
assert n[0] & 0x80, "模数必须是满 2048 位（最高位为 1），否则简化过的约减逻辑不成立"

def arr(b):
    return "\n".join("    " + ", ".join(f"0x{x:02X}" for x in b[i:i+12]) + "," for i in range(0,len(b),12))

open(out,"w",encoding="utf-8").write(f'''/* adbkey.h — ADB 认证用的 RSA-2048 私钥（由 tools/make-adbkey.sh 生成，请勿手改）
 *
 * 为什么硬编码：见 tools/make-adbkey.sh 顶部说明。手机第一次连接会弹
 * 「允许 USB 调试吗？」，用户点一次之后就不再问了。
 *
 * 指纹（用于人工核对，与 SHA-1 无关）：
 *   n 的前 8 字节 = {" ".join(f"{x:02X}" for x in n[:8])}
 */
#ifndef ADBKEY_H
#define ADBKEY_H

/* 模数 n，256 字节，大端 */
static const unsigned char ADB_KEY_N[256] = {{
{arr(n)}
}};

/* 私钥指数 d，256 字节，大端 */
static const unsigned char ADB_KEY_D[256] = {{
{arr(d)}
}};

/* 公开指数，固定 65537 */
#define ADB_KEY_E 65537

#endif
''')
print(f"  ✅ 已写 {out}")
print(f"     n 最高字节 0x{n[0]:02X}（满 2048 位 ✓）")
PY

openssl rsa -in "$TMP/adbkey.pem" -pubout -out keys/adbkey.pub.pem 2>/dev/null
cp "$TMP/adbkey.pem" keys/adbkey.pem
chmod 600 keys/adbkey.pem
echo "  ✅ 钥匙已备份到 keys/（.pem 不进仓库，.h 进）"
