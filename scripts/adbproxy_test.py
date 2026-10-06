#!/usr/bin/env python3
"""用真实转发器代码 + 假手机，端到端验证端口转发。

验证的是完整链路：本地 socket → 转发器 → ADB 通道 → 手机端口 → 再原路回来。
这条链路就是 CarLife 有线模式的实际数据通路（触摸回传走的就是它）。

特别验证「手机端没启动」这条分支：假装第一次连 7240 被拒，看转发器会不会
自己用 shell 列出包名、找到 CarLife、把它启动起来，再重试成功。

假手机独占 7997 端口，不与别的测试共用（踩过端口冲突的坑）。
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
PORT = 7997                      # 与 adb_test.py 的 7998 分开，避免互相抢端口

PKG_LIST = (b"package:com.android.settings\n"
            b"package:com.example.facility\n"
            b"package:com.vivo.joviincar\n"
            b"package:com.baidu.carlife\n")

fails = []


def check(name, ok, extra=""):
    print(f"  {'✅' if ok else '❌'} {name}" + (f"  {extra}" if extra else ""))
    if not ok:
        fails.append(name)


def load_key():
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
    srv.settimeout(45)
    ready.set()
    try:
        conn, _ = srv.accept()
    except socket.timeout:
        state["err"] = "转发器没来连假手机"
        return
    conn.settimeout(30)

    try:
        # ── CNXN ──
        m = read_msg(conn)
        state["cnxn"] = m is not None and m[0] == CNXN
        send_msg(conn, CNXN, 0x01000000, 256 * 1024, b"device::ro.product.name=fake;")

        # ── AUTH：先发 20 字节挑战，再验签 ──
        # ⚠ 顺序不能反：被测代码是等手机先发 AUTH(TOKEN) 才会签名的，
        #   假手机要是直接等签名，两边会互相死等。
        token = os.urandom(20)
        send_msg(conn, AUTH, 1, 0, token)              # ADB_AUTH_TOKEN

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

        # ── 认证通过，发 CNXN 表示已授权 ──
        send_msg(conn, CNXN, 0x01000000, 256 * 1024,
                 b"device::ro.product.model=JoviTest;")

        # ── 服务循环 ──
        remote_id = 0x2000
        tcp_local = None
        refused = False
        state["opens"] = []
        state["shells"] = []
        state["writes"] = []

        for _ in range(80):
            m = read_msg(conn)
            if not m:
                break
            cmd, a0, a1, data = m
            state["last"] = name_of(cmd)

            if cmd == OPEN:
                svc = data.rstrip(b"\x00").decode("ascii", "replace")
                state["opens"].append(svc)

                if svc.startswith("tcp:7240") and not refused:
                    # ★ 关键分支：第一次拒绝，假装"手机端 CarLife 没在跑"
                    refused = True
                    state["refused_first"] = True
                    send_msg(conn, CLSE, 0, a0)

                elif svc.startswith("shell:"):
                    state["shells"].append(svc[6:])
                    remote_id += 1
                    send_msg(conn, OKAY, remote_id, a0)
                    out = PKG_LIST if "pm list" in svc else b""
                    send_msg(conn, WRTE, remote_id, a0, out)
                    m2 = read_msg(conn)
                    if m2 and m2[0] == OKAY:
                        state["shell_acked"] = True
                    send_msg(conn, CLSE, remote_id, a0)

                elif svc.startswith("tcp:"):
                    remote_id += 1
                    tcp_local = a0
                    send_msg(conn, OKAY, remote_id, a0)

                else:
                    send_msg(conn, CLSE, 0, a0)

            elif cmd == WRTE:
                # 车机→手机 WRTE: arg0=车机本地 id，arg1=手机 id
                # 手机→车机 OKAY: arg1 必须回【车机的本地 id】，即 arg0
                state["writes"].append(data)
                send_msg(conn, OKAY, remote_id, a0)
                if tcp_local is not None and a0 == tcp_local:
                    send_msg(conn, WRTE, remote_id, a0, data)   # 回显
                    m2 = read_msg(conn)
                    if m2 and m2[0] == OKAY:
                        state["echo_acked"] = True

            elif cmd == CLSE:
                send_msg(conn, CLSE, 0, a1)
                break
    except Exception as e:                                   # noqa: BLE001
        state["err"] = f"{type(e).__name__}: {e}"
    finally:
        try:
            conn.close()
        except Exception:                                    # noqa: BLE001
            pass
        srv.close()


def main():
    state = {}
    ready = threading.Event()
    t = threading.Thread(target=adbd, args=(ready, state), daemon=True)
    t.start()
    ready.wait(5)

    print("── 编译转发器主机测试 ──")
    exe = "/tmp/test_adbproxy"
    cmd = ["gcc", "-O1", "-g", "-Wall", "-DADBP_HOST_TEST", "-Ihost", "-Isrc",
           "-o", exe, "host/test_adbproxy.c", "src/adbproxy.c", "src/adb.c",
           "src/rsa.c", "-lpthread"]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print("❌ 编译失败")
        print(p.stdout)
        print(p.stderr)
        return 1
    print("  ✅ 编译通过")

    print("── 运行：真实转发器 + 假手机 ──")
    p = subprocess.run([exe, str(PORT)], capture_output=True, text=True,
                       timeout=90)
    vals = {}
    for line in p.stdout.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            vals[k.strip()] = v.strip()
    for line in p.stdout.splitlines():
        if line.strip():
            print("    " + line[:150])

    print("── 核对 ──")
    check("假手机完成认证（签名验签通过）", state.get("sig_ok") is True,
          f"签名 {state.get('sig_len')} 字节")
    check("转发器启动成功", vals.get("ADBP_START") == "0",
          vals.get("REASON", ""))
    check("三个服务都分到了本地端口",
          all(int(vals.get(k, "0") or 0) > 0
              for k in ("LPORT_CMD", "LPORT_VID", "LPORT_TOUCH")),
          f"CMD={vals.get('LPORT_CMD')} VID={vals.get('LPORT_VID')} "
          f"TOUCH={vals.get('LPORT_TOUCH')}")
    check("本地客户端能连上转发端口",
          vals.get("LOCAL_CONNECT") == "成功", str(vals.get("LOCAL_CONNECT")))

    opens = state.get("opens", [])
    check("★ 第一次 tcp:7240 被拒（模拟手机端没跑）",
          state.get("refused_first") is True, f"共 {len(opens)} 次 OPEN")
    check("★ 被拒后自动去列手机包名",
          any("pm list packages" in s for s in state.get("shells", [])),
          str(state.get("shells", [])[:3]))
    check("★ 从包名清单里找到 CarLife 并启动它",
          any("monkey -p com.baidu.carlife" in s
              for s in state.get("shells", [])),
          str([s for s in state.get("shells", []) if "monkey" in s][:1]))
    check("★ 启动后重试 tcp:7240 成功",
          opens.count("tcp:7240") >= 2 and vals.get("LOCAL_CONNECT") == "成功",
          f"tcp:7240 出现 {opens.count('tcp:7240')} 次")

    check("★ 本地发出的数据送到了手机",
          any(w == b"TOUCH-HELLO-FROM-HU" for w in state.get("writes", [])),
          str(state.get("writes", [])[:2]))
    check("★ 手机的回显送回了本地（触摸回传的完整链路）",
          vals.get("LOCAL_RECV") == "TOUCH-HELLO-FROM-HU",
          str(vals.get("LOCAL_RECV")))
    check("双向字节都不丢",
          vals.get("LOCAL_RECV_LEN") == "19" and vals.get("STATS_TX") == "19",
          f"收到 {vals.get('LOCAL_RECV_LEN')} 字节 / 发出 {vals.get('STATS_TX')} 字节")
    check("统计字节数与实际一致",
          vals.get("STATS_RX") == "19", str(vals.get("STATS_RX")))
    check("程序正常走完", vals.get("DONE") is not None or "DONE" in p.stdout)
    check("假手机侧无异常", "err" not in state, state.get("err", ""))

    print()
    if fails:
        print(f"❌ 转发器端到端验证失败：{len(fails)} 项")
        for f in fails:
            print(f"     · {f}")
        return 1
    print("✅ 转发器端到端验证通过（含"手机端没跑时自动拉起并重试"）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
