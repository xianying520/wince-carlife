#!/usr/bin/env python3
"""用一个"假手机"逐字节验证我们的协议实现。

为什么值得做：让【真实的 carlife.c】用主机编译器编出来，连上这里的假手机，
把它发出的每个包都拆开核对。字节序、包头、字段号、时序这类错误，
光读代码看不出来，跑一遍就现形 —— 而这些错去车上才发现，代价是一趟车程。

关键设计：**期望的字节全部硬编码在下面**，不是用代码算出来的。
用代码算就是循环论证（我的理解错两次会互相抵消），硬编码才是独立核对。
"""
import os
import socket
import struct
import subprocess
import sys
import threading
import time

HOST = "127.0.0.1"
PORT_CMD, PORT_VIDEO, PORT_TOUCH = 7240, 8240, 9340

# 由 scripts/make_test_jpeg.py 生成的 141 字节灰度 JPEG（解码后恒为 128）
TEST_JPEG_PATH = "scripts/test_gray.jpg"

HDR_CMD, HDR_MEDIA = 8, 12

received = []          # (端口名, msg_id, body)
fails = []
lock = threading.Lock()


def recvn(conn, n):
    buf = b""
    while len(buf) < n:
        c = conn.recv(n - len(buf))
        if not c:
            return None
        buf += c
    return buf


def read_pkt(conn, hdr):
    """按指定包头长度读一个包，返回 (msg_id, body)"""
    h = recvn(conn, hdr)
    if h is None:
        return None
    if hdr == 8:
        size, _res, mid = struct.unpack(">HHI", h)
    else:
        size, _ts, mid = struct.unpack(">III", h)
    body = recvn(conn, size) if size else b""
    if body is None:
        return None
    return mid, body


def expect(tag, got, want):
    with lock:
        if got == want:
            print(f"  ✅ {tag}")
        else:
            print(f"  ❌ {tag}\n       实际 {got if isinstance(got, str) else got.hex(' ')}"
                  f"\n       期望 {want if isinstance(want, str) else want.hex(' ')}")
            fails.append(tag)


def cmd_server(ready):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT_CMD))
    srv.listen(1)
    ready.set()
    conn, _ = srv.accept()
    conn.settimeout(8)

    # ① 握手：HU_PROTOCOL_VERSION + CarlifeProtocolVersion{1,0}
    #    proto: majorVersion=1, minorVersion=2 → tag 0x08 / 0x10
    p = read_pkt(conn, HDR_CMD)
    if p:
        received.append(("cmd",) + p)
        expect("握手消息 ID = HU_PROTOCOL_VERSION(0x00018001)", p[0], 0x00018001)
        expect("握手包体 = 08 01 10 00", p[1], bytes([0x08, 0x01, 0x10, 0x00]))
        # 回 PROTOCOL_VERSION_MATCH + matchStatus=1
        body = bytes([0x08, 0x01])
        conn.sendall(struct.pack(">HHI", len(body), 0, 0x00010002) + body)

    # ② 重发版本
    p = read_pkt(conn, HDR_CMD)
    if p:
        received.append(("cmd",) + p)
        expect("重发版本消息 ID 正确", p[0], 0x00018001)
        expect("重发包体 = 08 01 10 00", p[1], bytes([0x08, 0x01, 0x10, 0x00]))

    # ③ VIDEO_ENCODER_INIT(480,272,15)
    #    CarlifeVideoEncoderInfo: width=1,height=2,frameRate=3
    #    480→E0 03 (varint), 272→90 02, 15→0F
    p = read_pkt(conn, HDR_CMD)
    if p:
        received.append(("cmd",) + p)
        expect("VIDEO_ENCODER_INIT 消息 ID = 0x00018007", p[0], 0x00018007)
        expect("INIT 包体 = 08 E0 03 10 90 02 18 0F  (480x272@15)",
               p[1], bytes([0x08, 0xE0, 0x03, 0x10, 0x90, 0x02, 0x18, 0x0F]))

    # ④ VIDEO_ENCODER_START（空包体）
    p = read_pkt(conn, HDR_CMD)
    if p:
        received.append(("cmd",) + p)
        expect("VIDEO_ENCODER_START 消息 ID = 0x00018009", p[0], 0x00018009)
        expect("START 包体为空", p[1], b"")

    # ⑤ VIDEO_ENCODER_JPEG（空包体）
    p = read_pkt(conn, HDR_CMD)
    if p:
        received.append(("cmd",) + p)
        expect("VIDEO_ENCODER_JPEG 消息 ID = 0x00018056", p[0], 0x00018056)
        expect("JPEG 包体为空", p[1], b"")

    time.sleep(2.0)
    conn.close()
    srv.close()


