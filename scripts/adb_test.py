#!/usr/bin/env python3
"""用 Python 写一个假 adbd，让真实的 src/adb.c 连上来做端到端验证。

最关键的检查是【用真实 RSA 验签】—— 这等于在电脑上先回答"手机到底会不会
接受我们的认证"。签名错了，真机上就会一直弹授权提示或者直接连不上。

注意：假 adbd 独占 7998 端口，绝不和别的测试共用端口
（之前踩过"诊断程序借用了被测目标的端口"的坑，查了很久）。
"""
import hashlib
import os
import re
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
PORT = 7998

fails = []
recv_log = []


def check(name, ok, extra=""):
    print(f"  {'✅' if ok else '❌'} {name}" + (f"  {extra}" if extra else ""))
    if not ok:
        fails.append(name)


def load_key():
    """从 src/adbkey.h 里读出模数 n（和被测代码用的是同一把钥匙，但这不算
    循环论证 —— 我们验的是"签名是否正确"，不是"钥匙是否正确"）。"""
    src = open("src/adbkey.h", encoding="utf-8").read()
    body = src.split("ADB_KEY_N[256] = {", 1)[1].split("};", 1)[0]
    nums = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    assert len(nums) == 256, len(nums)
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
    if magic != (cmd ^ 0xFFFFFFFF):
        raise AssertionError(f"magic 不对: cmd={cmd:08x} magic={magic:08x}")
    data = recvn(conn, dlen) if dlen else b""
    if data is None:
        raise AssertionError("载荷读不全")
    return cmd, a0, a1, data


def send_msg(conn, cmd, a0, a1, data=b""):
    conn.sendall(struct.pack("<6I", cmd, a0, a1, len(data), 0,
                             cmd ^ 0xFFFFFFFF) + data)


def name_of(cmd):
    for n, v in (("CNXN", CNXN), ("AUTH", AUTH), ("OPEN", OPEN),
                 ("OKAY", OKAY), ("CLSE", CLSE), ("WRTE", WRTE)):
        if v == cmd:
            return n
    return f"0x{cmd:08x}"


def adbd(ready, state):
    n = load_key()

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT))
    srv.listen(1)
    state["listening"] = True
    ready.set()

    conn, addr = srv.accept()
    state["peer"] = addr
    conn.settimeout(10)

    try:
        # ── ① 等 CNXN ──
        m = read_msg(conn)
        if not m:
            state["err"] = "没收到 CNXN"
            return
        cmd, a0, a1, data = m
        state["cnxn"] = (cmd, a0, a1, data)

        # ── ② 发一个 20 字节 token ──
        token = os.urandom(20)
        state["token"] = token
        send_msg(conn, AUTH, 1, 0, token)          # ADB_AUTH_TOKEN

        # ── ③ 收回应，验签 ──
        m = read_msg(conn)
        if not m:
            state["err"] = "没收到认证回应"
            return
        cmd, a0, a1, sig = m
        state["auth_cmd"] = (cmd, a0, a1)
        state["sig_len"] = len(sig)

        if cmd == AUTH and a0 == 2 and len(sig) == 256:
            recovered = pow(int.from_bytes(sig, "big"), 65537, n).to_bytes(256, "big")
            prefix = bytes.fromhex("3021300906052b0e03021a05000414")
            want = (b"\x00\x01" + b"\xff" * (256 - 3 - 35) + b"\x00" + prefix
                    + hashlib.sha1(token).digest())
            state["sig_ok"] = (recovered == want)
            if recovered != want:
                state["sig_head"] = recovered[:4].hex()
        elif cmd == AUTH and a0 == 3:
            state["sent_pubkey_first"] = True
            state["pubkey_payload"] = sig[:80]
            # 第一次就发了公钥：按协议回一个新 token，看它接下来发什么
            token2 = os.urandom(20)
            state["token"] = token2
            send_msg(conn, AUTH, 1, 0, token2)
            m = read_msg(conn)
            if m and m[0] == AUTH and m[1] == 2 and len(m[3]) == 256:
                recovered = pow(int.from_bytes(m[3], "big"), 65537, n).to_bytes(256, "big")
                prefix = bytes.fromhex("3021300906052b0e03021a05000414")
                want = (b"\x00\x01" + b"\xff" * (256 - 3 - 35) + b"\x00" + prefix
                        + hashlib.sha1(token2).digest())
                state["sig_ok"] = (recovered == want)

        # ── ④ 认证通过，发 CNXN 表示已授权 ──
        send_msg(conn, CNXN, 0x01000000, 256 * 1024,
                 b"device::ro.product.name=mock;ro.product.model=JoviTest;")

        # ── ⑤ 等 OPEN ──
        m = read_msg(conn)
        if not m:
            state["err"] = "没收到 OPEN"
            return
        cmd, local0, a1, data = m
        state["open"] = (cmd, local0, data)
        remote_id = 0x1234
        state["remote_id"] = remote_id
        send_msg(conn, OKAY, remote_id, local0)

        # ── ⑥ 收 WRTE，回 OKAY，然后原样发回 ──
        for _ in range(10):
            m = read_msg(conn)
            if not m:
                break
            cmd, a0, a1, data = m
            if cmd == WRTE:
                recv_log.append(data)
                send_msg(conn, OKAY, remote_id, local0)
                # 把收到的原样发回（arg0=对端的 id，arg1=对方的 id）
                send_msg(conn, WRTE, remote_id, local0, data)
                m2 = read_msg(conn)
                if m2 and m2[0] == OKAY:
                    state["echo_acked"] = True
            state["last"] = name_of(cmd)
    except Exception as e:
        state["err"] = f"{type(e).__name__}: {e}"
    finally:
        try:
            conn.close()
        except Exception:
            pass
        srv.close()


