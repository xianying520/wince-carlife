/* adbio_ce.c — 见 adbio_ce.h。 */
#include "adbio_ce.h"
#include "cllog.h"

#include <string.h>
#include <stdio.h>

static HANDLE g_h = INVALID_HANDLE_VALUE;
static WCHAR  g_name[64];

/* ── 诊断计数 ──
 * 现场只有一次机会，所以每个失败点都要留下数字证据：
 * 到底是「写不进去」、「读不出来」、还是「读得到但此刻没数据」。
 * 这三种以前全都塌成同一个 -1，现场根本分不清。 */
static unsigned long g_in_bytes;      /* 累计读到的字节 */
static unsigned long g_out_bytes;     /* 累计写出的字节 */
static unsigned long g_err_read;      /* ReadFile 真失败次数 */
static unsigned long g_err_write;     /* WriteFile 真失败次数 */
static unsigned long g_no_data;       /* ReadFile 成功但 0 字节（暂时没数据） */
static unsigned long g_last_err;      /* 最近一次失败的错误码 */
static int           g_err_run;       /* 连续读失败计数（用来避免空转） */
static int           g_fatal;         /* 设备已确认消失，句柄该丢了 */

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

/* 候选设备名。ADB1: 是这台车机实际在用的名字 —— 两条独立证据：
 *   ① 车机自带 CECarLife.exe 的字符串里就有 "ADB1:"；
 *   ② 车机自带 adb_driver.dll 的注册表值 Name = "ADB1:"。
 * 其余的是常见变体，多试几个不吃亏。 */
static const WCHAR *g_cands[] = {
    L"ADB1:", L"ADB0:", L"ADB2:", L"ADB3:", L"tADB1:", L"ADB:"
};

int adbio_ce_is_open(void)
{
    return g_h != INVALID_HANDLE_VALUE;
}

int adbio_ce_fatal(void)
{
    return g_fatal;
}

void adbio_ce_stats(unsigned long *in_bytes, unsigned long *out_bytes,
                    unsigned long *read_err, unsigned long *write_err,
                    unsigned long *no_data, unsigned long *last_err)
{
    if (in_bytes)  *in_bytes  = g_in_bytes;
    if (out_bytes) *out_bytes = g_out_bytes;
    if (read_err)  *read_err  = g_err_read;
    if (write_err) *write_err = g_err_write;
    if (no_data)   *no_data   = g_no_data;
    if (last_err)  *last_err  = g_last_err;
}

void adbio_ce_stats_reset(void)
{
    g_in_bytes = 0;
    g_out_bytes = 0;
    g_err_read = 0;
    g_err_write = 0;
    g_no_data = 0;
    g_last_err = 0;
}

/* 这个错误码是不是「设备真的没了」。
 * 只有这几种才允许把句柄判死；其余的（尤其是「现在没数据」）都必须继续等。 */
static int err_is_fatal(DWORD e)
{
    return (e == ERROR_DEV_NOT_EXIST || e == ERROR_BAD_UNIT ||
            e == ERROR_INVALID_HANDLE || e == ERROR_ACCESS_DENIED);
}

/* ⚠⚠ 本文件最关键的一段修改，先说清楚为什么。
 *
 *   现场日志（carlife-log.txt）里反复出现这一幕：
 *       [19.581] ADB 设备已打开: ADB1:
 *       [19.595]    XX   ADB 设备打开 + 认证 + 端口转发  ← ADB 认证失败（错误 -1）
 *   也就是说：设备【打开成功】了，但只过了 14 毫秒就报「认证失败」。
 *   一次真正的协议握手不可能 14 毫秒就失败 —— 8 秒的等待时间根本没走到。
 *
 *   原因就在这个函数里：以前读一次，ReadFile 不成功就直接返回 -1，
 *   而上层把 -1 理解成「传输出错，握手失败」。
 *   可这台车机的 ADB 驱动是【非阻塞】的：刚发完 CNXN 去读应答时，
 *   它必然先返回一次「此刻没数据」，于是我们立刻自己判了自己死刑。
 *
 *   现在改成：只有「设备真的没了」才算失败，其余一律在超时预算内接着等。
 *   超时预算到了才返回 0（= 超时），由上层区分「手机没应答」和「设备读不了」。 */
