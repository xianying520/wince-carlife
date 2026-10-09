/* cllog.h — 分阶段日志
 *
 * 为什么要它：车机没法在我这边实测，所以现场那一次运行【必须能自己把
 * 「走到了哪一步、卡在哪一步、为什么」讲清楚】。屏幕上的状态栏只能显示一行，
 * 且一下车就没了；日志文件才能带回电脑上逐行分析。
 *
 * 设计要点：
 *   1. 【不缓冲】。每写一行都直接 WriteFile 落盘 —— 程序要是崩了，
 *      缓冲区里的内容会全部丢掉，正好丢掉最关键的那几行。
 *      这个教训来自本项目之前的一次实测（stdout 全缓冲，一崩就看不清走到哪）。
 *   2. 【UTF-8 带 BOM】，中文在 Windows 记事本里能直接看。
 *   3. 写不进去就自动换地方（见 cl_log_open 的候选目录），
 *      并把最终路径回显到屏幕上，用户照着去找文件就行。
 */
#ifndef CLLOG_H
#define CLLOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* 打开日志。返回 0 成功。路径可用 cl_log_path() 取。 */
int  cl_log_open(void);
void cl_log_close(void);

/* 一行带时间戳的日志（自动加 \r\n） */
void cl_log(const char *fmt, ...);

/* 阶段标记：把「走到第几步」做成一眼能看出来的一行 */
void cl_log_stage(int step, int total, const char *name);

/* 结果标记：ok=1 打✅，ok=0 打❌；失败时把原因一起写进去 */
void cl_log_step(const char *what, int ok, const char *why);

/* 十六进制 + 可读字符 dump（用于看真实码流的头部） */
void cl_log_hex(const char *tag, const unsigned char *d, int n);

/* 把原始字节单独存一个文件（比如真实视频码流），方便带回来离线分析。
 * 只写前 maxTotal 字节，避免把车机塞满。 */
void cl_log_dumpfile(const char *name, const unsigned char *d, int n);

/* 日志写入失败过几次（0 = 一直正常）。非 0 说明盘写不进去了，
 * 上层要把这件事显示到屏幕上 —— 因为日志本身已经不可靠了。 */
int  cl_log_write_failed(void);

/* 强制刷盘。关键几行写完后调一次，防止程序崩了把缓存内容全丢掉。 */
void cl_log_sync(void);

const char *cl_log_path(void);     /* 日志文件的完整路径（ANSI，给屏幕显示用） */
const char *cl_log_dir(void);      /* 日志所在目录 */

/* 便捷宏 */
#define CLOG(...)  cl_log(__VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif /* CLLOG_H */
