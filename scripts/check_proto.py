#!/usr/bin/env python3
"""用 Baidu 官方 .proto 文件核对我们的 protobuf 编码。

为什么要这个：此前所有验证都是"代码是否符合我的理解"。这个脚本换一个方向——
拿【权威规格】（CarLife 官方仓库里的 .proto 文件）来核对代码。

能抓到的错：字段号写错、字段顺序颠倒、包头的 wire type 算错、
消息 ID 抄错。这些都是"代码看起来对但真机上手机不理你"的典型原因。
"""
import os
import re
import sys

PROTO_DIR = "docs/carlife-proto"
CARLIFE_C = "src/carlife.c"
CARLIFE_H = "src/carlife.h"

fails = []


def rd(p):
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read()


def proto_fields(msg):
    """从 .proto 里取出消息的字段：(字段号, 字段名, 类型)"""
    for fn in sorted(os.listdir(PROTO_DIR)):
        if not fn.endswith(".proto"):
            continue
        s = rd(os.path.join(PROTO_DIR, fn))
        m = re.search(r"message\s+" + re.escape(msg) + r"\s*\{(.*?)\n\}", s, re.S)
        if not m:
            continue
        out = []
        for ln in m.group(1).splitlines():
            f = re.match(r"\s*(required|optional|repeated)?\s*"
                         r"(int32|int64|uint32|uint64|sint32|bool|string|bytes|enum|\w+)\s+"
                         r"(\w+)\s*=\s*(\d+)\s*;", ln)
            if f:
                out.append((int(f.group(4)), f.group(3), f.group(2)))
        return out
    return None


# ── ① 逐函数核对字段号 ──
# (C 函数名, proto 消息名)
CHECKS = [
    ("cl_send_video_encoder_init", "CarlifeVideoEncoderInfo"),
    ("cl_send_hard_key",           "CarlifeCarHardKeyCode"),
    # 触摸单独核对：它有两套写法，分别对应两个不同的 proto 消息（见 ①b / ①c）
]

src_c = rd(CARLIFE_C)

print("══ ① protobuf 字段号 vs 官方 .proto ══")
for func, msg in CHECKS:
    m = re.search(re.escape(func) + r"\s*\([^)]*\)\s*\{(.*?)\n\}", src_c, re.S)
    if not m:
        fails.append(f"找不到函数 {func}")
        print(f"  ❌ {func}: 找不到")
        continue
    body = m.group(1)
    used = [int(x) for x in re.findall(r"pb_int32\(\s*[^,]+,\s*(\d+)\s*,", body)]
    pf = proto_fields(msg)
    if pf is None:
        fails.append(f"proto 里没有 {msg}")
        print(f"  ❌ {msg}: proto 里找不到")
        continue
    want = [n for n, _, _ in pf]
    ok = (used == want)
    if not ok:
        fails.append(f"{func}: 字段号 {used} != proto {want}")
    print(f"  {'✅' if ok else '❌'} {func}")
    print(f"       我们发的字段号 {used}")
    print(f"       proto 声明      {want}  ({', '.join(n for _, n, _ in pf)})")
    # 参数名与字段名的一致性（提示性质，不一致只提醒不判失败）
    args = m.group(0)[m.group(0).index("(") + 1:m.group(0).index(")")]
    anames = [a.strip().split()[-1].lstrip("*")
              for a in args.split(",") if a.strip() and "SOCKET" not in a]
    # 参数名与字段名的同义词（只用于避免误报，不参与判定）
    ALIAS = {"fps": "framerate", "w": "width", "h": "height",
             "key": "keycode", "code": "keycode"}
    if len(anames) == len(pf):
        def norm(x):
            x = x.lower()
            return ALIAS.get(x, x)
        mism = [(a, n) for a, (_, n, _) in zip(anames, pf)
                if norm(a) not in n.lower() and n.lower() not in norm(a)]
        if mism:
            print(f"       ⚠ 参数名与字段名对不上，人工确认一下: {mism}")

# cl_send_touch_action 有两套写法，分别对应两个不同的 proto 消息
m = re.search(r"int\s+cl_send_touch_action\s*\([^)]*\)\s*\{(.*?)\n\}", src_c, re.S)
if m:
    body = m.group(1)
    # 写法 A：只用字段 1,2（CarlifeTouchSinglePoint 是 x=1,y=2）
    pa = proto_fields("CarlifeTouchSinglePoint")
    wantA = [n for n, _, _ in pa]
    segA = body[body.index("写法 A"):] if "写法 A" in body else body
    usedA = [int(x) for x in re.findall(r"pb_int32\(\s*[^,]+,\s*(\d+)\s*,", segA)]
    print()
    print("══ ①b 触摸写法 A（专用消息 + 单点坐标）══")
    oka = (usedA == wantA)
    if not oka:
        fails.append(f"触摸写法A 字段 {usedA} != {wantA}")
    print(f"  {'✅' if oka else '❌'} 我们发的 {usedA}  proto 声明 {wantA}"
          f"  ({', '.join(n for _, n, _ in pa)})")

    # 写法 B：字段 1,2,3（CarlifeTouchAction）
    pb_ = proto_fields("CarlifeTouchAction")
    wantB = [n for n, _, _ in pb_]
    segB = body[body.index("写法 B"):body.index("写法 A")] if "写法 A" in body else body
    usedB = [int(x) for x in re.findall(r"pb_int32\(\s*[^,]+,\s*(\d+)\s*,", segB)]
    print("══ ①c 触摸写法 B（通用消息 + 动作坐标）══")
    okb = (usedB == wantB)
    if not okb:
        fails.append(f"触摸写法B 字段 {usedB} != {wantB}")
    print(f"  {'✅' if okb else '❌'} 我们发的 {usedB}  proto 声明 {wantB}"
          f"  ({', '.join(nm for _, nm, _ in pb_)})")

