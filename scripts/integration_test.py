#!/usr/bin/env python3
"""全链路集成测试：假手机 ← 假 adbd(tcp 转发) ← 真实转发器 ← 真实 CarLife 客户端 → 解码

    mock phone :7240/8240/9340
         ↑  假装成手机的本地回环端口
    mock adbd  :7996  ← 真的 ADB 协议，OPEN tcp:7240 会连到假手机并双向搬运
         ↑  socket 顶替 USB 设备
    真实转发器 (src/adbproxy.c)
         ↑  127.0.0.1:<系统分配的端口>
    真实 CarLife 客户端 (src/carlife.c) + 真实 nanojpeg 解码

除掉车机的 GDI 画面、真机 USB，这条链路和现场跑的是同一份代码。

端口分配（各测试互不冲突，踩过端口冲突的坑）：
    7998 adb_test / 7997 adbproxy_test / 7996 本测试的假 adbd
"""
import hashlib
import os
import re
import select
import socket
import struct
import subprocess
import sys
import threading
import time

CNXN = 0x4E584E43
AUTH = 0x48545541
OPEN = 0x4E45504F
OKAY = 0x59414B4F
CLSE = 0x45534C43
WRTE = 0x45545257

HOST = "127.0.0.1"
ADBD_PORT = 7996
PHONE_PORTS = {"tcp:7240": 7240, "tcp:8240": 8240, "tcp:9340": 9340}

fails = []


def check(name, ok, extra=""):
    print(f"  {'✅' if ok else '❌'} {name}" + (f"  {extra}" if extra else ""))
    if not ok:
        fails.append(name)


def load_key():
    src = open("src/adbkey.h", encoding="utf-8").read()
    body = src.split("ADB_KEY_N[256] = {", 1)[1].split("};", 1)[0]
    nums = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    assert len(nums) == 256
    return int.from_bytes(bytes(nums), "big")


def recvn(conn, n):
    buf = b""
    while len(buf) < n:
        try:
            c = conn.recv(n - len(buf))
        except socket.timeout:
            return None
        if not c:
            return None
        buf += c
    return buf


def read_msg(conn):
    h = recvn(conn, 24)
    if h is None:
        return None
    cmd, a0, a1, dlen, crc, magic = struct.unpack("<6I", h)
    assert magic == (cmd ^ 0xFFFFFFFF), "magic 不对"
    data = recvn(conn, dlen) if dlen else b""
    if data is None:
        return None
    return cmd, a0, a1, data


def send_msg(conn, cmd, a0, a1, data=b""):
    conn.sendall(struct.pack("<6I", cmd, a0, a1, len(data), 0,
                             cmd ^ 0xFFFFFFFF) + data)


# ══════════════════════════════════════════════════════════════
# 假手机 —— 三个 CarLife 端口
# ══════════════════════════════════════════════════════════════
phone_state = {}
phone_socks = {}
phone_ready = threading.Event()


def phone_server(port, handler):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, port))
    srv.listen(1)
    srv.settimeout(60)
    phone_socks[port] = srv
    if len(phone_socks) == 3:
        phone_ready.set()
    try:
        conn, _ = srv.accept()
    except socket.timeout:
        phone_state[f"{port}_err"] = "没人来连"
        return
    conn.settimeout(20)
    phone_state[f"{port}_connected"] = True
    try:
        handler(conn)
    except Exception as e:                                   # noqa: BLE001
        phone_state[f"{port}_err"] = f"{type(e).__name__}: {e}"
    finally:
        try:
            conn.close()
        except Exception:                                    # noqa: BLE001
            pass
        srv.close()


def rd(conn, n):
    """读满 n 字节；CarLife 是大端"""
    return recvn(conn, n)


def cmd_handler(conn):
    """控制通道：握手 → 视频初始化。

    消息 ID 与包体格式全部照抄已验证过的 protocol_test.py，
    不再自己猜 —— 之前这里把 ID 猜成 0x00018002/3/4，全错。
    """
    while True:
        h = rd(conn, 8)
        if not h:
            return
        size = struct.unpack(">H", h[0:2])[0]
        msg_id = struct.unpack(">I", h[4:8])[0]
        body = rd(conn, size) if size else b""
        phone_state.setdefault("cmd_msgs", []).append(
            (f"0x{msg_id:08X}", bytes(body)))

        if msg_id == 0x00018001:                 # HU_PROTOCOL_VERSION
            phone_state["got_version"] = body
            # 回 PROTOCOL_VERSION_MATCH + matchStatus=1
            rb = bytes([0x08, 0x01])
            conn.sendall(struct.pack(">HHI", len(rb), 0, 0x00010002) + rb)
        elif msg_id == 0x00018007:               # VIDEO_ENCODER_INIT
            phone_state["video_init"] = body
            phone_state["video_init_seen"] = True
        elif msg_id == 0x00018009:               # VIDEO_ENCODER_START
            phone_state["video_start"] = True
        elif msg_id == 0x00018056:               # VIDEO_ENCODER_JPEG
            phone_state["video_jpeg_req"] = True


