/* ceemu.h — 把「这台车机的 ADB 驱动」在电脑上照实仿真出来
 *
 * ══════════════════════════════════════════════════════════════════════════
 *  为什么非要有这个（每一句都是从现场日志里读出来的，不是凭空设计的）
 *
 *  已经连着四趟车、四个 bug，全部落在 ADB / 设备这一层：
 *    1. 读设备时把「此刻还没数据」当成「设备坏了」→ 14 毫秒就自判失败
 *    2. 同步状态文字用的 _vsnprintf 在车上崩掉
 *    3. 转发线程握着 ADB 锁做阻塞读 → 会话线程的 shell 命令被挡在外面 66 秒
 *    4. 环形缓冲满了以后在 while 里死等 → 读线程再也不退出，整机卡死 1242 秒
 *
 *  这四个 bug 有一个共同点：**它们在电脑上全都能跑出来**，
 *  只要电脑上那个「假设备」肯像真驱动一样阻塞。
 *  以前没有这层仿真，所以只能拿车当测试机 —— 一趟车一个 bug。
 *
 *  原则只有一条：**照着现场看到的行为来，不照着「合理」来。**
 *    · ReadFile 是阻塞的，而且【无视调用方给的超时】—— 第 3 个 bug 的根；
 *    · ReadFile 有数据就立刻返回、【有多少给多少】，不填满缓冲区；
 *    · WriteFile 可能「成功但写 0 字节」——「写失败 0 次却秒退 -1」的根；
 *    · CreateFile 设备名不存在给 55；设备在但打不开给 110；
 *    · CloseHandle 碰上正在读的句柄不会立刻生效（读线程因此退出不了）。
 *
 *  最后一条尤其重要：它正是「延迟关闭」那条路径存在的理由，
 *  电脑上必须也能复现，否则又会写出一个只在车上才炸的 bug。
 * ══════════════════════════════════════════════════════════════════════════
 */
#ifndef CEEMU_H
#define CEEMU_H

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

/* ── 基本类型 ── */
typedef void          *HANDLE;
typedef void          *LPVOID;
typedef void          *HKEY;
typedef const void    *LPCVOID;
typedef unsigned long  DWORD;
typedef unsigned int   UINT;
typedef int            BOOL;
typedef unsigned char  BYTE;
typedef wchar_t        WCHAR;
typedef short          SHORT;
typedef void          *LPSECURITY_ATTRIBUTES;
typedef const WCHAR   *LPCWSTR;
typedef WCHAR         *LPWSTR;
typedef unsigned long *LPDWORD;

#define WINAPI
#define TRUE  1
#define FALSE 0

#define INVALID_HANDLE_VALUE ((HANDLE)0)
#define HKEY_LOCAL_MACHINE   ((HKEY)0)

/* ── 错误码（只列代码里真正用到的）── */
#define ERROR_SUCCESS           0u
#define ERROR_FILE_NOT_FOUND    2u
#define ERROR_ACCESS_DENIED     5u
#define ERROR_INVALID_HANDLE    6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_BAD_UNIT          20u
#define ERROR_GEN_FAILURE       31u
#define ERROR_DEV_NOT_EXIST     55u
#define ERROR_OPEN_FAILED       110u
#define ERROR_NO_MORE_ITEMS     259u
#define ERROR_IO_PENDING        997u
#define WAIT_OBJECT_0           0u
#define WAIT_TIMEOUT            258u
#define WAIT_FAILED             0xFFFFFFFFu

/* ── CreateFile / 注册表常量 ── */
#define GENERIC_READ          0x80000000u
#define GENERIC_WRITE         0x40000000u
#define OPEN_EXISTING         3u
#define CREATE_ALWAYS         2u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define KEY_READ              0x20019u
#define FILE_SHARE_READ       1u
#ifndef MAX_PATH
#define MAX_PATH              260
#endif

/* ── 错误码：按线程存 ── */
static __thread DWORD t_lasterr;
static inline DWORD GetLastError(void)    { return t_lasterr; }
static inline void  SetLastError(DWORD e) { t_lasterr = e; }

/* ── 时间 ── */
static inline DWORD GetTickCount(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((unsigned long long)ts.tv_sec * 1000ull
                 + (unsigned long long)(ts.tv_nsec / 1000000));
}
#define Sleep(ms) usleep((unsigned)(ms) * 1000u)

