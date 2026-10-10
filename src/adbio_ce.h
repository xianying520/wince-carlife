/* adbio_ce.h — 把车机上的 ADB 设备包装成 ADB_IO
 *
 * 车机系统里装了 ADB 的 USB 客户端驱动，应用层用 CreateFile("ADB1:") 就能打开。
 * 设备名在不同 ROM 上可能不一样（ADB1: / ADB0: / tADB1: …），所以这里提供
 * 一个自动探测：挨个试，哪个能打开就用哪个。
 *
 * ⚠ 行车机上的真身已经查清（BDCarlife/adb_driver.dll，百度随车机出厂自带）：
 *   · 它是【USB 主机端】的客户端驱动（导出 USBDeviceAttach / USBInstallDriver，
 *     并从 USBD.dll 调 RegisterClientDriverID / RegisterClientSettings）；
 *   · 它认的接口是 255_66_1（class 0xFF / subclass 0x42 / proto 0x01），
 *     也就是安卓的 ADB 接口 —— **手机上必须打开「USB 调试」才会出现**；
 *   · 注册表位置：Drivers\USB\ClientDrivers\ADB_Class、
 *     Drivers\USB\LoadClients\Default\Default\255_66_1\ADB_Class；
 *   · 设备名前缀就是 ADB，实例名 ADB1:。
 *   所以「ADB1: 打不开」的两种含义要分清楚：
 *     错误 55(ERROR_DEV_NOT_EXIST) = 驱动根本没加载 → 手机上没开 USB 调试；
 *     错误 110(ERROR_OPEN_FAILED)  = 驱动在、但 ADB_Open 失败 → 接口还没就绪。
 */
#ifndef ADBIO_CE_H
#define ADBIO_CE_H

#include "adb.h"
#include <windows.h>

/* 打开 ADB 设备。成功返回 0，并把实际打开的设备名写进 out_name。
 * 失败返回 -1，把尝试过的名字和错误码写进 reason（给界面显示用）。
 *
 * ⚠ 句柄是【黏住】的：已经打开着就直接复用，不会先关再开。
 *   现场日志证明「开→握手失败→关」之后，紧接着十几次 CreateFile("ADB1:")
 *   全部返回 110 —— 这个驱动的关闭是脏的，每 2 秒折腾一次等于把它
 *   一直按在坏状态里。要强制重开用 adbio_ce_reopen()。 */
int  adbio_ce_open(WCHAR *out_name, int name_cap, char *reason, int reason_cap);

void adbio_ce_close(void);

/* 强制重开：扔掉当前句柄，下一次 adbio_ce_open 会重新 CreateFile。
 * 用于「句柄可能已经随手机拔插失效」的场合。 */
void adbio_ce_reopen(void);

/* 取回包装好的读写接口，交给 adb_connect 用 */
ADB_IO adbio_ce_io(void);

/* 是否已打开 */
int  adbio_ce_is_open(void);

/* 设备是否已经确定消失（读到 ERROR_DEV_NOT_EXIST 之类），该丢句柄了 */
int  adbio_ce_fatal(void);

/* 列出车机上所有已加载的设备名（诊断用，拼进 reason/status） */
int  adbio_ce_list_devices(char *out, int cap);

/* 把 USB/ADB 相关的注册表状态逐行交给 emit 打印（诊断用）。
 * ⚠ 用回调而不是拼成一个大缓冲：cl_log 一行的缓冲只有 640 字节，
 *   拼成一坨会被截断，正好截掉最关键的后半段。 */
void adbio_ce_dump_usb_state(void (*emit)(const char *line));

/* 累加统计（诊断用）：读到的字节、写出的字节、读失败、写失败、
 * 「暂时没数据」的次数、最近一次错误码。现场只有一次机会，
 * 这几个数字是判断「到底卡在写不进去还是读不出来」的唯一依据。 */
void adbio_ce_stats(unsigned long *in_bytes, unsigned long *out_bytes,
                    unsigned long *read_err, unsigned long *write_err,
                    unsigned long *no_data, unsigned long *last_err);

/* 把上面那组计数清零（每轮开始时调一次，这样统计的是「这一轮」）。 */
void adbio_ce_stats_reset(void);

/* 读线程环形缓冲的水位（诊断用）。
 * used = 现在缓冲里还有多少字节没被取走；cap = 容量；drops = 因为满而丢过几次。
 * 现场判断「读线程跟不跟得上」就看这几个数。 */
void adbio_ce_ring(int *used, int *cap, unsigned long *drops);

/* 读线程还活着吗（0 = 还没起 / 已经退出） */
int  adbio_ce_reader_alive(void);

/* 「上一次 adbio_ce_open 是不是真的新开了一个句柄」——
 * 读一次就清零。用来判断「旧的 ADB 连接还算不算数」：
 * 换了新句柄 = 旧连接已经没了，必须重新握手；复用旧句柄 = 连接还活着。 */
int  adbio_ce_was_fresh(void);

/* 读线程侧的统计：设备到底吐了多少字节、读了几次、单次最大多少。
 * 这三个数和 adbio_ce_stats 里的「收到 N 字节」对不上时，
 * 就说明「设备在吐数据，但我们的协议层根本没在消费」——
 * 上一版程序卡死时就是这个形态（设备吐满 512KB，协议层只取了 24 字节）。 */
void adbio_ce_reader_stats(unsigned long *bytes, unsigned long *reads,
                           unsigned long *maxone);

#endif