JPEG = None


def video_handler(conn):
    """视频通道：等收到视频初始化后，推一帧真 JPEG"""
    deadline = time.time() + 25
    while time.time() < deadline:
        if phone_state.get("video_init_seen"):
            break
        time.sleep(0.1)

    data = open("/tmp/test_gray32x16.jpg", "rb").read()
    phone_state["video_jpeg_len"] = len(data)

    # ① 先推一帧【故意超大】的（5KB，远大于客户端的 1KB 测试缓冲）。
    #    这是为了逼出"帧比缓冲大"时那段清理逻辑 —— 它以前从没被执行过。
    #    帧头故意写成 JPEG 的 FF D8，让它看起来像真帧，后面是真垃圾数据。
    big = b"\xff\xd8" + bytes((k * 37) & 0xFF for k in range(5120))
    phone_state["big_sent"] = len(big)
    conn.sendall(struct.pack(">III", len(big), 0, 0x00000001) + big)
    time.sleep(0.5)

    # ② 【关键回归】把一帧拆开发：包头分两段、包体分三段，中间各停一下。
    #
    #    真机 USB 链路上数据本来就是分段的，这几乎是常态。而"读到一半超时就
    #    把已读字节丢掉"那个 bug 只会在这里暴露 —— 前面所有测试都是整帧一次
    #    发出去的，环回快得根本不会卡在中间，所以永远碰不到。
    #    这里让包头两段之间停 120ms（客户端轮询超时是 20ms），必然触发。
    blob = struct.pack(">III", len(data), 3, 0x00000001) + data
    conn.sendall(blob[:5])
    time.sleep(0.12)                      # ← 关键：故意超过客户端的 20ms 轮询超时
    conn.sendall(blob[5:12])
    time.sleep(0.12)
    third = (len(blob) - 12) // 3
    for k in range(3):
        a = 12 + k * third
        b = len(blob) if k == 2 else 12 + (k + 1) * third
        conn.sendall(blob[a:b])
        time.sleep(0.08)
    phone_state["slow_sent"] = True

    # ③ 再连续推真 JPEG 多帧：客户端应当每一帧都收得到。
    #    挨过超大帧之后如果流错位了，这里就只能收到很少几帧。
    phone_state["jpeg_frames"] = 0
    for _ in range(6):
        try:
            conn.sendall(struct.pack(">III", len(data), 2, 0x00000001) + data)
            phone_state["jpeg_frames"] += 1
        except Exception:                                    # noqa: BLE001
            break
        time.sleep(0.25)
    phone_state["video_sent"] = True
    time.sleep(2)


def touch_handler(conn):
    """触摸通道：按 CMD 帧格式收一条触摸消息"""
    try:
        h = rd(conn, 8)
        if h:
            size = struct.unpack(">H", h[0:2])[0]
            msg_id = struct.unpack(">I", h[4:8])[0]
            body = rd(conn, size) if size else b""
            phone_state["touch_raw"] = bytes(body)
            phone_state["touch_msg_id"] = f"0x{msg_id:08X}"
            phone_state["touch_len"] = len(body)
    except Exception:                                        # noqa: BLE001
        pass


