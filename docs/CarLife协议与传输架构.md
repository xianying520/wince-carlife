# CarLife 协议与传输架构（M2/M3 的实现依据）

> 依据：`Doris-duo/CarLife-MD-Lib`（Apache-2.0，源自百度官方
> `ApolloAuto/apollo-DuerOS` 的 CarLife-Vehicle-Lib，**车机端**逻辑）。
> 本文件里的事实分三类标注：**【实证】/【代码原文】/【推断】**。

## 一、传输层 —— M2 的答案（此前最大的未知）

### 【代码原文】`CConnectManager.h`

```c
#define ADB_FORWARD_CMD    "adb forward tcp:7200 tcp:7240"
#define ADB_FORWARD_VIDEO  "adb forward tcp:8200 tcp:8240"
#define ADB_FORWARD_MEDIA  "adb forward tcp:9200 tcp:9240"
#define ADB_FORWARD_TTS    "adb forward tcp:9201 tcp:9241"
#define ADB_FORWARD_VR     "adb forward tcp:9202 tcp:9242"
#define ADB_FORWARD_TOUCH  "adb forward tcp:9300 tcp:9340"
#define LOCAL_IP_ADDR "127.0.0.1"
```

### 结论：CarLife 有**两条**传输路径

| | 有线（USB） | 无线 |
|---|---|---|
| 机制 | 车机是 **ADB host**，用 `adb forward` 把 phone 端口映射到车机 127.0.0.1 | 车机直接 TCP 连手机 IP |
| 车机需要 | **ADB host 实现**（USB + ADB 协议 + RSA 认证） | **只要 Winsock** |
| 本车机可行性 | 难（要自己写 ADB host） | ✅ **WS2.dll 已实证存在** |

### 六个通道 / 端口表

| 通道 | 车机监听 | 手机端口 | 包头 | 单包上限 |
|---|---|---|---|---|
| CMD（控制/握手） | 7200 | 7240 | 8 字节 | 40 KB |
| VIDEO（视频流） | 8200 | 8240 | 12 字节 | 600 KB |
| MEDIA（音频） | 9200 | 9240 | 12 字节 | 100 KB |
| TTS | 9201 | 9241 | 12 字节 | 50 KB |
| VR（语音识别） | 9202 | 9242 | 12 字节 | 50 KB |
| TOUCH（触摸回传） | 9300 | 9340 | 12 字节 | — |

### 通道方向（车机端的角色）

【代码原文】`CConnectManager` 的接口：
- `readVideoData()` / `readMediaData()` / `readTTSData()` —— **车机读**（接收手机推来的画面和声音）
- `writeCtrlData()` —— **车机写**（把触摸/控制发给手机）

→ **车机端 = 收视频 + 发触摸**，与预期一致。

### ⭐ 由此产生的最重要战略推论【推断】

既然**无线路径只需要 Winsock**，而本车机的 `WS2.dll` 已实证存在 ——
那么只要能给车机**一条 IP 链路**，就能绕开"实现 ADB host"这块最硬的骨头。

**候选方案：手机开 USB 网络共享（RNDIS）→ 车机拿到 IP → 走无线路径连手机的 7240 等端口。**

需要先验证三件事（按此顺序）：
1. 车机支持 USB 网络共享主机驱动吗？（插上开了共享的手机，车机是否获得 IP）
2. 手机 CarLife 是否监听在所有网卡上（0.0.0.0）—— 若是，车机就能连
3. Jovi InCar 是否愿意在该 IP 链路上进入连接状态

**这是当前性价比最高的实验路径。**

## 二、消息封装【代码原文】

### 包头结构

```c
typedef struct analyzedHead {
    u32 packageDataSize;       // 数据体字节数
    u32 packageDataTimeStamp;  // 时间戳
    u32 packageHeadType;       // 消息 ID
} S_ANALYZED_HEAD;             // 共 12 字节
```

### 精确字段偏移（已从解析+打包两侧代码确认）✅

**全部为网络字节序（大端）。**

#### CMD / CTRL 通道 —— 8 字节包头

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0..1 | 2 | `packageDataSize` | **BE16** |
| 2..3 | 2 | —— | **保留**（CMD/CTRL 的 timestamp 恒为 0） |
| 4..7 | 4 | `packageHeadType` | **BE32** 消息 ID |

#### VIDEO / MEDIA / TTS / VR 通道 —— 12 字节包头

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0..3 | 4 | `packageDataSize` | **BE32** |
| 4..7 | 4 | `packageDataTimeStamp` | **BE32** |
| 8..11 | 4 | `packageHeadType` | **BE32** 消息 ID |

#### 双向印证

- 解析侧：CMD 用 `head[0]<<8|head[1]`；其余通道 `head[4..7]` 为 timestamp、`head[8..11]` 为 type
- 打包侧：`head[0]=(size>>8)&0xff; head[1]=size&0xff;`（CMD 的 BE16 写法）

→ **发送时按此表手工拼头即可，无需猜测。**