static int ce_read(void *ctx, unsigned char *buf, int len, int timeout_ms)
{
    DWORD t0;
    (void)ctx;

    if (g_h == INVALID_HANDLE_VALUE)
        return -1;

    t0 = GetTickCount();
    g_err_run = 0;

    for (;;) {
        DWORD got = 0;

        if (ReadFile(g_h, (LPVOID)buf, (DWORD)len, &got, NULL)) {
            if (got > 0) {
                g_in_bytes += got;
                g_err_run = 0;
                return (int)got;
            }
            g_no_data++;                       /* 0 字节 = 现在还没数据，不是错 */
            g_err_run = 0;
        } else {
            DWORD e = GetLastError();
            g_last_err = e;
            if (err_is_fatal(e)) {
                g_err_read++;
                g_fatal = 1;
                return -1;
            }
            /* 非致命错误：可能就是驱动表达「暂时没有数据」的方式。
             * 连续太多次还一次都没读到就认输 —— 免得把 8 秒空转掉，
             * 也免得日志里看不出到底属于哪种情况。 */
            if (++g_err_run >= 16) {
                g_err_read++;
                g_err_run = 0;
                return -1;
            }
        }

        if (timeout_ms <= 0)
            return 0;                          /* 非阻塞：这一轮就到这 */

        if ((int)(GetTickCount() - t0) >= timeout_ms)
            return 0;                          /* 超时：上层要把它和「出错」分开 */

        Sleep(5);                              /* 别把 CPU 打满 */
    }
}

static int ce_write(void *ctx, const unsigned char *buf, int len)
{
    DWORD put = 0;
    (void)ctx;

    if (g_h == INVALID_HANDLE_VALUE)
        return -1;
    if (!WriteFile(g_h, (LPVOID)buf, (DWORD)len, &put, NULL)) {
        DWORD e = GetLastError();
        g_last_err = e;
        g_err_write++;
        if (err_is_fatal(e))
            g_fatal = 1;
        return -1;
    }
    g_out_bytes += put;
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

/* ── 注册表小工具（诊断用）── */
static int reg_has_key(const WCHAR *path)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    RegCloseKey(k);
    return 1;
}

static int reg_get_str(const WCHAR *path, const WCHAR *val, char *out, int cap)
{
    HKEY  k;
    DWORD cb, type = 0;
    WCHAR buf[256];
    int   i;

    if (!out || cap <= 0) return -1;
    out[0] = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return -1;
    buf[0] = 0;
    cb = (DWORD)(sizeof(buf) - sizeof(WCHAR));
    if (RegQueryValueExW(k, val, 0, &type, (BYTE *)buf, &cb) != ERROR_SUCCESS) {
        RegCloseKey(k);
        return -1;
    }
    RegCloseKey(k);
    for (i = 0; i < cap - 1 && buf[i] && i < 255; i++)
        out[i] = (char)(buf[i] & 0xFF);        /* 驱动名/前缀都是 ASCII */
    out[i] = 0;
    return i > 0 ? 0 : -1;
}

static void report_key(void (*emit)(const char *line), const char *label,
                       const WCHAR *path, int with_values)
{
    char line[300], v[220];
    int  has = reg_has_key(path);

    snprintf(line, sizeof(line), "   %s : %s", label, has ? "有" : "没有");
    emit(line);
    if (!has || !with_values) return;

    if (reg_get_str(path, L"Dll", v, (int)sizeof(v)) == 0) {
        snprintf(line, sizeof(line), "      Dll = %s", v);
        emit(line);
    }
    if (reg_get_str(path, L"Prefix", v, (int)sizeof(v)) == 0) {
        snprintf(line, sizeof(line), "      Prefix = %s", v);
        emit(line);
    }
    if (reg_get_str(path, L"Order", v, (int)sizeof(v)) == 0) {
        snprintf(line, sizeof(line), "      Order = %s", v);
        emit(line);
    }
}

/* 把 USB/ADB 的注册表现状逐行交出去。
 * 这一份是判断「ADB1: 为什么打不开」的根：驱动没注册 = 错误 55 的由来。 */
