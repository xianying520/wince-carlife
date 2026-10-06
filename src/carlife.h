/* carlife.h — CarLife 车机端协议层（最小实现）
 *
 * 依据：Doris-duo/CarLife-MD-Lib（Apache-2.0，源自百度 CarLife-Vehicle-Lib）
 *       + 其 CarLifeLibTest.cpp 示例代码中的实测初始化参数。
 */
#ifndef CARLIFE_H
#define CARLIFE_H

#include <winsock2.h>

/* ── 通道端口（车机主动连手机用）── */
#define CL_PORT_CMD   7240   /* 控制/握手 */
#define CL_PORT_VIDEO 8240   /* 视频流 */
#define CL_PORT_MEDIA 9240   /* 音频 */
#define CL_PORT_TTS   9241   /* 语音播报 */
#define CL_PORT_VR    9242   /* 语音识别 */
#define CL_PORT_TOUCH 9340   /* 触摸回传 */

/* ── 包头长度：CMD/CTRL 是 8 字节，其余通道是 12 字节 ── */
#define CL_HDR_CMD    8
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

#endif /* CARLIFE_H */
