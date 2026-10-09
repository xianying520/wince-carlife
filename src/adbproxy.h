/* adbproxy.h — ADB 端口转发器
 *
 * 作用：把「手机上的某个端口」搬到「车机本机的 127.0.0.1:某个端口」上，
 * 这样 CarLife 协议层用普通 socket 连本地端口就行，底下是 USB 还是网络
 * 它完全不需要知道 —— 已验证过的 carlife.c 因此一行都不用改。
 *
 * 这正是 EasyConnected 的做法（它的字符串里有 AdbForwardServer、
 * ForwardEntiity::AcceptorThread、"re_connect 127.0.0.1 port: %d"）。
 */
#ifndef ADBPROXY_H
#define ADBPROXY_H

#include "adb.h"

/* 启动转发。services 形如 {"tcp:7240","tcp:8240","tcp:9340"}。
 * 本地监听端口由系统分配，写回 local_ports。
 * 成功返回 0；失败返回负数，reason 里给出可读原因（可直接显示给用户）。 */
int  adbp_start(const char * const *services, int n_services,
                unsigned short *local_ports,
                char *reason, int reason_cap);

/* 用外部提供的设备 I/O 启动转发（主机端到端测试用）。
 * adbp_start 内部会先打开 ADB 设备再调它。 */
int  adbp_start_with_io(ADB_IO io, const char * const *services, int n_services,
                        unsigned short *local_ports,
                        char *reason, int reason_cap);

/* 停止转发并释放设备 */
void adbp_stop(void);

/* 是否正在运行 */
int  adbp_running(void);

/* 可读的状态描述（给界面显示） */
const char *adbp_status(void);

/* 尝试在手机上把 CarLife / Jovi InCar 拉起来。
 * 转发被拒时调用（被拒通常意味着手机端没在跑）。
 * 成功返回 0；detail 里写一段可读的说明，现场排查时很有用。 */
int  adbp_launch_phone_app(char *detail, int cap);

/* 手机包名清单（诊断用，可能在手机上直接看出 CarLife 叫什么） */
const char *adbp_phone_packages(void);

/* 借已经建好的 ADB 通道，在手机上执行一条 shell 命令，把输出取回来。
 *
 * 为什么需要它：ADB 一旦通了，手机对我们就不是一个黑盒了 ——
 * 可以【直接问手机】它现在处在哪种 USB 配置里（getprop sys.usb.config），
 * 而不是靠猜「传输文件 / 连接车辆」哪个才行。现场那种二选一的争论，
 * 用一条命令就能变成数据。
 *
 * 成功返回 0，输出写进 out（已去掉行尾 \r\n 并截断）；失败返回负数。 */
int  adbp_shell(const char *cmd, char *out, int cap, int timeout_ms);

/* 最近一次"拉起手机端"的结果说明（现场排查用） */
const char *adbp_last_note(void);

/* 统计：已转发字节数，用于判断"到底有没有数据在流动" */
void adbp_stats(unsigned long *to_phone, unsigned long *from_phone);

#endif