void adbio_ce_dump_usb_state(void (*emit)(const char *line))
{
    static char devs[1024];
    char line[1100];

    if (!emit) return;

    emit("   ── ADB 驱动注册表（HKLM\\Drivers\\USB）──");
    report_key(emit, "ClientDrivers", L"Drivers\\USB\\ClientDrivers", 0);
    report_key(emit, "ClientDrivers\\ADB_Class",
               L"Drivers\\USB\\ClientDrivers\\ADB_Class", 1);
    report_key(emit, "ClientDrivers\\ADB_Driver",
               L"Drivers\\USB\\ClientDrivers\\ADB_Driver", 1);
    report_key(emit, "LoadClients", L"Drivers\\USB\\LoadClients", 0);
    report_key(emit, "LoadClients\\Default\\Default",
               L"Drivers\\USB\\LoadClients\\Default\\Default", 0);
    report_key(emit, "LoadClients\\..\\255_66_1",
               L"Drivers\\USB\\LoadClients\\Default\\Default\\255_66_1", 0);
    report_key(emit, "LoadClients\\..\\255_66_1\\ADB_Class",
               L"Drivers\\USB\\LoadClients\\Default\\Default\\255_66_1\\ADB_Class", 1);

    emit("   ── 已加载的设备名（HKLM\\Drivers\\Active）──");
    devs[0] = 0;
    if (adbio_ce_list_devices(devs, (int)sizeof(devs)) > 0) {
        snprintf(line, sizeof(line), "   %s", devs);
        emit(line);
    } else {
        emit("   （一个都读不到）");
    }
}

int adbio_ce_open(WCHAR *out_name, int name_cap, char *reason, int reason_cap)
{
    int i, o = 0;
    char t[256];
    DWORD last = 0;

    if (reason && reason_cap > 0) reason[0] = 0;

    /* ⚠ 句柄黏住，不先关再开。
     *   现场日志里出现了非常清楚的一段：
     *     19.581 打开成功 → 握手失败 → 关闭
     *     21.6 ~ 56.5  ADB1: 连续 18 次返回 110(ERROR_OPEN_FAILED)
     *     58.576 又能打开了
     *   也就是这个驱动的「关」是脏的，关一次要几十秒才缓过来；
     *   而我们每 2 秒就给它来一轮开关，等于一直把它按在坏状态里。
     *   能复用就复用。真需要重开时走 adbio_ce_reopen()。 */
    if (g_h != INVALID_HANDLE_VALUE) {
        if (out_name && name_cap > 0)
            w_copy(out_name, g_name, name_cap);
        if (reason && reason_cap > 0)
            sprintf(reason, "复用已打开的 ADB 设备 %ls", g_name);
        return 0;
    }

    g_fatal = 0;

    for (i = 0; i < (int)(sizeof(g_cands) / sizeof(g_cands[0])); i++) {
        HANDLE h = CreateFileW(g_cands[i], GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);
        {
            /* 把「试了哪个 ADB 设备名、成没成」记进日志 ——
             * 这是车机上最容易卡住的一步，而光看屏幕只知道"打不开"。 */
            char nm[32];
            int q;
            for (q = 0; q < 31 && g_cands[i][q]; q++)
                nm[q] = (char)g_cands[i][q];
            nm[q] = 0;
            if (h != INVALID_HANDLE_VALUE)
                cl_log("ADB 设备已打开: %s", nm);
            else
                cl_log("ADB 设备 %s 打不开（错误 %lu）", nm,
                       (unsigned long)GetLastError());
        }
        if (h != INVALID_HANDLE_VALUE) {
            g_h = h;
            g_err_run = 0;
            w_copy(g_name, g_cands[i], 64);
            /* 刚打开时给驱动一点时间把 USB 管道挂好。
             * 被 USB 枚举/管道建立挡掉第一包是这类驱动常见的小毛病，
             * 代价只有 150 毫秒，而且只在新开的那一次付。 */
            Sleep(150);
            if (out_name && name_cap > 0)
                w_copy(out_name, g_cands[i], name_cap);
            if (reason && reason_cap > 0)
                sprintf(reason, "已打开 ADB 设备 %ls", g_cands[i]);
            return 0;
        }
        last = GetLastError();
        {
            int left = (int)sizeof(t) - o;
            if (left <= 0) break;
            o += snprintf(t + o, (size_t)left, "%ls(错误%lu) ",
                          g_cands[i], (unsigned long)last);
        }
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
    g_err_run = 0;
    g_fatal = 0;
}

void adbio_ce_reopen(void)
{
    adbio_ce_close();
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