/* ── 临界区（可重入）── */
typedef pthread_mutex_t CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION *c)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(c, &a);
    pthread_mutexattr_destroy(&a);
}
#define EnterCriticalSection(c) pthread_mutex_lock(c)
#define LeaveCriticalSection(c) pthread_mutex_unlock(c)

/* ══ 句柄：线程 / 事件 / 设备，都是「可等待对象」══════════════════════════
 * ⚠ 必须做成可等待，而不是简单 pthread_join：
 *   被测代码里 WaitForSingleObject(h, 超时) 是【带超时】的，
 *   而现场那个卡死正是「等了 2 秒等不到就往下走」。
 *   join 会永远等下去，那就什么都测不出来。 */
enum { CE_THREAD = 1, CE_EVENT = 2, CE_FILE = 3 };

typedef struct ce_h {
    int             kind;
    int             done;
    int             signalled;
    int             fd;
    int             shut;          /* CloseHandle 已经调过 */
    int             reading;       /* 正卡在 read 里 */
    DWORD           opened_at;
    pthread_t       th;
    pthread_mutex_t m;
    pthread_cond_t  c;
    void         *(*fn)(void *);
    void           *arg;
} ce_h;

static inline ce_h *ce_new(int kind)
{
    ce_h *h = (ce_h *)calloc(1, sizeof(ce_h));
    if (!h) return 0;
    h->kind = kind;
    h->fd   = -1;
    pthread_mutex_init(&h->m, 0);
    pthread_cond_init(&h->c, 0);
    return h;
}

static inline void *ce_thread_main(void *p)
{
    ce_h *h = (ce_h *)p;
    h->fn(h->arg);
    pthread_mutex_lock(&h->m);
    h->done = 1;
    pthread_cond_broadcast(&h->c);
    pthread_mutex_unlock(&h->m);
    return 0;
}

static inline HANDLE CreateThread(LPVOID a, DWORD b, void *(*fn)(void *),
                                  LPVOID arg, DWORD c, DWORD *tid)
{
    ce_h *h = ce_new(CE_THREAD);
    (void)a; (void)b; (void)c;
    if (!h) return 0;
    h->fn = fn; h->arg = arg;
    if (tid) *tid = 1;
    if (pthread_create(&h->th, 0, ce_thread_main, h) != 0) { free(h); return 0; }
    return (HANDLE)h;
}

static inline HANDLE CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual,
                                  BOOL initial, LPCWSTR name)
{
    ce_h *h;
    (void)sa; (void)manual; (void)name;
    h = ce_new(CE_EVENT);
    if (!h) return 0;
    h->signalled = initial ? 1 : 0;
    return (HANDLE)h;
}

static inline BOOL SetEvent(HANDLE hd)
{
    ce_h *h = (ce_h *)hd;
    if (!h) return FALSE;
    pthread_mutex_lock(&h->m);
    h->signalled = 1;
    pthread_cond_broadcast(&h->c);
    pthread_mutex_unlock(&h->m);
    return TRUE;
}

static inline BOOL ResetEvent(HANDLE hd)
{
    ce_h *h = (ce_h *)hd;
    if (!h) return FALSE;
    pthread_mutex_lock(&h->m);
    h->signalled = 0;
    pthread_mutex_unlock(&h->m);
    return TRUE;
}

