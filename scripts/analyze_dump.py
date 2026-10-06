#!/usr/bin/env python3
"""分析车机带回来的 video-dump.bin。

用法:
    python3 scripts/analyze_dump.py video-dump.bin [client-result.txt]

能回答三个关键问题（不用再跑一趟车机）：
  1. 手机推的到底是 JPEG 还是 H.264
  2. 如果是 H.264，真实分辨率/档次是多少 —— 直接解析 SPS
  3. 帧的分布：大小区间、有没有超大帧（会撞缓冲区上限）

帧边界从 client-result.txt 里的「帧N: X 字节 type=0x.. 偏移=Y」读出来。
"""
import re
import struct
import sys


# ───────────────────────── 位读取（H.264 用） ─────────────────────────
class BitReader:
    def __init__(self, data):
        self.d = data
        self.pos = 0

    def bit(self):
        if self.pos >> 3 >= len(self.d):
            raise EOFError
        b = (self.d[self.pos >> 3] >> (7 - (self.pos & 7))) & 1
        self.pos += 1
        return b

    def bits(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | self.bit()
        return v

    def ue(self):
        """无符号指数哥伦布"""
        zeros = 0
        while self.bit() == 0:
            zeros += 1
            if zeros > 32:
                raise ValueError("ue 太长")
        return (1 << zeros) - 1 + (self.bits(zeros) if zeros else 0)

    def se(self):
        """有符号指数哥伦布"""
        k = self.ue()
        return (k + 1) // 2 if k % 2 else -(k // 2)


def unescape_h264(payload):
    """去掉防竞争字节 00 00 03 → 00 00"""
    out = bytearray()
    zeros = 0
    i = 0
    while i < len(payload):
        b = payload[i]
        if zeros >= 2 and b == 3:
            zeros = 0
            i += 1
            continue
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
        i += 1
    return bytes(out)


def parse_sps(sps_nal):
    """sps_nal 含 NAL 头（去掉起始码后的第一个字节）。返回 (宽, 高, profile, level) 或 None"""
    if len(sps_nal) < 4:
        return None
    profile = sps_nal[1]
    level = sps_nal[3]
    r = BitReader(unescape_h264(sps_nal[1:]))
    try:
        r.bits(8)                      # profile_idc
        r.bits(8)                      # constraint flags
        r.bits(8)                      # level_idc
        r.ue()                         # seq_parameter_set_id
        chroma_format_idc = 1
        if profile in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
            chroma_format_idc = r.ue()
            if chroma_format_idc == 3:
                r.bit()                # separate_colour_plane_flag
            r.ue()                     # bit_depth_luma_minus8
            r.ue()                     # bit_depth_chroma_minus8
            r.bit()                    # qpprime_y_zero_transform_bypass_flag
            if r.bit():                # seq_scaling_matrix_present_flag
                n = 8 if chroma_format_idc != 3 else 12
                for i in range(n):
                    if r.bit():        # seq_scaling_list_present_flag
                        size = 16 if i < 6 else 64
                        last, nxt = 8, 8
                        for _ in range(size):
                            if nxt != 0:
                                nxt = (last + r.se() + 256) % 256
                            last = nxt if nxt != 0 else last
        r.ue()                         # log2_max_frame_num_minus4
        poc_type = r.ue()
        if poc_type == 0:
            r.ue()                     # log2_max_pic_order_cnt_lsb_minus4
        elif poc_type == 1:
            r.bit()                    # delta_pic_order_always_zero_flag
            r.se()                     # offset_for_non_ref_pic
            r.se()                     # offset_for_top_to_bottom_field
            for _ in range(r.ue()):    # num_ref_frames_in_pic_order_cnt_cycle
                r.se()
        r.ue()                         # max_num_ref_frames
        r.bit()                        # gaps_in_frame_num_value_allowed_flag
        w_mbs = r.ue() + 1
        h_units = r.ue() + 1
        frame_mbs_only = r.bit()
        if not frame_mbs_only:
            r.bit()                    # mb_adaptive_frame_field_flag
        r.bit()                        # direct_8x8_inference_flag
        crop_l = crop_r = crop_t = crop_b = 0
        if r.bit():                    # frame_cropping_flag
            crop_l, crop_r, crop_t, crop_b = r.ue(), r.ue(), r.ue(), r.ue()
        sub_w = 2 if chroma_format_idc in (1, 2) else 1
        sub_h = 2 if chroma_format_idc == 1 else 1
        width = w_mbs * 16 - (crop_l + crop_r) * sub_w
        height = (2 - frame_mbs_only) * h_units * 16 - (crop_t + crop_b) * sub_h
        return width, height, profile, level
    except (EOFError, ValueError):
        return None


def split_nals(data):
    """按起始码切 NAL，返回 [(nal_type, payload)]"""
    out = []
    i = 0
    n = len(data)
    starts = []
    while i < n - 3:
        if data[i] == 0 and data[i+1] == 0:
            if data[i+2] == 1:
                starts.append((i, 3))
                i += 3
                continue
            if i < n - 4 and data[i+2] == 0 and data[i+3] == 1:
                starts.append((i, 4))
                i += 4
                continue
        i += 1
    for k, (off, sc) in enumerate(starts):
        end = starts[k+1][0] if k + 1 < len(starts) else n
        payload = data[off+sc:end]
        if payload:
            out.append((payload[0] & 0x1F, payload))
    return out


# ───────────────────────── JPEG ─────────────────────────
def jpeg_info(data):
    """从 SOI 里读尺寸/分量数/是否渐进式"""
    if data[:2] != b"\xff\xd8":
        return None
    i = 2
    while i + 4 <= len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        m = data[i+1]
        if m in (0xD8, 0xD9) or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        seglen = struct.unpack_from(">H", data, i+2)[0]
        if m in (0xC0, 0xC1, 0xC2, 0xC3):
            h, w = struct.unpack_from(">HH", data, i+5)
            comps = data[i+9]
            kind = {0xC0: "基线", 0xC1: "扩展顺序", 0xC2: "渐进式", 0xC3: "无损"}[m]
            return dict(w=w, h=h, comps=comps, kind=kind)
        i += 2 + seglen
    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    blob = open(sys.argv[1], "rb").read()
    print(f"══ {sys.argv[1]}: {len(blob)} 字节 ══")

    # 解析帧边界
    frames = []
    if len(sys.argv) > 2:
        txt = open(sys.argv[2], "rb").read().decode("utf-16", "replace")
        for m in re.finditer(r"帧(\d+): (\d+) 字节 type=0x([0-9a-f]+) 偏移=(\d+)", txt):
            frames.append(dict(n=int(m.group(1)), size=int(m.group(2)),
                               type=int(m.group(3), 16), off=int(m.group(4))))
        print(f"从结果文件读到 {len(frames)} 条帧记录")
        if frames:
            print(f"  帧类型: {sorted(set(hex(f['type']) for f in frames))}")

    # ── 编码判定 ──
    if blob[:2] == b"\xff\xd8":
        print("\n【判定】JPEG（MJPEG 流）")
        info = jpeg_info(blob)
        if info:
            print(f"  分辨率 {info['w']}x{info['h']}  分量 {info['comps']}  类型 {info['kind']}")
        eoi = blob.find(b"\xff\xd9")
        if eoi > 0:
            print(f"  第一帧完整长度约 {eoi+2} 字节")
    elif blob[:4] == b"\x00\x00\x00\x01" or blob[:3] == b"\x00\x00\x01":
        print("\n【判定】H.264")
        nals = split_nals(blob)
        print(f"  共 {len(nals)} 个 NAL，前 12 个类型:")
        for t, p in nals[:12]:
            names = {1: "slice(非IDR)", 5: "slice(IDR)", 6: "SEI", 7: "SPS",
                     8: "PPS", 9: "AUD"}
            print(f"    type={t:<2} {names.get(t,''):<12} {len(p)} 字节")
        sps = [p for t, p in nals if t == 7]
        if sps:
            got = parse_sps(sps[0])
            if got:
                w, h, prof, lvl = got
                print(f"\n  ⭐ SPS 解析成功：分辨率 {w}x{h}  profile={prof} level={lvl}")
                if prof == 66:
                    print("     profile 66 = Baseline（无 B 帧，软解最容易）")
                elif prof == 77:
                    print("     profile 77 = Main（有 B 帧）")
                elif prof in (100, 110):
                    print("     profile 100/110 = High（有 CABAC + 8x8 变换，软解最吃力）")
            else:
                print("  ⚠ SPS 解析失败（数据可能被截断）")
        else:
            print("  ⚠ 没找到 SPS")
    else:
        print(f"\n【判定】无法识别，前 16 字节: {blob[:16].hex(' ')}")

    # ── 帧大小分布 ──
    if frames:
        sizes = [f["size"] for f in frames]
        print(f"\n【帧大小】最小 {min(sizes)}  最大 {max(sizes)}  平均 {sum(sizes)//len(sizes)}")
        big = [f for f in frames if f["size"] > 200000]
        if big:
            print(f"  ⚠ {len(big)} 帧超过 200KB —— 车机端缓冲需加大")
        print("  前 15 帧大小:", ", ".join(str(s) for s in sizes[:15]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
