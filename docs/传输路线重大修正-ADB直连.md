# 传输路线重大修正：车机自带 ADB 驱动

**结论先说**：之前判断"在 WinCE 用户态自己实现 ADB 主机大概不可行、需要内核驱动，
所以只能走 USB 网卡（RNDIS）" —— **这个判断是错的**。

车机系统里**已经装了 ADB over USB 的驱动**，应用层可以直接打开它。
我们**不需要** USB 网卡，走 **ADB 端口转发**即可，而这恰好就是 CarLife 有线模式的
标准架构。

---

## 证据从哪来

用户手上这台车机装了 **EasyConnected**（亿连），而它是**能通过 USB 连上手机**的
（用户实测：能连旧版亿连手机应用）。

于是去分析它的可执行文件（`easyconnected.exe` v4.4.32，UPX 解包后 2049536 字节）。
完整字符串证据见 `docs/evidence/easyconnected-strings.txt`。

### 一、它在打开一个 ADB 设备

```
<%d><%s>,CreateFile ADB1: OK
<%d><%s>,CreateFile ADB1: fail
ADB1:
```

打开设备用的是 `ADB1:` 这个**流接口设备名** —— 这是 WinCE 的流驱动接口，
`CreateFile` 就够了，**不需要导入任何 USB 相关的 DLL**。
这解释了为什么它的导入表里只有 `COREDLL / WS2 / iphlpapi` 而没有任何 USB 接口。

### 二、车机系统里真的装了 ADB 驱动

```
Drivers\USB\ClientDrivers\ADB_Driver
Drivers\USB\ClientDrivers\ADB_Class
Drivers\USB\ClientDrivers\Adb
Drivers\USB\LoadClients\Default\Default\255_66_1\ADB_Driver
Drivers\USB\LoadClients\1256\0\Default\ADB_Driver
Drivers\USB\LoadClients\4817\0\Default\ADB_Driver
Drivers\USB\LoadClients\6610\0\Default\ADB_Driver
```

⭐ 最关键的一行是 **`Default\Default\255_66_1`**：

| 项 | 值 |
|---|---|
| bInterfaceClass | 255 (0xFF) |
| bInterfaceSubClass | 66 (0x42) |
| bInterfaceProtocol | 1 |

这正是 **Android ADB 接口的标准类代码**。也就是说这个驱动按**接口类**匹配，
**匹配任何 Android 手机，包括 vivo** —— 不依赖厂商 ID。

（厂商 ID 那几条是补充匹配：`1256`=0x04E8 三星、`4817`=0x12D1 华为、`6610`=0x19D2 中兴。）

### 三、它自己实现了 ADB 协议和端口转发

```
AdbForwardServer::StartAdbForward
AdbForwardServer::CreateListenSocket
ForwardEntiity::StartForward
ForwardEntiity::CreateListenSocket
ForwardEntiity::AcceptorThread
ForwardEntiity::Local2RemoteLoop
ForwardEntiity::Remote2LocalLoop
ForwardEntiity::ProxyThread

<%d><%s>,Adb Forward: %d->%d started.
<%d><%s>,Begin of Adb Proxy.
<%d><%s>,forward remote port is same ,local port is diffrent,reforward
<%d><%s>,the forward remotePort:%d,localPort:%d,is forwarded
```

`Local2RemoteLoop` / `Remote2LocalLoop` 是典型的**双向转发泵**：
把本地 TCP 连接的数据搬到 ADB 通道，再搬回来 —— 这就是 `adb forward` 的实现。

还有 `AUTH` / `OKAY` / `CLSE` 这些 ADB 协议关键字，说明它连 ADB 的
**认证流程**都自己做了（它静态链接了 OpenSSL，字符串里有 OpenSSL 痕迹）。

### 四、它的画面传输用的是 JPEG

```
JpegRender::StartAdbForward
```

这说明**在这条传输通道上传输 JPEG 画面是有先例的**，不是理论推测。

---

## 这对我们的方案意味着什么

| | 修正前 | 修正后 |
|---|---|---|
| 主路线 | USB 网卡（RNDIS）拿 IP → TCP 连手机 | **ADB 直连 + 端口转发 → 连 127.0.0.1** |
| 前提 | 车机必须有 RNDIS 驱动 ❓ 未知 | 车机有 ADB 驱动 ✅ **已由 EasyConnected 实测佐证** |
| 与 CarLife 有线模式的关系 | 不一致（那是无线模式的连法） | ✅ **完全一致** —— 有线 CarLife 本来就是这个架构 |
| 需要写内核驱动吗 | 原以为要，判断为不可行 | **不需要**，车机已提供 |

而且这**回到了我最初的架构分析**：CarLife 有线模式 = 车机做 ADB 主机 + `adb forward`。
当时唯一的疑虑是"WinCE 上能不能做 ADB 主机"，现在这个疑虑被证据消除了。

---

## 新的工作量

要新增的部分：

1. **打开 ADB 设备**：`CreateFile(L"ADB1:", ...)` + 读写
2. **ADB 协议**：`CNXN` → `AUTH`（可能需要）→ `OPEN` / `OKAY` / `WRTE` / `CLSE`
3. **ADB 认证的签名**：手机要求 RSA 公钥认证，要能对 token 做 RSA-2048 签名
   → 需要引入密码学实现（如 mbedTLS，Apache-2.0）或自己写最小的 RSA 签名
4. **端口转发**：把本地连接的数据在 ADB 通道和 CarLife 协议之间搬
5. **私钥持久化**：存到文件，手机首次会弹"允许 USB 调试"提示，用户点一次即可

已有部分**全部保留可用**：
- 协议层（握手 / 视频初始化 / 收帧 / 触摸）—— 不关心底下是 socket 还是 ADB 通道 ✅
- JPEG 解码器 ✅
- 显示与触摸层 ✅

**也就是说：协议层以上完全不用改。** 只需要把最底下的传输换掉。

---

## 仍然需要实测确认的

- `ADB1:` 这个设备名在**我们这台车机**上是否就是这个名字（EasyConnected 用的就是它，
  但不同 ROM 可能不同）—— 探测工具已列出可枚举的设备。
- 手机首次连接会弹授权提示，用户需要点"允许"。
- 转发到 7240 后，手机上的 CarLife 服务是否真的在监听（有线模式应当如此）。