static inline DWORD WaitForSingleObject(HANDLE hd, DWORD ms)
{
    ce_h *h = (ce_h *)hd;
    struct timespec ts;
    DWORD t0;
    int   rc = 0;

    if (!h) return WAIT_FAILED;
    t0 = GetTickCount();

    pthread_mutex_lock(&h->m);
    for (;;) {
        if (h->kind == CE_THREAD && h->done)      { pthread_mutex_unlock(&h->m); return WAIT_OBJECT_0; }
        if (h->kind == CE_EVENT && h->signalled)  { h->signalled = 0; pthread_mutex_unlock(&h->m); return WAIT_OBJECT_0; }
        if (ms == 0)                              { pthread_mutex_unlock(&h->m); return WAIT_TIMEOUT; }
        {
            DWORD el = GetTickCount() - t0;
            long  left;
            if (el >= ms) { pthread_mutex_unlock(&h->m); return WAIT_TIMEOUT; }
            left = (long)(ms - el);
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += left / 1000;
            ts.tv_nsec += (left % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            rc = pthread_cond_timedwait(&h->c, &h->m, &ts);
            if (rc == ETIMEDOUT && GetTickCount() - t0 >= ms) {
                pthread_mutex_unlock(&h->m);
                return WAIT_TIMEOUT;
            }
        }
    }
}

static inline BOOL CloseHandle(HANDLE hd)
{
    ce_h *h = (ce_h *)hd;
    if (!h) return FALSE;

    /* ⚠ 照着真驱动来：如果这个句柄正卡在 read 里，
     *   CloseHandle【不会立刻生效】—— 我们只是把 shut 记下来。
     *   这正是现场「读线程退出不了、句柄收不了尾」的成因，
     *   必须能在电脑上复现，否则又会写出一个只在车上才炸的 bug。 */
    pthread_mutex_lock(&h->m);
    h->shut = 1;
    pthread_mutex_unlock(&h->m);

    if (h->kind == CE_THREAD) {
        if (h->done) { pthread_join(h->th, 0); free(h); }
        return TRUE;                       /* 还没结束就交给线程自己收尾 */
    }
    if (h->kind == CE_FILE) {
        if (h->reading) return TRUE;       /* 正卡在读里，关闭被推迟 */
        if (h->fd >= 0) close(h->fd);
        h->fd = -1;
        free(h);
        return TRUE;
    }
    free(h);
    return TRUE;
}

/* ══ 设备：一条 socket 就是那根 USB 线 ══════════════════════════════════
 * 测试程序负责把 socket 的另一头接到「假手机」上。 */
static int   g_ce_fd        = -1;      /* 设备管道 */
static int   g_ce_present   = 0;       /* 设备名在不在（55 / 110 用） */
static long  g_ce_read_ms   = 0;       /* 驱动自己的读超时，0 = 永远阻塞 */
static int   g_ce_zero_write = 0;      /* 非 0 = 先来几次「成功但写 0 字节」 */
static long  g_ce_read_calls = 0;
static long  g_ce_blocked_ms = 0;      /* 累计阻塞时长（诊断/断言用） */

static inline void ce_set_fd(int fd)      { g_ce_fd = fd; }
static inline void ce_set_present(int p)  { g_ce_present = p; }
static inline void ce_set_read_timeout(long ms) { g_ce_read_ms = ms; }
static inline void ce_set_zero_write(int n){ g_ce_zero_write = n; }
static inline long ce_read_calls(void)    { return g_ce_read_calls; }
static inline long ce_blocked_ms(void)    { return g_ce_blocked_ms; }

static inline int ce_wstr_eq(const WCHAR *a, const char *b)
{
    int i;
    for (i = 0; b[i]; i++)
        if (!a[i] || (unsigned)a[i] != (unsigned char)b[i]) return 0;
    return a[i] == 0;
}

/* 只认 ADB1:（和车机上的设备名一致）；其余名字一律给 55 */
static inline HANDLE CreateFileW(LPCWSTR name, DWORD acc, DWORD share,
                                 LPSECURITY_ATTRIBUTES sa, DWORD how,
                                 DWORD flags, HANDLE tmpl)
{
    ce_h *h;
    (void)acc; (void)share; (void)sa; (void)how; (void)flags; (void)tmpl;

    if (!name || !ce_wstr_eq(name, "ADB1:")) {
        SetLastError(ERROR_DEV_NOT_EXIST);         /* 55：设备名不存在 */
        return INVALID_HANDLE_VALUE;
    }
    if (!g_ce_present) {
        /* 驱动在、但 USB 管道没好 —— 现场那个 110 就是这个意思 */
        SetLastError(ERROR_OPEN_FAILED);
        return INVALID_HANDLE_VALUE;
    }
    if (g_ce_fd < 0) {
        SetLastError(ERROR_OPEN_FAILED);
        return INVALID_HANDLE_VALUE;
    }

    h = ce_new(CE_FILE);
    if (!h) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
    h->fd        = dup(g_ce_fd);       /* 各自持有，关掉一个不影响另一个 */
    h->opened_at = GetTickCount();
    if (h->fd < 0) { free(h); SetLastError(ERROR_OPEN_FAILED); return INVALID_HANDLE_VALUE; }
    return (HANDLE)h;
}

/* ⚠⚠ 这一段的每一行都是「照实现场行为」，不要图省事改成 non-blocking：
 *   真驱动就是会一直等下去，正是它把转发线程连人带锁一起卡住 66 秒。 */
static inline BOOL ReadFile(HANDLE hd, LPVOID buf, DWORD len,
                            LPDWORD got, LPVOID ov)
{
    ce_h *h = (ce_h *)hd;
    ssize_t n;
    DWORD   t0;

    (void)ov;
    if (got) *got = 0;
    if (!h || h->kind != CE_FILE || h->fd < 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }

    g_ce_read_calls++;
    t0 = GetTickCount();

    pthread_mutex_lock(&h->m);
    h->reading = 1;
    pthread_mutex_unlock(&h->m);

    if (g_ce_read_ms > 0) {                 /* 模拟驱动自己的读超时 */
        struct timeval tv;
        tv.tv_sec  = g_ce_read_ms / 1000;
        tv.tv_usec = (g_ce_read_ms % 1000) * 1000;
        setsockopt(h->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    } else {
        struct timeval tv = { 0, 0 };
        setsockopt(h->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    for (;;) {
        n = recv(h->fd, buf, (size_t)len, 0);   /* 阻塞；无视调用方给的超时 */
        if (n > 0) break;
        if (n == 0) {                            /* 对端关了 = 设备拔了 */
            SetLastError(ERROR_DEV_NOT_EXIST);
            pthread_mutex_lock(&h->m); h->reading = 0; pthread_mutex_unlock(&h->m);
            return FALSE;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 驱动自己的读超时到了：返回 0 字节，但【不算错】 */
            pthread_mutex_lock(&h->m); h->reading = 0; pthread_mutex_unlock(&h->m);
            g_ce_blocked_ms += (long)(GetTickCount() - t0);
            return TRUE;                        /* got 仍是 0 */
        }
        SetLastError(ERROR_DEV_NOT_EXIST);
        pthread_mutex_lock(&h->m); h->reading = 0; pthread_mutex_unlock(&h->m);
        return FALSE;
    }

    g_ce_blocked_ms += (long)(GetTickCount() - t0);
    pthread_mutex_lock(&h->m); h->reading = 0; pthread_mutex_unlock(&h->m);

    if (got) *got = (DWORD)n;
    return TRUE;
}

static inline BOOL WriteFile(HANDLE hd, LPCVOID buf, DWORD len,
                             LPDWORD put, LPVOID ov)
{
    ce_h *h = (ce_h *)hd;
    ssize_t n;
    (void)ov;

    if (put) *put = 0;
    if (!h || h->kind != CE_FILE || h->fd < 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }

    /* 「成功但写 0 字节」——现场「写失败 0 次却秒退 -1」就是这个 */
    if (g_ce_zero_write > 0) {
        g_ce_zero_write--;
        return TRUE;                        /* 没报错，就是没写进去 */
    }

    n = send(h->fd, buf, (size_t)len, MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EINTR) { if (put) *put = 0; return TRUE; }
        SetLastError(ERROR_DEV_NOT_EXIST);
        return FALSE;
    }
    if (put) *put = (DWORD)n;
    return TRUE;
}

/* ── 注册表：诊断功能用，这里只要不崩就行 ── */
static inline long RegOpenKeyExW(HKEY k, LPCWSTR path, DWORD opt,
                                 DWORD sam, HKEY *out)
{
    (void)k; (void)path; (void)opt; (void)sam; (void)out;
    return (long)ERROR_FILE_NOT_FOUND;
}
static inline long RegQueryValueExW(HKEY k, LPCWSTR name, LPDWORD res,
                                    LPDWORD type, BYTE *data, LPDWORD cb)
{
    (void)k; (void)name; (void)res; (void)type; (void)data; (void)cb;
    return (long)ERROR_FILE_NOT_FOUND;
}
static inline long RegCloseKey(HKEY k) { (void)k; return 0; }

/* ── wsprintfW：只支持 %s %d %u（本项目用到的就这些）── */
static inline int wsprintfW(LPWSTR out, LPCWSTR fmt, ...)
{
    va_list ap;
    int o = 0, i;
    va_start(ap, fmt);
    for (i = 0; fmt[i]; i++) {
        if (fmt[i] != L'%') { out[o++] = fmt[i]; continue; }
        i++;
        if (fmt[i] == L's') {
            const WCHAR *s = va_arg(ap, const WCHAR *);
            if (!s) s = L"(null)";
            while (*s) out[o++] = *s++;
        } else if (fmt[i] == L'd' || fmt[i] == L'u') {
            char t[16];
            int  n = 0, k;
            unsigned int v = va_arg(ap, unsigned int);
            do { t[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v && n < 15);
            for (k = n - 1; k >= 0; k--) out[o++] = (WCHAR)t[k];
        } else {
            out[o++] = fmt[i];
        }
    }
    out[o] = 0;
    va_end(ap);
    return o;
}

#endif /* CEEMU_H */
