/* hwdump.h — 车机硬件 / 网络 / ADB 能力清点
 *
 * 回答两个决定项目生死的问题：
 *   ① 这台车机有没有 ADB 的 USB 驱动？（主路线：车机做 ADB 主机 + 端口转发）
 *      —— 见 hw_probe_adb()，这是现在最关键的一项。
 *   ② 有没有 USB 网卡(RNDIS) 驱动？（备选路线：手机开 USB 网络共享拿 IP）
 *
 * ⚠ ADB 设备节点只有插上手机并处于 ADB 模式时才存在，
 *   所以本程序要跑两次对比：不插手机一次，插上手机一次。
 */
#ifndef HWDUMP_H
#define HWDUMP_H

#include <windows.h>

/* 清点并生成文本报告（内部缓存，可重复调用） */
void hw_dump(void);

/* 取回报告文本 */
const WCHAR *hw_dump_text(void);

#endif