# 触摸包头的确认依据
m = re.search(r"send_packet\(s,\s*id,\s*body,\s*n,\s*(\w+)\)", src_c)
if m and m.group(1) != "CL_HDR_CMD":
    fails.append(f"触摸用的包头 {m.group(1)} 应为 CL_HDR_CMD")
print()
print(f"  {'✅' if m and m.group(1) == 'CL_HDR_CMD' else '❌'} "
      f"触摸走 CTRL 通道（参考源码 CTRL_HEAD_LEN 8 + 注释 'ctrol channel [HU->MD]'）")

# ── ② 核对握手那段手写字节的 tag ──
print()
print("══ ② 握手的 wire type tag ══")
pf = proto_fields("CarlifeProtocolVersion")
hdr_body = re.findall(r"body\[0\]\s*=\s*(0x[0-9A-Fa-f]+);\s*\n\s*body\[1\]\s*=[^;]+;\s*\n"
                      r"\s*body\[2\]\s*=\s*(0x[0-9A-Fa-f]+)\s*;", src_c)
if not hdr_body:
    fails.append("找不到握手的手写字节")
    print("  ❌ 找不到那段手写字节")
else:
    for t1, t2 in hdr_body:
        v1, v2 = int(t1, 16), int(t2, 16)
        # tag = (field << 3) | wire_type ; wire type 0 = varint
        e1 = (pf[0][0] << 3) | 0
        e2 = (pf[1][0] << 3) | 0
        ok = (v1 == e1 and v2 == e2)
        if not ok:
            fails.append(f"握手 tag {t1}/{t2} 应为 {e1:#04x}/{e2:#04x}")
        print(f"  {'✅' if ok else '❌'} tag {t1}={e1:#04x}  tag {t2}={e2:#04x}"
              f"   (字段 {pf[0][0]}/{pf[1][0]}, wire type varint)")

# ── ③ 核对消息 ID ──
# 期望值来自 CarLife 官方实现 LibSource/include/CTranRecvPackageProcess.h
EXPECT_IDS = {
    "CL_MSG_HU_PROTOCOL_VERSION":        0x00018001,
    "CL_MSG_PROTOCOL_VERSION_MATCH":     0x00010002,
    "CL_MSG_VIDEO_ENCODER_INIT":         0x00018007,
    "CL_MSG_VIDEO_ENCODER_INIT_DONE":    0x00010008,
    "CL_MSG_VIDEO_ENCODER_START":        0x00018009,
    "CL_MSG_VIDEO_ENCODER_PAUSE":        0x0001800A,
    "CL_MSG_VIDEO_ENCODER_RESET":        0x0001800B,
    "CL_MSG_VIDEO_ENCODER_FPS_CHANGE":   0x0001800C,
    "CL_MSG_VIDEO_ENCODER_JPEG":         0x00018056,
    "CL_MSG_VIDEO_ENCODER_JPEG_ACK":     0x00010057,
    "CL_MSG_TOUCH_ACTION":               0x00068001,
    "CL_MSG_TOUCH_ACTION_DOWN":          0x00068002,
    "CL_MSG_TOUCH_ACTION_UP":            0x00068003,
    "CL_MSG_TOUCH_ACTION_MOVE":          0x00068004,
    "CL_MSG_TOUCH_CAR_HARD_KEY":         0x00068008,
}
src_h = rd(CARLIFE_H)
print()
print("══ ③ 消息 ID vs 官方实现 ══")
for name, want in sorted(EXPECT_IDS.items()):
    m = re.search(r"#define\s+" + name + r"\s+(0x[0-9A-Fa-f]+)", src_h)
    if not m:
        fails.append(f"carlife.h 缺 {name}")
        print(f"  ❌ {name}: 没定义")
        continue
    got = int(m.group(1), 16)
    ok = (got == want)
    if not ok:
        fails.append(f"{name} = {got:#010x} 应为 {want:#010x}")
    print(f"  {'✅' if ok else '❌'} {name:<34} {got:#010x}"
          + ("" if ok else f"  应为 {want:#010x}"))

# ── ④ 包头长度 ──
print()
print("══ ④ 包头长度 ══")
for name, want, why in (("CL_HDR_CMD", 8, "控制/触摸通道"),
                        ("CL_HDR_MEDIA", 12, "视频/音频/语音通道")):
    m = re.search(r"#define\s+" + name + r"\s+(\d+)", src_h)
    got = int(m.group(1)) if m else -1
    ok = (got == want)
    if not ok:
        fails.append(f"{name}={got} 应为 {want}")
    print(f"  {'✅' if ok else '❌'} {name:<14} = {got}  ({why})")

print()
if fails:
    print(f"❌ 共 {len(fails)} 项不符：")
    for f in fails:
        print("   -", f)
    sys.exit(1)
print("✅ 全部与官方规格一致")
