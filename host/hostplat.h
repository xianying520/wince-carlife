/* hostplat.h — 让"车机专用"的模块能在电脑上编译运行的平台垫片
 *
 * 只做类型与函数名等价映射，不改动任何业务逻辑。
 * 已有一份 clhost.h 负责 socket 那部分，这里补线程与休眠。
 *
 * 为什么值得做：转发器（adbproxy.c）是现场真正要跑的代码，但它的核心
 * 是一个 select 循环 —— 光读代码看不出"事件来了会不会漏掉""通道关了会不会
 * 卡死"这类问题，只有真跑一遍才知道。有了这层垫片，就能在电脑上用真实的
 * 转发器代码对着一个假手机端到端地跑。
 */
#ifndef HOSTPLAT_H
#define HOSTPLAT_H

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

typedef void          *HANDLE;
typedef unsigned long  DWORD;
typedef void          *LPVOID;

#define WINAPI
#define Sleep(ms)              usleep((unsigned)(ms) * 1000)
#define _vsnprintf             vsnprintf
#define MAKEWORD(a, b)         ((unsigned short)((a) | ((b) << 8)))

static inline HANDLE hostplat_spawn(void *(*fn)(void *), void *arg)
{
    pthread_t *t = (pthread_t *)malloc(sizeof(pthread_t));
    if (!t) return NULL;
    if (pthread_create(t, NULL, fn, arg) != 0) {
        free(t);
        return NULL;
    }
    return (HANDLE)t;
}

static inline DWORD hostplat_join(HANDLE h)
{
    if (h) {
        pthread_join(*(pthread_t *)h, NULL);
        free(h);
    }
    return 0;
}

#define CreateThread(a, b, fn, arg, c, tid)  hostplat_spawn((fn), (arg))
#define WaitForSingleObject(h, ms)           hostplat_join(h)
#define CloseHandle(h)                       ((void)(h))

#endif /* HOSTPLAT_H */