## 三、消息 ID 规律【代码原文】

ID 里**编码了方向** —— 这是实现时最省事的线索：

- `0x0001` **`8`** `xxx` → **车机发给手机**（HU → MD）
- `0x0001` **`0`** `xxx` → **手机发给车机**（MD → HU）

### 握手与关键消息

| ID | 方向 | 含义 |
|---|---|---|
| `0x00018001` | HU→MD | 协议版本 |
| `0x00010002` | MD→HU | 版本匹配结果 |
| `0x00018003` | HU→MD | 车机信息 |
| `0x00010004` | MD→HU | 手机信息 |
| `0x00018005/06` | 双向 | 蓝牙配对信息 |
| `0x00018007` | HU→MD | **视频编码器初始化**（含宽/高/帧率） |
| `0x00010008` | MD→HU | 初始化完成 |
| `0x00018009` | HU→MD | 视频编码开始 |
| `0x0001800A/0B` | HU→MD | 暂停 / 复位 |
| `0x0001800C/0D` | 双向 | 帧率变更 |
| `0x00018048` | HU→MD | **认证请求** |
| `0x00010049` | MD→HU | 认证应答 |
| `0x0001804A/4B` | 双向 | 认证结果 |
| `0x00010035/36` | MD→HU | 媒体信息 / 进度条 |
| `0x00010014~17` | MD→HU | 电话状态（来电/去电/空闲/通话中） |
| `0x0001801D~20` | HU→MD | 启动模式（普通/电话/地图/音乐） |
| `0x00010021` | MD→HU | 回桌面 |

### 触摸相关【代码原文】

`.proto` 提供完整触摸支持：`CarlifeTouchEventProto`、`CarlifeTouchSinglePointProto`、
`CarlifeTouchActionProto`、`CarlifeTouchScrollProto`、`CarlifeTouchFlingProto`、
`CarlifeTouchEventDeviceProto`、`CarlifeTouchEventAllDeviceProto`

## 四、连接建立时序【代码原文】

```c
int CConnectionSetupModule::connectionSetup() {      // 有线（USB/ADB）
    execSocketForward();        // 1. 执行 6 条 adb forward
    createCmdSocket();          // 2. 依次建立 6 条 TCP 连接（连 127.0.0.1）
    createVideoSocket();
    createMediaSocket();
    createTTSSocket();
    createVRSocket();
    createTouchSocket();
}
int CConnectionSetupModule::connectionSetup(string mdIPAddress) {  // 无线
    createCmdSocket(mdIPAddress);   // 直接连手机 IP 的 7240/8240/...
    ... 同序 6 条
}
```

**之后才进入协议握手**：版本协商 → 设备信息 → 视频编码器初始化 → 认证。

## 五、许可证 —— 可以合法复用 ✅

源码头部：`Copyright 2018 The Baidu Authors` + **Apache License 2.0**。

→ 我们可以**合法移植、修改、商用**，只需保留版权声明与许可文本。
这大幅降低了 M3 的工作量：**协议骨架不用从零写**。

## 六、修订后的路线

| 阶段 | 原计划 | 修订后 |
|---|---|---|
| M2 | 未知，可能要从零写 ADB host | **分两支**：<br>M2a **IP 路径**（USB 网络共享 → 只用 Winsock）← 先试这个<br>M2b ADB 路径（自己实现 ADB host）← 兜底 |
| M3 | 自己实现协议 | **在 Apache-2.0 现成实现上移植**（54 个 .proto 已收入 `docs/carlife-proto/`） |


---

## 触摸回传：已用参考实现证实（本轮）

此前触摸是最不确定的一环（只能从消息 ID 前缀推断）。查参考实现
`CTranRecvPackageProcess.cpp` 后已确证：

```cpp
//ctrol channel [HU->MD]                                      ← 通道：控制通道
int CTranRecvPackageProcess::sendCtrlTouchAction(S_TOUCH_ACTION* touchAction) {
    CarlifeTouchAction action;
    action.set_action(...); set_x(...); set_y(...);
    setPackageHeadType(MSG_TOUCH_ACTION);
    ...
    writeCtrlData(sendPackage.packageHead, CTRL_HEAD_LEN);     ← 包头长度
```

**结论**：

| 项目 | 值 | 依据 |
|---|---|---|
| 通道 | 控制通道 | 函数名前缀 `Ctrl` + 注释 `ctrol channel [HU->MD]` |
| 包头长度 | **8 字节** | `#define CTRL_HEAD_LEN 8` |
| 写法 A | `MSG_TOUCH_ACTION_DOWN/UP/MOVE` + `CarlifeTouchSinglePoint{x=1,y=2}` | 参考实现**正在使用** |
| 写法 B | `MSG_TOUCH_ACTION` + `CarlifeTouchAction{action=1,x=2,y=3}` | 参考实现里**被注释掉** |

**两套写法都实现，做成界面按钮现场切换** —— 因为从源码分不出手机接受哪一套。