# ══════════════════════════════════════════════════════════════
# 假 adbd —— 真的 ADB 协议 + tcp 端口转发
# ══════════════════════════════════════════════════════════════
class FakeAdbd:
    def __init__(self, conn, state):
        self.conn = conn
        self.state = state
        self.chans = {}          # their_local_id -> {"sock":..., "our_id":...}
        self.err = None
        # ⚠ 通道一开就记进 state，不要等 run() 返回再一起写。
        #   run() 要等连接断开才返回，测试却是在被测进程退出后立刻读 state——
        #   差一点点就读到空的（这个竞态真的发生过，表现成"三条通道一条都没开"）。

    def handle_open(self, a0, data):
        svc = data.rstrip(b"\x00").decode("ascii", "replace")
        self.state.setdefault("opens", []).append(svc)
        if svc in PHONE_PORTS:
            try:
                s = socket.create_connection((HOST, PHONE_PORTS[svc]), timeout=5)
            except OSError as e:
                self.err = f"连假手机 {svc} 失败: {e}"
                send_msg(self.conn, CLSE, 0, a0)
                return
            s.setblocking(False)
            our_id = 0x3000 + len(self.chans)
            self.chans[a0] = {"sock": s, "our_id": our_id}
            send_msg(self.conn, OKAY, our_id, a0)
        else:
            send_msg(self.conn, CLSE, 0, a0)

    def handle_wrte(self, a0, a1, data):
        # a0 = 对端本地 id(=这里的 their_local)，a1 = 我们给的 id
        ch = self.chans.get(a0)
        if ch:
            try:
                ch["sock"].sendall(data)
            except OSError:
                pass
        send_msg(self.conn, OKAY, a1 if ch else 0, a0)

    def read_one(self, timeout):
        self.conn.settimeout(timeout)
        m = read_msg(self.conn)
        if not m:
            return False
        cmd, a0, a1, data = m
        if cmd == OPEN:
            self.handle_open(a0, data)
        elif cmd == WRTE:
            self.handle_wrte(a0, a1, data)
        elif cmd == CLSE:
            ch = self.chans.pop(a0, None)
            if ch:
                try:
                    ch["sock"].close()
                except OSError:
                    pass
            send_msg(self.conn, CLSE, a1, a0)
        return True

    def push(self, their_local, data):
        """把手机侧的数据推给车机，并等 OKAY（等待期间继续处理别的消息）"""
        ch = self.chans[their_local]
        send_msg(self.conn, WRTE, ch["our_id"], their_local, data)
        for _ in range(60):
            if not self.read_one(1.0):
                return
            # OKAY 会被 read_one 当作"不是 OPEN/WRTE/CLSE"直接跳过；
            # 这里用一个粗略但够用的判据：只要还活着就继续等
            return

    def run(self):
        while True:
            try:
                socks = [self.conn] + [c["sock"] for c in self.chans.values()]
                r, _, _ = select.select(socks, [], [], 1.0)
            except (OSError, ValueError):
                return
            if self.conn in r:
                try:
                    if not self.read_one(0.5):
                        return
                except OSError:
                    return
            for c in list(self.chans.values()):
                if c["sock"] in r:
                    try:
                        d = c["sock"].recv(65536)
                    except (BlockingIOError, OSError):
                        continue
                    if not d:
                        continue
                    key = [k for k, v in self.chans.items() if v is c][0]
                    self.push(key, d)


def adbd_server(ready, state):
    n = load_key()
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, ADBD_PORT))
    srv.listen(1)
    srv.settimeout(60)
    ready.set()
    try:
        conn, _ = srv.accept()
    except socket.timeout:
        state["err"] = "没人来连假 adbd"
        return
    conn.settimeout(30)

    try:
        conn.settimeout(20)
        m = read_msg(conn)
        state["cnxn"] = bool(m) and m[0] == CNXN

        token = os.urandom(20)
        send_msg(conn, AUTH, 1, 0, token)
        m = read_msg(conn)
        if m and m[0] == AUTH and m[1] == 2 and len(m[3]) == 256:
            rec = pow(int.from_bytes(m[3], "big"), 65537, n).to_bytes(256, "big")
            prefix = bytes.fromhex("3021300906052b0e03021a05000414")
            want = (b"\x00\x01" + b"\xff" * (256 - 3 - 35) + b"\x00" + prefix
                    + hashlib.sha1(token).digest())
            state["sig_ok"] = (rec == want)

        send_msg(conn, CNXN, 0x01000000, 256 * 1024, b"device::model=JoviTest;")
        conn.settimeout(None)

        a = FakeAdbd(conn, state)
        a.run()
        if a.err:
            state["err"] = a.err
    except Exception as e:                                   # noqa: BLE001
        state["err"] = f"{type(e).__name__}: {e}"
    finally:
        try:
            conn.close()
        except Exception:                                    # noqa: BLE001
            pass
        srv.close()


