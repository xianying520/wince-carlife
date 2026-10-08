/* cllog.c — 见 cllog.h 的说明 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "cllog.h"

static HANDLE g_h    = INVALID_HANDLE_VALUE;
static char   g_dirA[260]  = "";
static char   g_pathA[300] = "";
static unsigned long g_t0 = 0;

/* ── 宽窄转换（只用 ASCII 路径，手写最稳；不用 lstrcpynA 之类的 coredll 函数，
 *    本工具链的 coredll 里那些 A 版函数不一定导出）── */
static void w2a(const WCHAR *w, char *a, int cap)
{
    int i;
    if (cap <= 0) return;
    for (i = 0; i < cap - 1 && w[i]; i++)
        a[i] = (char)(w[i] < 128 ? w[i] : '?');
    a[i] = 0;
}

static void a2w(const char *a, WCHAR *w, int cap)
{
    int i;
    if (cap <= 0) return;
    for (i = 0; i < cap - 1 && a[i]; i++)
        w[i] = (WCHAR)(unsigned char)a[i];
    w[i] = 0;
}

/* 拼路径：dir + name（都在宽字符域里拼，避免非 ASCII 目录名被砸坏） */
static void joinw(WCHAR *out, int cap, const WCHAR *dir, const WCHAR *name)
{
    int i = 0, j = 0;
    while (dir[i] && i < cap - 2) { out[i] = dir[i]; i++; }
    if (i > 0 && out[i - 1] != L'\\' && i < cap - 2) out[i++] = L'\\';
    while (name[j] && i < cap - 1) out[i++] = name[j++];
    out[i] = 0;
}

/* 这个目录能不能写 —— 真去创建一个探针文件来试，比查属性可靠 */
static int dir_writable(const WCHAR *dir)
{
    WCHAR p[MAX_PATH + 40];
    HANDLE h;
    joinw(p, MAX_PATH + 40, dir, L"cllog.tst");
    h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    CloseHandle(h);
    DeleteFileW(p);
    return 1;
}

static void write_bytes(const void *p, int n)
{
    DWORD bw = 0;
    if (g_h == INVALID_HANDLE_VALUE || n <= 0) return;
    WriteFile(g_h, p, (DWORD)n, &bw, NULL);      /* 不缓冲，立刻落盘 */
}

void cl_log(const char *fmt, ...)
{
    char buf[512];
    char line[600];
    va_list ap;
    unsigned long ms;
    int n;

    if (g_h == INVALID_HANDLE_VALUE) return;

    buf[0] = 0;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;

    ms = GetTickCount() - g_t0;
    n = _snprintf(line, sizeof(line) - 3, "[%lu.%03lu] %s\r\n",
                  ms / 1000, ms % 1000, buf);
    if (n < 0) n = 0;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    write_bytes(line, n);
}

void cl_log_stage(int step, int total, const char *name)
{
    cl_log("──────────────────────────────────────────────");
    cl_log("【阶段 %d/%d】%s", step, total, name);
}

void cl_log_step(const char *what, int ok, const char *why)
{
    if (ok)
        cl_log("   OK   %s", what);
    else
        cl_log("   XX   %s    ← %s", what, why ? why : "原因未知");
}

