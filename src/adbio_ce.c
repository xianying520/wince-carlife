/* adbio_ce.c — 见 adbio_ce.h。 */
#include "adbio_ce.h"

#include <string.h>
#include <stdio.h>

static HANDLE g_h = INVALID_HANDLE_VALUE;
static WCHAR  g_name[64];

/* 手写的宽字符串拷贝。
 * ⚠ 刻意不用 lstrcpynW / lstrcpyW：手上的 COREDLL 导出表里没有它们
 *   （只有 lstrcmpW / lstrcmpiW）。虽然本次链接通过了，但一旦真机上缺这个
 *   导出，程序会「一点就闪退」—— 这种代价太大，不值得为省一个循环去赌。
 *   手写 8 行的循环，行为完全确定。 */
static void w_copy(WCHAR *dst, const WCHAR *src, int cap)
{
    int i;
    if (cap <= 0) return;
    for (i = 0; i < cap - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

/* 候选设备名。ADB1: 是 EasyConnected 实际在用的名字（见 docs/ 里的证据），
 * 其余的是常见变体，多试几个不吃亏。 */
static const WCHAR *g_cands[] = {
    L"ADB1:", L"ADB0:", L"ADB2:", L"ADB3:", L"tADB1:", L"ADB:"
};

int adbio_ce_is_open(void)
{
    return g_h != INVALID_HANDLE_VALUE;
}

static int ce_read(void *ctx, unsigned char *buf, int len, int timeout_ms)
{
    DWORD got = 0;
    (void)ctx;

    if (g_h == INVALID_HANDLE_VALUE)
        return -1;

    /* 流驱动没有 SetCommTimeouts 那套，靠等句柄来模拟超时。
     * 大多数 WinCE 流驱动的句柄在有数据时会变成有信号状态。
     * ⚠ 如果某个驱动不支持，WaitForSingleObject 会立刻返回，
     *   于是 ReadFile 可能阻塞 —— 这一点只能靠真机实测确认。 */
    if (timeout_ms >= 0) {
        DWORD w = WaitForSingleObject(g_h, (DWORD)timeout_ms);
        if (w != WAIT_OBJECT_0)
            return 0;                       /* 超时：ADB 语义里 0 就是超时 */
    }

    if (!ReadFile(g_h, (LPVOID)buf, (DWORD)len, &got, NULL))
        return -1;
    return (int)got;
}

static int ce_write(void *ctx, const unsigned char *buf, int len)
{
    DWORD put = 0;
    (void)ctx;

    if (g_h == INVALID_HANDLE_VALUE)
        return -1;
    if (!WriteFile(g_h, (LPVOID)buf, (DWORD)len, &put, NULL))
        return -1;
    return (int)put;
}

ADB_IO adbio_ce_io(void)
{
    ADB_IO io;
    io.read  = ce_read;
    io.write = ce_write;
    io.ctx   = 0;
    return io;
}

int adbio_ce_open(WCHAR *out_name, int name_cap, char *reason, int reason_cap)
{
    int i, o = 0;
    char t[256];
    DWORD last = 0;

    adbio_ce_close();
    if (reason && reason_cap > 0) reason[0] = 0;

    for (i = 0; i < (int)(sizeof(g_cands) / sizeof(g_cands[0])); i++) {
        HANDLE h = CreateFileW(g_cands[i], GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            g_h = h;
            w_copy(g_name, g_cands[i], 64);
            if (out_name && name_cap > 0)
                w_copy(out_name, g_cands[i], name_cap);
            if (reason && reason_cap > 0)
                sprintf(reason, "已打开 ADB 设备 %ls", g_cands[i]);
            return 0;
        }
        last = GetLastError();
        o += snprintf(t, sizeof(t) - o > 0 ? sizeof(t) - o : 0,
                      "%ls(错误%lu) ", g_cands[i], (unsigned long)last);
    }

    if (reason && reason_cap > 0) {
        snprintf(reason, (size_t)reason_cap - 1,
                 "打不开任何 ADB 设备，试过：%s", o > 0 ? t : "(无)");
        reason[reason_cap - 1] = 0;
    }
    return -1;
}

void adbio_ce_close(void)
{
    if (g_h != INVALID_HANDLE_VALUE) {
        CloseHandle(g_h);
        g_h = INVALID_HANDLE_VALUE;
    }
    g_name[0] = 0;
}

/* 把 HKLM\Drivers\Active 里所有已加载的设备名列出来。
 * 这些名字就是 CreateFile 能用的流接口名 —— 万一 ADB 的设备名很特别，
 * 靠这份清单就能看出来它到底叫什么。 */
int adbio_ce_list_devices(char *out, int cap)
{
    int n = 0, used = 0;
    char t[128];

    if (!out || cap <= 0) return -1;
    out[0] = 0;

    for (;;) {
        WCHAR path[96], sub[16], nm[128];
        HKEY  k;
        DWORD cb = sizeof(nm) - sizeof(WCHAR), type = 0;

        wsprintfW(sub, L"%d", n);
        wsprintfW(path, L"Drivers\\Active\\%s", sub);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
            break;

        nm[0] = 0;
        if (RegQueryValueExW(k, L"Name", 0, &type, (BYTE *)nm, &cb) == ERROR_SUCCESS) {
            char a[128];
            int i;
            for (i = 0; i < 127 && nm[i]; i++)
                a[i] = (char)(nm[i] & 0xFF);      /* 设备名都是 ASCII */
            a[i] = 0;
            if (i > 0) {
                int w = snprintf(t, sizeof(t), "%s ", a);
                if (w > 0 && used + w < cap - 1) {
                    memcpy(out + used, t, (size_t)w);
                    used += w;
                    out[used] = 0;
                }
            }
        }
        RegCloseKey(k);
        n++;
        if (n > 200) break;
    }
    return used;
}
