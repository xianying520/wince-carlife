/* clhost.h — 把 carlife.c 编到电脑上运行所需的兼容层
 *
 * 目的：让【真实的协议代码】在电脑上真正跑起来，对着一个"假手机"逐字节验证
 * 它发出的包。光读代码看不出字节序、对齐、时序这类错，跑一遍才看得见。
 *
 * 本文件只做类型与函数名的等价映射，【不改动任何协议逻辑】。
 */
#ifndef CLHOST_H
#define CLHOST_H

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <wchar.h>

typedef int     SOCKET;
typedef wchar_t WCHAR;          /* 与 L"..." 字面量类型一致，不能用 unsigned short */

#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)

#define closesocket(s)            close(s)
#define ioctlsocket(s, cmd, arg)  ioctl((s), (cmd), (arg))
#define WSAStartup(a, b)          (0)
#define WSACleanup()              (0)

/* errno → Winsock 语义。
 * 关键点：非阻塞 connect 在 Linux 上返回 EINPROGRESS，Windows 用的是
 * WSAEWOULDBLOCK。协议代码只判断"是不是 would-block"，所以必须把几个
 * 等价错误归一化，否则 connect 会被误判成失败。 */
#define CL_WSA_WOULDBLOCK 10035

static inline int clhost_wsa_error(void)
{
    int e = errno;
    if (e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS || e == EALREADY)
        return CL_WSA_WOULDBLOCK;
    return e;
}

#define WSAGetLastError() clhost_wsa_error()
#define WSAEWOULDBLOCK    CL_WSA_WOULDBLOCK

#endif /* CLHOST_H */