def main():
    print("── 准备测试图（32x16，可精确断言）──")
    p = subprocess.run(["python3", "scripts/make_test_jpeg.py",
                        "/tmp/test_gray32x16.jpg", "32", "16"],
                       capture_output=True, text=True)
    if p.returncode != 0:
        print("❌ 造图失败:", p.stdout, p.stderr)
        return 1
    print("  ✅", os.path.getsize("/tmp/test_gray32x16.jpg"), "字节")

    state = {}
    ready = threading.Event()
    threading.Thread(target=adbd_server, args=(ready, state), daemon=True).start()
    ready.wait(5)

    for port, fn in ((7240, cmd_handler), (8240, video_handler),
                     (9340, touch_handler)):
        threading.Thread(target=phone_server, args=(port, fn), daemon=True).start()
    phone_ready.wait(5)
    print("  ✅ 假手机三个端口就绪，假 adbd 就绪")

    print("── 编译集成测试 ──")
    exe = "/tmp/test_integration"
    cmd = ["gcc", "-O1", "-g", "-Wall", "-DCL_HOST_TEST", "-DADBP_HOST_TEST",
           "-Ihost", "-Isrc",
           "-o", exe,
           "host/test_integration.c", "src/adbproxy.c", "src/adb.c", "src/rsa.c",
           "src/carlife.c", "src/third_party/nanojpeg.c",
           "-lpthread"]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print("❌ 编译失败")
        print(p.stdout)
        print(p.stderr)
        return 1
    print("  ✅ 编译通过")

    print("── 运行：假手机 ← 假 adbd ← 转发器 ← CarLife 客户端 ──")
    p = subprocess.run([exe, str(ADBD_PORT)], capture_output=True, text=True,
                       timeout=120)
    vals = {}
    for line in p.stdout.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            vals[k.strip()] = v.strip()
    for line in p.stdout.splitlines():
        if line.strip():
            print("    " + line[:150])
    if p.stderr.strip():
        print("    [stderr]", p.stderr.strip()[:300])

    print("── 核对 ──")
    check("假 adbd 完成 RSA 认证", state.get("sig_ok") is True)
    check("转发器启动成功", vals.get("ADBP_START") == "0", vals.get("REASON", ""))
    check("★ 三条通道全部被转发",
          sorted(a.split(":")[1] for a in state.get("opens", []) if ":" in a)
          == ["7240", "8240", "9340"],
          str(state.get("opens", [])))

    check("★ 控制通道握手成功", vals.get("HANDSHAKE") == "0",
          f"match={vals.get('HANDSHAKE_MATCH')}")
    check("★ 假手机收到了协议版本包", "got_version" in phone_state,
          str(phone_state.get("got_version", b"")[:8].hex()))
    check("★ 假手机收到了视频初始化",
          phone_state.get("video_init_seen") is True,
          str(phone_state.get("video_init", b"")[:12].hex()))
    check("★ 视频初始化 = 32x16@5（protobuf: 08 20 10 10 18 05）",
          phone_state.get("video_init", b"")
          == bytes([0x08, 0x20, 0x10, 0x10, 0x18, 0x05]),
          str(phone_state.get("video_init", b"").hex()))

    check("★ 超大帧（5KB）被收下并按缓冲截断",
          vals.get("BIGFRAME_GOT") == "1" and vals.get("BIGFRAME_TRUNCATED") == "1",
          f"len={vals.get('BIGFRAME_LEN')}")

    check("★ 客户端收到了视频帧", vals.get("FRAME_GOT") == "1")
    check("★ ★ 拆开发的那一帧仍然完整收到（读到一半超时不能丢字节）",
          vals.get("FRAME_GOT") == "1" and vals.get("NJDECODE") == "0",
          f"帧长={vals.get('FRAME_LEN')} 解码={vals.get('NJDECODE')}")

    check("★ 收到的是 JPEG（FF D8 开头）",
          vals.get("FRAME_IS_JPEG") == "是", vals.get("FRAME_HEAD", ""))
    check("★ JPEG 解码成功", vals.get("NJDECODE") == "0", vals.get("NJDECODE"))
    check("★ 解出的尺寸正是 32x16",
          vals.get("DECODED_W") == "32" and vals.get("DECODED_H") == "16",
          f"{vals.get('DECODED_W')}x{vals.get('DECODED_H')}")
    check("★ 解出的像素全对（整幅 128 灰）",
          vals.get("DECODED_ALL_128") == "1",
          f"首={vals.get('DECODED_PX0')} 末={vals.get('DECODED_PXLAST')} "
          f"大小={vals.get('DECODED_SIZE')}")

    check("★ ★ 挨过超大帧之后流仍同步（连收多帧）",
          int(vals.get("MORE_FRAMES", "0")) >= 4,
          f"又收到 {vals.get('MORE_FRAMES')} 帧 / 手机共推 "
          f"{phone_state.get('jpeg_frames')} 帧")

    check("★ 触摸回传发出去了", vals.get("TOUCH_SEND") == "0",
          vals.get("TOUCH_SEND"))
    check("★ 假手机收到了触摸包", phone_state.get("touch_len", 0) > 0,
          f"ID={phone_state.get('touch_msg_id')} "
          f"{phone_state.get('touch_len')} 字节 "
          f"{phone_state.get('touch_raw', b'')[:10].hex()}")

    check("没有卡死：程序自己走完了", "DONE" in p.stdout.splitlines()
          or vals.get("DONE") is not None or "DONE" in p.stdout)
    check("假 adbd 侧无异常", "err" not in state, state.get("err", ""))
    for k, v in phone_state.items():
        if k.endswith("_err"):
            check(f"假手机 {k} 无异常", False, str(v))

    print()
    if fails:
        print(f"❌ 全链路集成验证失败：{len(fails)} 项")
        for f in fails:
            print(f"     · {f}")
        return 1
    print("✅ 全链路集成验证通过（假手机 → 假 adbd → 转发器 → CarLife 客户端 → 解码）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
