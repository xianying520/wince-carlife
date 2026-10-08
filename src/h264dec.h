/* h264dec.h — 车机端 H.264 解码
 *
 * 分工：本文件是【薄封装】，真正干活的是 h264bsd（Apache-2.0，源自 AOSP）。
 * 这里只负责三件 h264bsd 不管的事：
 *   1. 切帧 —— CarLife 视频通道到底怎么切帧没有实证，三种都支持并自动识别
 *   2. 裁剪 —— h264bsd 输出按 16 像素对齐，比真实画面宽/高，要按 SPS 里的
 *              cropping 参数裁掉补边
 *   3. 报告 —— 没有车机实测，所以必须把「到底发生了什么」原样带回窗口上：
 *              profile 是多少、切帧识别成哪种、出了几帧、错在哪
 *
 * 为什么不用车机的硬件解码器（HW_MSDK.dll 的 Vdec_*）：
 *   Vdec_* 的函数签名反不出来（msdkcore.dll 我们没有），没有车机可试，
 *   参数猜错就是崩溃。软解虽然费 CPU，但【在这台电脑上就能完整验证】。
 */
#ifndef H264DEC_H
#define H264DEC_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 返回值 ---- */
#define H264DEC_OK           0   /* 数据吃掉了，但还没出一帧 */
#define H264DEC_GOT_FRAME    1   /* 出了一帧，out_bgra 里是完整的 */
#define H264DEC_ERR_DATA    -1   /* 码流有问题 */
#define H264DEC_ERR_MEM     -2   /* 内存不够 */
#define H264DEC_ERR_PROFILE -3   /* profile 不支持（h264bsd 只支持 Baseline）*/

/* ---- 切帧方式（首帧自动识别后锁定）---- */
#define H264DEC_FMT_UNKNOWN 0
#define H264DEC_FMT_ANNEXB  1   /* 00 00 01 / 00 00 00 01 起始码 */
#define H264DEC_FMT_AVCC    2   /* 4 字节大端长度 + NAL */
#define H264DEC_FMT_RAW     3   /* 一条消息正好一个裸 NAL，无前缀 */

typedef struct H264DEC H264DEC;

H264DEC *h264dec_open(void);
void     h264dec_close(H264DEC *d);

/* 喂一段数据（一次喂一条 CarLife 视频消息即可）。
 *   out_bgra / out_cap : 调用者给的输出缓冲（BGRA，每像素 4 字节）
 *   out_w / out_h      : 出帧时填真实画面尺寸（已裁剪）
 * 返回 H264DEC_GOT_FRAME 才表示 out_bgra 有效。 */
int h264dec_feed(H264DEC *d, const unsigned char *data, int len,
                 unsigned char *out_bgra, int out_cap, int *out_w, int *out_h);

/* ---- 取证用 ---- */
int  h264dec_width  (H264DEC *d);   /* 真实画面宽（裁剪后） */
int  h264dec_height (H264DEC *d);
int  h264dec_profile(H264DEC *d);   /* profile_idc: 66=Baseline 77=Main 100=High */
int  h264dec_format (H264DEC *d);   /* H264DEC_FMT_* */
int  h264dec_frames (H264DEC *d);
const char *h264dec_format_name(int f);
const char *h264dec_log(H264DEC *d);   /* 最近一条人话状态，直接显示到窗口 */

/* 取证：h264bsdDecode 每次返回的 (返回码, 吃掉字节数) 序列。
 * 返回长度；codes/bytes 指向内部数组（最多 80 项），不要释放。 */
int h264dec_trace(H264DEC *d, const unsigned char **codes, const unsigned char **bytes);

/* 返回码 → 人话（0=RDY 1=PIC_RDY 2=HDRS_RDY 3=ERROR 4=PARAM_SET_ERROR 5=MEMALLOC_ERROR）*/
const char *h264dec_code_name(int c);

/* 首帧前 12 字节 → 猜编码。返回 1=H.264, 2=JPEG, 0=不确定 */
int h264dec_guess_codec(const unsigned char *p, int n);

/* profile_idc → 人话 */
const char *h264dec_profile_name(int p);

#ifdef __cplusplus
}
#endif
#endif /* H264DEC_H */
