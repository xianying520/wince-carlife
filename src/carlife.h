/* carlife.h — CarLife 车机端协议层（最小实现）
 *
 * 依据：Doris-duo/CarLife-MD-Lib（Apache-2.0，源自百度 CarLife-Vehicle-Lib）
 *       + 其 CarLifeLibTest.cpp 示例代码中的实测初始化参数。
 */
#ifndef CARLIFE_H
#define CARLIFE_H

#ifdef CL_HOST_TEST
/* 在电脑上跑协议测试时用等价兼容层代替 WinCE 的 socket 头。
 * 见 host/clhost.h —— 只做类型映射，不改协议逻辑。 */
#include "clhost.h"
#else
#include <winsock2.h>
#endif

/* ── 通道端口（车机主动连手机用）── */
#define CL_PORT_CMD   7240   /* 控制/握手 */
#define CL_PORT_VIDEO 8240   /* 视频流 */
#define CL_PORT_MEDIA 9240   /* 音频 */
#define CL_PORT_TTS   9241   /* 语音播报 */
#define CL_PORT_VR    9242   /* 语音识别 */
#define CL_PORT_TOUCH 9340   /* 触摸回传 */

/* ── 包头长度：CMD/CTRL 是 8 字节，其余通道是 12 字节 ── */
#define CL_HDR_CMD    8

/* 单帧上限。超过它说明长度字段被误读、流已损坏 ——
 * 直接判错，而不是傻乎乎地去收几百 MB 数据。 */
#define CL_MAX_FRAME (4 * 1024 * 1024)

/* 一条消息读到一半之后，最多再等多久把它读完（毫秒）。
 * 之所以要单独一个值：轮询用的短超时只适合"还没开始读"的情况，
 * 已经开始读就不能半途放弃，否则会丢掉字节、导致整个流永久错位。 */
#define CL_PARTIAL_WAIT_MS 2000
#define CL_HDR_MEDIA  12

/* ── 消息 ID ──
 * 规律：0x00018xxx = 车机发给手机(HU→MD)；0x00010xxx = 手机发给车机(MD→HU) */
#define CL_MSG_HU_PROTOCOL_VERSION        0x00018001UL
#define CL_MSG_PROTOCOL_VERSION_MATCH     0x00010002UL
#define CL_MSG_HU_INFO                    0x00018003UL
#define CL_MSG_MD_INFO                    0x00010004UL
#define CL_MSG_VIDEO_ENCODER_INIT         0x00018007UL
#define CL_MSG_VIDEO_ENCODER_INIT_DONE    0x00010008UL
#define CL_MSG_VIDEO_ENCODER_START        0x00018009UL
#define CL_MSG_VIDEO_ENCODER_PAUSE        0x0001800AUL
#define CL_MSG_VIDEO_ENCODER_RESET        0x0001800BUL
#define CL_MSG_VIDEO_ENCODER_FPS_CHANGE   0x0001800CUL
#define CL_MSG_VIDEO_ENCODER_JPEG         0x00018056UL
#define CL_MSG_VIDEO_ENCODER_JPEG_ACK     0x00010057UL
/* ── 触摸通道消息（注意 0x0006 前缀 = 独立通道）── */
#define CL_MSG_TOUCH_ACTION               0x00068001UL
#define CL_MSG_TOUCH_ACTION_DOWN          0x00068002UL
#define CL_MSG_TOUCH_ACTION_UP            0x00068003UL
#define CL_MSG_TOUCH_ACTION_MOVE          0x00068004UL
#define CL_MSG_TOUCH_CAR_HARD_KEY         0x00068008UL

#define CL_KEYCODE_HOME        0x00000001
#define CL_KEYCODE_PHONE_CALL  0x00000002
#define CL_KEYCODE_PHONE_END   0x00000003
#define CL_KEYCODE_NAVI        0x0000000B
#define CL_KEYCODE_MEDIA       0x00000009

#define CL_MSG_HU_AUTHEN_REQUEST          0x00018048UL
#define CL_MSG_MD_AUTHEN_RESPONSE         0x00010049UL

/* ── 车机侧协议版本（取自示例代码 huProtocolVersion={1,0}）── */
#define CL_VER_MAJOR 1
#define CL_VER_MINOR 0

/* ── 视频编码参数（取自示例代码 initVideoParam={768,480,30}）── */
#define CL_VIDEO_W   768
#define CL_VIDEO_H   480
#define CL_VIDEO_FPS 30

