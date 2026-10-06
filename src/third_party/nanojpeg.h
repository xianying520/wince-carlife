/* 只取 nanojpeg 的声明，不要实现。
 * nanojpeg.c 自己支持这种用法：定义 _NJ_INCLUDE_HEADER_ONLY 后包含它，
 * 就只得到函数声明。实现由单独编译的 nanojpeg.o 提供。
 */
#ifndef WINCE_NANOJPEG_H
#define WINCE_NANOJPEG_H

#define _NJ_INCLUDE_HEADER_ONLY
#include "nanojpeg.c"

#endif
