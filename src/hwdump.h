/* hwdump.h — 车机硬件/网络能力清点
 *
 * 目的只有一个：回答「这台车机到底有没有 USB 网卡(RNDIS) 驱动」。
 * 因为整个项目的核心假设是"车机经 USB 拿到手机网络 → 普通 TCP 连手机"，
 * 而这个假设的前提就是车机上必须存在 USB 网卡驱动。
 * 没有的话，手机开 USB 网络共享也不会冒出网络接口，整条路就是死的。
 */
#ifndef HWDUMP_H
#define HWDUMP_H

#include <windows.h>

/* 清点并生成文本报告（内部缓存，可重复调用） */
void hw_dump(void);

/* 取回报告文本 */
const WCHAR *hw_dump_text(void);

#endif