/* ── 返回码 ── */
#define CL_OK           0
#define CL_ERR_SOCKET  -1
#define CL_ERR_SEND    -2
#define CL_ERR_RECV    -3
#define CL_ERR_TIMEOUT -4

/* 建立到 ip:port 的 TCP 连接（非阻塞 connect + select 超时）。
 * 成功返回 socket，失败返回 INVALID_SOCKET。 */
SOCKET cl_connect(unsigned long ip_be, int port, int timeout_ms);

/* 发送一条 CMD 通道消息（自动拼 8 字节包头）。 */
int cl_send_cmd(SOCKET s, unsigned long msg_id,
                const unsigned char *body, int body_len);

/* 接收一条 CMD 通道消息。msg_id 回填消息 ID，body 回填数据体，
 * *body_len 回填实际长度。返回 CL_OK / 负错误码。 */
int cl_recv_cmd(SOCKET s, unsigned long *msg_id,
                unsigned char *body, int cap, int *body_len, int timeout_ms);

/* 完整握手：发 HU 协议版本 1.0，等手机回 match status。
 * 成功返回 CL_OK 并把手机返回的 matchStatus 写进 *match_status。 */
int cl_handshake(SOCKET s, int *match_status, unsigned long *reply_id);

/* 重发一次 HU_PROTOCOL_VERSION。
 * 参考实现（CarLifeLibTest.cpp）明确注释：
 * "it is necessary to send MSG_CMD_HU_PROTOCOL_VERSION message"
 * 且在【每个通道使用前】都要发一次（视频/音频/触摸都算）。
 * 发完顺手把可能到来的回应收掉，免得留在 CMD 缓冲里干扰后续读取。 */
int cl_resend_version(SOCKET s);

/* ── 视频通道 ──
 * 通知手机"以 w x h @ fps 编码"，手机回 VIDEO_ENCODER_INIT_DONE。 */
int cl_send_video_encoder_init(SOCKET s, int w, int h, int fps);

/* 通知手机开始推流。 */
int cl_send_video_encoder_start(SOCKET s);

/* 请求手机改用 JPEG 方式推帧（若手机支持，解码难度远低于 H.264）。
 * 手机成功会回 MSG_CMD_VIDEO_ENCODER_JPEG_ACK(0x00010057)。 */
int cl_send_video_encoder_jpeg(SOCKET s);

/* 判断一帧数据的编码格式，返回可读字符串（用于现场诊断）。 */
const WCHAR *cl_guess_codec(const unsigned char *p, int len);

/* ── 触摸回传 ──
 * hdr_len 传 CL_HDR_CMD(8) 或 CL_HDR_MEDIA(12)：
 * 参考实现把触摸归在 CTRL 通道（8 字节），但未实证，
 * 所以留成参数，两条都试，以手机有响应的那条为准。 */
/* 触摸回传：action 0=按下 1=抬起 2=移动。
 *
 * mode 决定用哪一套写法 —— 参考实现里两套都存在，但只有一套在用：
 *   mode 0：专用消息 TOUCH_ACTION_DOWN/UP/MOVE + CarlifeTouchSinglePoint{x,y}
 *           ← 参考实现当前在用的写法（推荐）
 *   mode 1：通用消息 TOUCH_ACTION + CarlifeTouchAction{action,x,y}
 *           ← 参考实现里被注释掉的写法
 * 包头已实证为 CTRL 8 字节（参考源码 CTRL_HEAD_LEN 8，且注释写明
 * "ctrol channel [HU->MD]"），不再作为参数。 */
int cl_send_touch_action(SOCKET s, int action, int x, int y, int mode);

/* 车机硬按键（上一曲/下一曲等），同样走 CTRL 通道 */
int cl_send_hard_key(SOCKET s, int keycode);

/* 生成候选手机地址表（同网段 .1/.129/.100 + 常见 USB 共享地址）。
 * 返回个数；调用前必须先 WSAStartup。 */
int cl_candidate_ips(unsigned long *out, int max);

/* 接收一个视频包（12 字节包头：size/timestamp/type，全 BE）。
 * 返回 CL_OK 并把时间戳、类型、数据体回填。 */
int cl_recv_video(SOCKET s, unsigned long *timestamp, unsigned long *vtype,
                  unsigned char *buf, int cap, int *len, int timeout_ms);

#endif /* CARLIFE_H */