def video_server(ready):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT_VIDEO))
    srv.listen(1)
    ready.set()
    conn, _ = srv.accept()
    jpg = open(TEST_JPEG_PATH, "rb").read()
    # 视频通道用 12 字节包头：size(BE32) timestamp(BE32) msgID(BE32)
    conn.sendall(struct.pack(">III", len(jpg), 12345, 0x00010000) + jpg)
    time.sleep(1.5)
    conn.close()
    srv.close()


def touch_server(ready):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT_TOUCH))
    srv.listen(1)
    ready.set()
    conn, _ = srv.accept()
    conn.settimeout(8)

    # 写法 A：专用消息 + CarlifeTouchSinglePoint{x=1,y=2}
    for tag, mid, body in (
        ("写法A 按下 ID=TOUCH_ACTION_DOWN(0x00068002)", 0x00068002,
         bytes([0x08, 120, 0x10, 80])),
        ("写法A 移动 ID=TOUCH_ACTION_MOVE(0x00068004)", 0x00068004,
         bytes([0x08, 0xC8, 0x01, 0x10, 0x96, 0x01])),
        ("写法A 抬起 ID=TOUCH_ACTION_UP(0x00068003)", 0x00068003,
         bytes([0x08, 0xC8, 0x01, 0x10, 0x96, 0x01])),
    ):
        p = read_pkt(conn, HDR_CMD)
        if p:
            received.append(("touch",) + p)
            expect(tag, p[0], mid)
            expect(tag + " 包体", p[1], body)

    # 写法 B：通用消息 + CarlifeTouchAction{action=1,x=2,y=3}
    p = read_pkt(conn, HDR_CMD)
    if p:
        received.append(("touch",) + p)
        expect("写法B 消息 ID=TOUCH_ACTION(0x00068001)", p[0], 0x00068001)
        expect("写法B 包体 = 08 00 10 78 18 50",
               p[1], bytes([0x08, 0x00, 0x10, 0x78, 0x18, 0x50]))

    # 硬按键：CarlifeCarHardKeyCode{keycode=1}
    p = read_pkt(conn, HDR_CMD)
    if p:
        received.append(("touch",) + p)
        expect("硬按键 ID=CAR_HARD_KEY(0x00068008)", p[0], 0x00068008)
        expect("硬按键包体 = 08 10 (keycode=0x10)", p[1], bytes([0x08, 0x10]))

    conn.close()
    srv.close()


def main():
    if not os.path.exists(TEST_JPEG_PATH):
        subprocess.run([sys.executable, "scripts/make_test_jpeg.py"], check=True)

    print("══ 用主机编译器编出真实协议代码 ══")
    r = subprocess.run(
        ["gcc", "-O1", "-g", "-Wall", "-DCL_HOST_TEST", "-Isrc", "-Ihost",
         "-o", "/tmp/cl_host_test", "host/test_main.c", "src/carlife.c"],
        capture_output=True, text=True)
    print(r.stdout + r.stderr, end="")
    if r.returncode != 0:
        print("❌ 主机编译失败")
        return 1
    print("  ✅ 主机编译通过（说明协议代码没有平台耦合）")

    print("\n══ 启动假手机并跑真实协议代码 ══")
    ready = [threading.Event() for _ in range(3)]
    ts = [threading.Thread(target=f, args=(e,), daemon=True)
          for f, e in zip((cmd_server, video_server, touch_server), ready)]
    for t in ts:
        t.start()
    for e in ready:
        e.wait(3)

    proc = subprocess.run(["/tmp/cl_host_test"], capture_output=True, text=True, timeout=40)
    print(proc.stdout, end="")
    if proc.stderr.strip():
        print(proc.stderr, end="")
    if proc.returncode != 0:
        fails.append("被测程序自身返回非零")

    for t in ts:
        t.join(timeout=6)

    print("\n══ 假手机侧的逐字节核对结果 ══")
    print(f"  共收到 {len(received)} 个包")
    if fails:
        print(f"\n❌ 共 {len(fails)} 项不符：")
        for f in fails:
            print("   -", f)
        return 1
    print("\n✅ 协议实现的每一个字节都与期望一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