void cl_log_hex(const char *tag, const unsigned char *d, int n)
{
    char line[128];
    int i, k;

    if (!d || n <= 0) return;
    if (n > 128) n = 128;

    for (i = 0; i < n; i += 16) {
        int p = 0, j;
        p += _snprintf(line + p, 16, "%04X  ", i);
        for (j = 0; j < 16; j++) {
            if (i + j < n)
                p += _snprintf(line + p, 8, "%02X ", d[i + j]);
            else
                p += _snprintf(line + p, 8, "   ");
        }
        p += _snprintf(line + p, 4, " |");
        for (j = 0; j < 16 && i + j < n; j++) {
            unsigned char c = d[i + j];
            line[p++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        line[p++] = '|';
        line[p] = 0;
        cl_log("%s %s", tag, line);
    }
}

/* 原始字节单独存文件：真实码流带回来，就能在电脑上离线分析
 * （是 JPEG 还是 H.264、什么 profile、怎么切帧 —— 全都看得到）。 */
#define DUMPFILE_TOTAL (256 * 1024)

void cl_log_dumpfile(const char *name, const unsigned char *d, int n)
{
    static DWORD written = 0;
    WCHAR wp[MAX_PATH + 40], wdir[260];
    HANDLE h;
    DWORD bw = 0;
    int k;

    if (!d || n <= 0 || written >= DUMPFILE_TOTAL) return;
    k = (int)(DUMPFILE_TOTAL - written);
    if (n < k) k = n;

    a2w(g_dirA, wdir, 260);
    {
        WCHAR wn[80];
        int i;
        for (i = 0; i < 78 && name[i]; i++) wn[i] = (WCHAR)name[i];
        wn[i] = 0;
        joinw(wp, MAX_PATH + 40, wdir, wn);
    }

    h = CreateFileW(wp, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    (written == 0) ? CREATE_ALWAYS : OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (written > 0) SetFilePointer(h, 0, NULL, FILE_END);

    if (written == 0) {
        /* 开头记一行说明，免得拿回来不知道这是什么 */
        const char *hdr = "CARLIFE-RAW-VIDEO v1\r\n"
                          "这是车机收到的原始视频通道字节，未做任何处理。\r\n"
                          "拿回来可以直接判断：JPEG / H.264、profile、切帧方式。\r\n"
                          "---- 以下是原始数据 ----\r\n";
        DWORD w2 = 0;
        WriteFile(h, hdr, (DWORD)strlen(hdr), &w2, NULL);
    }

    WriteFile(h, d, (DWORD)k, &bw, NULL);
    CloseHandle(h);

    if (written == 0)
        cl_log("   原始视频数据开始存到 %s.%s（最多 %d KB）",
               name, "", DUMPFILE_TOTAL / 1024);
    written += bw;
}

const char *cl_log_path(void) { return g_pathA; }
const char *cl_log_dir(void)  { return g_dirA; }

int cl_log_open(void)
{
    static const WCHAR *cand[] = {
        L"",                 /* 空串 = exe 自己所在目录，下面单独处理 */
        L"\\iNAND",
        L"\\Residentflash2",
        L"\\SDMEM",
        L"\\",
        L"\\Windows"
    };
    WCHAR mod[MAX_PATH];
    WCHAR exedir[MAX_PATH];
    WCHAR path[MAX_PATH + 40];
    WCHAR bom[1];
    int i, n;

    g_t0 = GetTickCount();

    /* 先算 exe 自己所在目录 —— 用户把整个文件夹拷到 U 盘/车机上，
     * 日志就落在同一个文件夹里，最好找。 */
    exedir[0] = 0;
    if (GetModuleFileNameW(NULL, mod, MAX_PATH) > 0) {
        int k = -1, j;
        for (j = 0; mod[j]; j++)
            if (mod[j] == L'\\') k = j;
        if (k > 0) {
            for (j = 0; j < k && j < MAX_PATH - 1; j++)
                exedir[j] = mod[j];
            exedir[j] = 0;
        }
    }

    for (i = 0, n = (int)(sizeof(cand) / sizeof(cand[0])); i < n; i++) {
        const WCHAR *dir;
        if (cand[i][0] == 0) {
            if (!exedir[0]) continue;
            dir = exedir;
        } else {
            dir = cand[i];
        }
        if (!dir_writable(dir)) continue;

        joinw(path, MAX_PATH + 40, dir, L"carlife-log.txt");
        g_h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_h == INVALID_HANDLE_VALUE) continue;

        w2a(dir, g_dirA, 260);
        w2a(path, g_pathA, 300);

        /* UTF-8 BOM，记事本打开中文不乱码 */
        bom[0] = 0xFEFF;
        write_bytes(bom, 2);

        cl_log("================================================");
        cl_log(" CarLife 车机端 · 运行日志");
        cl_log(" 日志文件: %s", g_pathA);
        cl_log(" 这一份就是【请带回来分析】的文件。");
        cl_log("================================================");
        return 0;
    }

    g_pathA[0] = 0;
    return -1;
}

void cl_log_close(void)
{
    if (g_h != INVALID_HANDLE_VALUE) {
        cl_log("=== 程序退出，日志结束 ===");
        CloseHandle(g_h);
        g_h = INVALID_HANDLE_VALUE;
    }
}
