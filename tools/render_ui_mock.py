#!/usr/bin/env python3
"""把 ui.c 的界面布局在电脑上渲染成 PNG —— 为了「先看过再改」。

背景：车机上我看不到效果，而这个界面是要给人在车里看的，纯靠想象改必然走样。
所以：C 代码和这个脚本【读同一份 src/ui_layout.h】，
几何与配色只有一处定义，两边不会各说各话。

注意：这是【布局稿】，不是逐像素仿真 —— 字体是电脑上的 Noto CJK，
真机走 WinCE 的系统字面，字形会有差异；但版面、留白、层级、配色是一致的。
"""
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LAYOUT = os.path.join(ROOT, "src", "ui_layout.h")

FONT_CANDIDATES = [
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",     # apt fonts-noto-cjk
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",              # apt fonts-wqy-zenhei
    "/usr/share/fonts/truetype/arphic/uming.ttc",                # apt fonts-arphic-uming
    "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
]


def parse_layout():
    src = open(LAYOUT, encoding="utf-8").read()
    col = {}
    for n, r, g, b in re.findall(
        r"#define\s+(UI_[A-Z_]+)\s+RGB\(\s*0x([0-9A-Fa-f]{2})\s*,\s*0x([0-9A-Fa-f]{2})\s*,\s*0x([0-9A-Fa-f]{2})\s*\)",
        src,
    ):
        col[n] = (int(r, 16), int(g, 16), int(b, 16))
    num = {}
    # ⚠ 行尾可能跟着 /* 中文注释 */ —— 不加这段可选注释，UI_PAD 之类的
    #   常量会整片解析不出来（踩过一次：KeyError: 'UI_PAD'）
    for n, v in re.findall(
        r"#define\s+(UI_[A-Z_0-9]+)\s+(\d+)\s*(?:/\*[^*]*\*/)?\s*$", src, re.M
    ):
        if n not in col:
            num[n] = int(v)
    return col, num


def content_cols(G, sw):
    """内容列：屏幕比 UI_CONTENT_W 宽就居中收窄。
    和 ui.c 里的 content_left() 是同一套算法 —— 两边必须一致，
    否则设计稿和实机又是两回事。"""
    cw = G["UI_CONTENT_W"] * sw // G["UI_BASE_W"]
    if cw > sw:
        cw = sw
    l = (sw - cw) // 2
    return l, l + cw


def pick_font(size, bold=False):
    for p in FONT_CANDIDATES:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size, index=0)
            except Exception:
                continue
    return ImageFont.load_default()


