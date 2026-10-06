/* adbio_ce.h — 把车机上的 ADB 设备包装成 ADB_IO
 *
 * 车机系统里装了 ADB 的 USB 客户端驱动，应用层用 CreateFile("ADB1:") 就能打开。
 * 设备名在不同 ROM 上可能不一样（ADB1: / ADB0: / tADB1: …），所以这里提供
 * 一个自动探测：挨个试，哪个能打开就用哪个。
 */
#ifndef ADBIO_CE_H
#define ADBIO_CE_H

#include "adb.h"
#include <windows.h>

/* 打开 ADB 设备。成功返回 0，并把实际打开的设备名写进 out_name。
 * 失败返回 -1，把尝试过的名字和错误码写进 reason（给界面显示用）。 */
int  adbio_ce_open(WCHAR *out_name, int name_cap, char *reason, int reason_cap);

void adbio_ce_close(void);

/* 取回包装好的读写接口，交给 adb_connect 用 */
ADB_IO adbio_ce_io(void);

/* 是否已打开 */
int  adbio_ce_is_open(void);

/* 列出车机上所有已加载的设备名（诊断用，拼进 reason/status） */
int  adbio_ce_list_devices(char *out, int cap);

#endif
