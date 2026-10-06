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

/* 启动转发。services 形如 {"tcp:7240","tcp:8240","tcp:9340"}。
 * 本地监听端口由系统分配，写回 local_ports。
 * 成功返回 0；失败返回负数，reason 里给出可读原因（可直接显示给用户）。 */
int  adbp_start(const char * const *services, int n_services,
                unsigned short *local_ports,
                char *reason, int reason_cap);

/* 停止转发并释放设备 */
void adbp_stop(void);

/* 是否正在运行 */
int  adbp_running(void);

/* 可读的状态描述（给界面显示） */
const char *adbp_status(void);

/* 统计：已转发字节数，用于判断"到底有没有数据在流动" */
void adbp_stats(unsigned long *to_phone, unsigned long *from_phone);

#endif