def main():
    ready = threading.Event()
    state = {}
    th = threading.Thread(target=adbd, args=(ready, state), daemon=True)
    th.start()

    if not ready.wait(5):
        print("❌ 假 adbd 起不来")
        return 1
    print(f"  ✅ 假 adbd 已在 {HOST}:{PORT} 监听")

    r = subprocess.run(
        ["gcc", "-O2", "-Wall", "-DCL_HOST_TEST", "-Isrc", "-Ihost",
         "-o", "/tmp/test_adb", "host/test_adb.c", "src/adb.c", "src/rsa.c"],
        capture_output=True, text=True)
    if r.returncode != 0:
        print("  ❌ 编译失败：")
        print(r.stderr[:2500])
        return 1
    print("  ✅ 编译通过")

    p = subprocess.run(["/tmp/test_adb", str(PORT)], capture_output=True,
                       text=True, timeout=60)
    print()
    print("  ── 被测程序输出 ──")
    for line in p.stdout.splitlines():
        print("   ", line)
    if p.returncode != 0 and p.stdout.strip():
        print(f"    (退出码 {p.returncode})")

    vals = {}
    for line in p.stdout.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            vals[k.strip()] = v.strip()

    th.join(timeout=3)
    print()
    print("  ── 假 adbd 侧的逐字节核对 ──")

    # ① CNXN
    cx = state.get("cnxn")
    check("收到 CNXN", bool(cx) and cx[0] == CNXN, name_of(cx[0]) if cx else "")
    if cx:
        check("CNXN 版本 = 0x01000000", cx[1] == 0x01000000, f"0x{cx[1]:08x}")
        check("CNXN banner 以 host:: 开头", cx[3].startswith(b"host::"),
              cx[3][:24].decode("ascii", "replace"))

    # ② 认证
    ac = state.get("auth_cmd")
    check("回应的是签名（AUTH arg0=2）", bool(ac) and ac[1] == 2,
          f"{name_of(ac[0])} arg0={ac[1]}" if ac else "没收到")
    check("签名长度 256 字节", state.get("sig_len") == 256, str(state.get("sig_len")))
    check("★ 签名通过 RSA 验签（手机会接受这个认证）",
          state.get("sig_ok") is True,
          "" if state.get("sig_ok") else f"还原块开头 {state.get('sig_head','?')}")

    # ③ OPEN
    op = state.get("open")
    check("收到 OPEN", bool(op) and op[0] == OPEN)
    if op:
        check("OPEN 的服务名 = tcp:7240",
              op[2].rstrip(b"\x00") == b"tcp:7240",
              repr(op[2][:24]))

    # ④ 数据
    check("收到 WRTE 且内容正确", recv_log and recv_log[0] == b"hello-from-hu",
          repr(recv_log[0][:32]) if recv_log else "没收到")
    check("对端对我们的 WRTE 回了 OKAY", state.get("echo_acked") is True)
    check("被测程序收到了回显", vals.get("ECHO") == "hello-from-hu",
          vals.get("ECHO", "无"))
    check("协商出的最大载荷合理", vals.get("MAXDATA") == "262144",
          vals.get("MAXDATA", "?"))
    check("被测量程序自身退出码为 0", p.returncode == 0, str(p.returncode))

    if state.get("err"):
        print(f"\n  ⚠ 假 adbd 侧异常：{state['err']}")

    print()
    if fails:
        print(f"❌ 共 {len(fails)} 项不符：")
        for f in fails:
            print("   -", f)
        return 1
    print("✅ ADB 协议实现逐字节验证通过（含 RSA 认证）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
