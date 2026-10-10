/* host/windows.h — 让 src/adbio_ce.c 能在电脑上编译。
 *
 * 它里头写的是 #include <windows.h>，而我们要的不是 Windows，
 * 是「这台车机那个 ADB 驱动的仿真」。编译时把 -Ihost 放在最前面，
 * 这个头就会被找到，内容就是 ceemu.h。 */
#ifndef HOST_WINDOWS_H
#define HOST_WINDOWS_H
#include "ceemu.h"
#endif
