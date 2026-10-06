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
srv_state = {}         # 端口 → "ok" / 异常文本；用来定位"连不上"到底是哪一侧的问题
thread_err = []


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
    print(f"    [假手机] 收到包 msgID=0x{mid:08x} 长度={len(body)}")
    return mid, body


def expect(tag, got, want):
    with lock:
        if got == want:
            print(f"  ✅ {tag}")
        else:
            print(f"  ❌ {tag}\n       实际 {got if isinstance(got, str) else got.hex(' ')}"
                  f"\n       期望 {want if isinstance(want, str) else want.hex(' ')}")
            fails.append(tag)


def with_report(port, fn, ready):
    """把服务线程包起来：要么标记 ok，要么把异常原样记下来。
    之前这里异常会被静默吞掉，导致"连不上"看不出是谁的问题。"""
    try:
        fn(ready)
    except Exception as e:                      # noqa: BLE001
        with lock:
            srv_state[port] = f"{type(e).__name__}: {e}"
            thread_err.append(f"{port}: {type(e).__name__}: {e}")
        ready.set()


def cmd_server(ready):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT_CMD))
    srv.listen(1)
    with lock:
        srv_state[PORT_CMD] = "ok"
    print("    [假手机] 7240 已在监听")
    conn_t = None      # 已监听 ≠ 函数返回。accept() 会一直阻塞，
    ready.set()                       # 等 fn 返回才标记的话状态表永远是空的。
    conn, addr = srv.accept()
    print(f"    [假手机] 有人连上了: {addr}")
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


def video_server(ready):  # noqa: D401
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT_VIDEO))
    srv.listen(1)
    with lock:
        srv_state[PORT_VIDEO] = "ok"
    print("    [假手机] 8240 已在监听")      # 已监听 ≠ 函数返回。accept() 会一直阻塞，
    ready.set()                       # 等 fn 返回才标记的话状态表永远是空的。
    conn, addr = srv.accept()
    print(f"    [假手机] 有人连上了: {addr}")
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
    with lock:
        srv_state[PORT_TOUCH] = "ok"
    print("    [假手机] 9340 已在监听")      # 已监听 ≠ 函数返回。accept() 会一直阻塞，
    ready.set()                       # 等 fn 返回才标记的话状态表永远是空的。
    conn, addr = srv.accept()
    print(f"    [假手机] 有人连上了: {addr}")
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

    # 先跑逐步诊断：把 cl_connect 的每一步摊开，失败时能直接看出卡在哪
    r = subprocess.run(
        ["gcc", "-O1", "-Wall", "-DCL_HOST_TEST", "-Isrc", "-Ihost",
         "-o", "/tmp/cl_diag", "host/diag_connect.c"],
        capture_output=True, text=True)
    if r.returncode != 0:
        print("  ⚠ 诊断程序编译失败：", r.stderr[:400])
    else:
        # 诊断要连一个真实在听的端口，所以先临时起一个
        # ⚠ 诊断必须用【独立端口】。之前让它借用 7240，会在正式测试之前
        #   在那个端口上留下一条被 RST 的连接，污染真正的测试 —— 这个坑已经踩过。
        DIAG_PORT = 7999
        _probe = socket.socket()
        _probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            _probe.bind((HOST, DIAG_PORT))
            _probe.listen(1)
            d = subprocess.run(["/tmp/cl_diag", str(DIAG_PORT)],
                               capture_output=True, text=True, timeout=30)
            print(d.stdout, end="")
            if d.stderr.strip():
                print(d.stderr, end="")
        except OSError as e:
            print(f"  ⚠ 诊断用临时端口起不来: {e}")
        finally:
            _probe.close()

    print("\n══ 启动假手机并跑真实协议代码 ══")
    ready = [threading.Event() for _ in range(3)]
    ts = [threading.Thread(target=with_report, args=(port, fn, e), daemon=True)
          for port, fn, e in zip((PORT_CMD, PORT_VIDEO, PORT_TOUCH),
                                 (cmd_server, video_server, touch_server), ready)]
    for t in ts:
        t.start()
    for e in ready:
        e.wait(5)

    # 只报告监听状态，【不自己连一次】—— 自己连会消耗掉一次 accept，
    # 之后真正的被测程序就会读到那条错误的连接。
    print("  假手机监听状态：")
    pre_ok = True
    for port in (PORT_CMD, PORT_VIDEO, PORT_TOUCH):
        st = srv_state.get(port, "（线程未就绪）")
        if st == "ok":
            print(f"    ✅ {port} 已监听")
        else:
            print(f"    ❌ {port} 没起来: {st}")
            pre_ok = False
    if not pre_ok:
        print("\n⚠ 假手机自身有问题，下面的结果不可信。线程异常：")
        for e in thread_err:
            print("   -", e)
        return 1

    proc = subprocess.run(["/tmp/cl_host_test"], capture_output=True, text=True, timeout=40)
    print(proc.stdout, end="")
    if proc.stderr.strip():
        print(proc.stderr, end="")
    print(f"  [诊断] 被测程序退出码 = {proc.returncode}"
          + ("（负值 = 崩溃/收到信号）" if proc.returncode < 0 else ""))
    if proc.returncode != 0:
        fails.append("被测程序自身返回非零")

    for t in ts:
        t.join(timeout=6)

    if thread_err:
        print("\n⚠ 假手机线程异常（这些是测试环境自己的问题，不是被测代码的）：")
        for e in thread_err:
            print("   -", e)

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