def render(state, path, C, G, sw=800, sh=480):
    def px(v):
        return v * sw // G["UI_BASE_W"]

    img = Image.new("RGB", (sw, sh), C["UI_BG"])
    d = ImageDraw.Draw(img)

    f_head = pick_font(px(G["UI_F_HEAD"]), True)
    f_sub = pick_font(px(G["UI_F_SUB"]))
    f_step = pick_font(px(G["UI_F_STEP"]))
    f_det = pick_font(px(G["UI_F_DETAIL"]))
    f_bar = pick_font(px(G["UI_F_BAR"]))

    pad = px(G["UI_PAD"])
    barh = px(G["UI_BAR_H"])     # 底栏
    toph = px(G["UI_TOP_H"])     # 顶栏（两行）

    def text_at(x, y, s, font, color, anchor="la"):
        d.text((x, y), s, font=font, fill=color, anchor=anchor)

    def vcenter(y, h):
        return y + h // 2

    # ── 顶栏（两行：品牌 + 一句说明）──
    # ⚠ 必须和 ui.c 里的 ui_paint_connect 顶栏那段一一对应，
    #   否则「稿子好看、实机走样」——这脚本存在的意义就是防止这件事。
    tcl, tcr = content_cols(G, sw)
    f_brand = pick_font(px(G["UI_F_BRAND"]), True)
    f_tag = pick_font(px(G["UI_F_TAG"]))
    yb = px(4)
    text_at(tcl, yb, "XianyCar+互联", f_brand, C["UI_TEXT"], "la")
    text_at(tcl, yb + px(G["UI_F_BRAND"] + 3) + px(1),
            "支持WinCE车机的智驾车载互联工具", f_tag, C["UI_MUTED"], "la")
    text_at(tcr, yb + px(G["UI_F_BRAND"] + 3) // 2,
            "运行日志：carlife-log.txt", f_bar, C["UI_HINT"], "rm")
    d.rectangle([tcl, toph, tcr - 1, toph], fill=C["UI_RULE"])

    # ── 内容块垂直居中 ──
    tracks = G["UI_STEPS"] * px(G["UI_STEP_H"])
    block = (px(G["UI_HEAD_H"]) + px(G["UI_SUB_GAP"]) + px(G["UI_SUB_H"])
             + px(G["UI_TRACK_GAP"]) + tracks
             + px(G["UI_DETAIL_GAP"]) + px(G["UI_DETAIL_H"]))
    top = toph + (sh - toph - barh - block) // 2
    if top < toph + px(8):
        top = toph + px(8)

    clx, crx = content_cols(G, sw)
    y = top
    if state["head1"]:
        text_at(clx, vcenter(y, px(G["UI_HEAD_H"])), state["head1"], f_head,
                C["UI_TEXT"], "lm")
    y += px(G["UI_HEAD_H"]) + px(G["UI_SUB_GAP"])
    if state["head2"]:
        text_at(clx, vcenter(y, px(G["UI_SUB_H"])), state["head2"], f_sub,
                C["UI_MUTED"], "lm")

    # ── 阶段轨道 ──
    y = top + px(G["UI_HEAD_H"]) + px(G["UI_SUB_GAP"]) + px(G["UI_SUB_H"]) + px(G["UI_TRACK_GAP"])
    dotr = px(G["UI_DOT"]) // 2
    clx, crx = content_cols(G, sw)
    for i, name in enumerate(state["names"]):
        st = state["st"][i]
        cy = vcenter(y, px(G["UI_STEP_H"]))
        cx = clx + px(G["UI_DOT"]) // 2 + 1

        # 进行中那一行：极淡琥珀底（状态高亮，不是装饰）
        if st == 1:
            d.rectangle([clx - px(10), y + 1, crx + px(10), y + px(G["UI_STEP_H"]) - 1],
                        fill=C["UI_WASH"])

        fill = {0: C["UI_DOT_OFF"], 1: C["UI_AMBER"], 2: C["UI_GREEN"], 3: C["UI_RED"]}[st]
        if st == 0:
            d.ellipse([cx - dotr, cy - dotr, cx + dotr, cy + dotr],
                      outline=fill, width=max(1, px(2)))
        else:
            d.ellipse([cx - dotr, cy - dotr, cx + dotr, cy + dotr], fill=fill)

        tc = C["UI_MUTED"]
        if st in (1, 2):
            tc = C["UI_TEXT"]
        elif st == 3:
            tc = C["UI_RED"]
        text_at(cx + dotr + px(14), cy, name, f_step, tc, "lm")

        det = state["detail"][i]
        if det:
            dc = (C["UI_RED"] if st == 3 else
                  C["UI_AMBER"] if st == 1 else C["UI_MUTED"])
            text_at(crx, cy, det, f_det, dc, "rm")
        y += px(G["UI_STEP_H"])

    # ── 细节行（失败原因 / 提示）──
    if state["foot"]:
        fy = y + px(G["UI_DETAIL_GAP"])
        maxw = crx - clx
        words, line, lines = state["foot"].split(" "), "", []
        for w in words:
            t = (line + " " + w).strip()
            if d.textlength(t, font=f_det) > maxw and line:
                lines.append(line)
                line = w
            else:
                line = t
        if line:
            lines.append(line)
        for k, ln in enumerate(lines[:3]):
            text_at(clx, fy + k * (px(G["UI_F_DETAIL"]) + px(5)), ln, f_det,
                    C["UI_MUTED"], "la")

    # ── 底栏 ──
    d.rectangle([tcl, sh - barh, tcr - 1, sh - barh], fill=C["UI_RULE"])
    x = tcr
    for lbl in reversed(state["buttons"]):
        w = int(d.textlength(lbl, font=f_bar)) + 2 * px(G["UI_BTN_PAD"])
        w = max(w, px(46))
        bh = px(G["UI_BTN_H"])
        bt = sh - barh + (barh - bh) // 2
        d.rectangle([x - w, bt, x - 1, bt + bh], outline=C["UI_RULE"], width=1)
        text_at((x - w + x) // 2, bt + bh // 2, lbl, f_bar, C["UI_TEXT"], "mm")
        x -= w + px(G["UI_BTN_GAP"])

    img.save(path)
    return path


def render_mirror(path, C, G, sw=800, sh=480):
    """已连上：整屏画面 + 一条细状态条"""
    def px(v):
        return v * sw // G["UI_BASE_W"]

    img = Image.new("RGB", (sw, sh), (0, 0, 0))
    d = ImageDraw.Draw(img)
    # 用一块渐变当"手机画面"的占位，好看出状态条压在内容上是什么感觉
    for y in range(sh - px(G["UI_BAR_H"])):
        d.line([(0, y), (sw, y)], fill=(18 + y % 40, 26 + y % 30, 44 + y % 60))
    d.rectangle([0, 60, sw, 420], outline=(40, 60, 90))
    d.text((40, 200), "（手机画面铺满这一块）", font=pick_font(px(20)),
           fill=(120, 140, 170))

    barh = px(G["UI_BAR_H"])
    mcl, mcr = content_cols(G, sw)
    d.rectangle([0, sh - barh, sw, sh], fill=C["UI_PANEL"])
    d.rectangle([mcl, sh - barh, mcr - 1, sh - barh], fill=C["UI_RULE"])
    f_bar = pick_font(px(G["UI_F_BAR"]))
    d.text((mcl, sh - barh // 2), "480x272  15 帧/秒  已收 1234 帧",
           font=f_bar, fill=C["UI_MUTED"], anchor="lm")
    x = mcr
    for lbl in ["退出", "480x272", "触摸 A"]:
        w = int(d.textlength(lbl, font=f_bar)) + 2 * px(G["UI_BTN_PAD"])
        w = max(w, px(46))
        bh = px(G["UI_BTN_H"])
        bt = sh - barh + (barh - bh) // 2
        d.rectangle([x - w, bt, x - 1, bt + bh], outline=C["UI_RULE"], width=1)
        d.text(((x - w + x) // 2, bt + bh // 2), lbl, font=f_bar,
               fill=C["UI_TEXT"], anchor="mm")
        x -= w + px(G["UI_BTN_GAP"])
    img.save(path)
    return path


NAMES = ["认到手机（USB）", "启动手机车载系统", "接通控制通道", "与手机协商协议",
         "接通画面通道", "协商画面规格", "接收画面", "显示画面"]

STATES = {
    "1-等待插线": {
        "head1": "用 USB 线把手机连到车机",
        "head2": "然后在手机上打开「USB 调试」并点「允许」",
        "names": NAMES,
        "st": [1, 0, 0, 0, 0, 0, 0, 0],
        "detail": ["正在等待…", "", "", "", "", "", "", ""],
        "foot": "",
        "buttons": ["触摸 A", "480x272", "退出"],
    },
    "2-卡住了": {
        "head1": "没认到手机",
        "head2": "换个车机 USB 口，或换一根能传数据的数据线",
        "names": NAMES,
        "st": [3, 0, 0, 0, 0, 0, 0, 0],
        "detail": ["试了 6 个设备名都打不开", "", "", "", "", "", "", ""],
        "foot": "已等待 40 秒。日志里记着每个设备名各自的错误码，把日志拿回来就能定位。",
        "buttons": ["触摸 A", "480x272", "退出"],
    },
    "3-等手机端起来": {
        "head1": "手机已连上，正在启动车载系统",
        "head2": "如果手机弹出权限框，请点「允许」",
        "names": NAMES,
        "st": [2, 1, 0, 0, 0, 0, 0, 0],
        "detail": ["ADB1: 已打开", "com.baidu.carlife", "", "", "", "", "", ""],
        "foot": "手机里的智能车载正在启动，起来之后画面会自动接上。",
        "buttons": ["触摸 A", "480x272", "退出"],
    },
    "4-全通": {
        "head1": "已连上",
        "head2": "",
        "names": NAMES,
        "st": [2, 2, 2, 2, 2, 2, 2, 2],
        "detail": ["ADB1:", "com.baidu.carlife", "7240", "版本 1",
                   "8240", "480x272@15", "1234 帧", "H.264"],
        "foot": "",
        "buttons": ["触摸 A", "480x272", "退出"],
    },
}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "ui-mock"
    os.makedirs(out, exist_ok=True)
    C, G = parse_layout()
    print("解析 ui_layout.h: %d 个颜色, %d 个几何常量" % (len(C), len(G)))
    for name, st in STATES.items():
        p = render(st, os.path.join(out, "ui-%s.png" % name), C, G)
        print("  生成", p)
    p = render_mirror(os.path.join(out, "ui-5-已投屏.png"), C, G)
    print("  生成", p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
