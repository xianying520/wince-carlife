/* ceemu.c — 假设备的状态变量【唯一的一份定义】。
 * 见 ceemu.h 里那段说明：这几个必须是 extern，否则每个 .c 各拿一份，
 * 测试程序摆好的设备 adbio_ce.c 根本看不见。 */
#include "ceemu.h"

int  g_ce_fd         = -1;
int  g_ce_present    = 0;
long g_ce_read_ms    = 0;
int  g_ce_zero_write = 0;
long g_ce_read_calls = 0;
long g_ce_blocked_ms = 0;
