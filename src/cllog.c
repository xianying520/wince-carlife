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

static int g_write_failed = 0;   /* 写日志失败的次数 */

/* ⚠⚠ 日志会被【两个线程】写：
 *     · 会话线程：cl_log_open + 连接过程
 *     · 主线程：界面初始化的那几步
 *   两个线程同时 WriteFile 同一个句柄、共用 g_write_failed 计数器，
 *   轻则输出交错，重则把计数器顶到阈值、触发切换函数去重新打开文件
 *   —— 而那个函数用的是 CREATE_ALWAYS，等于把已经写好的日志清空。
 *   实锤怀疑点：车机上日志永远只留 3 行就断，且换到内部存储后依然如此。
 *   整段写入用临界区串起来，从根上排除这一类问题。 */
static CRITICAL_SECTION g_log_lock;
static int              g_log_lock_ready = 0;
static void log_lock_enter(void)
{
    if (!g_log_lock_ready) {
        InitializeCriticalSection(&g_log_lock);
        g_log_lock_ready = 1;
    }
    EnterCriticalSection(&g_log_lock);
}
static void log_lock_leave(void)
{
    LeaveCriticalSection(&g_log_lock);
}

/* 写日志（不缓冲、立刻落盘）。
 * ⚠ 返回值【必须】检查：U 盘写满、被拔掉、变成只读，WriteFile 都会失败，
 *   而失败之后继续写只是白费力气。之前没检查 ——
 *   结果日志莫名其妙只留下前几行，完全看不出是盘的问题。
 *   现在失败次数会被记下来，由上层显示到屏幕上（屏幕还在，还来得及告诉用户）。 */
/* ⚠ 原来这里有个 switch_to_internal()：写失败就换目录重开日志。
 *   现在日志【本来就优先写内部存储】，这个函数只会做一件坏事 ——
 *   用 CREATE_ALWAYS 重新打开同一个路径 = 把已写好的日志清空。
 *   而且它由 g_write_failed 计数触发，多线程下计数不可靠。
 *   写失败就老老实实记下失败次数，让上层告诉用户，不再折腾。 */

static void write_bytes(const void *p, int n)
{
    DWORD bw = 0;
    if (g_h == INVALID_HANDLE_VALUE || n <= 0) return;
    if (!WriteFile(g_h, p, (DWORD)n, &bw, NULL) || bw != (DWORD)n)
        g_write_failed++;
}

/* 日志写入失败过几次（0 = 一直正常）。非 0 说明盘写不进去了。 */
int cl_log_write_failed(void) { return g_write_failed; }

/* 强制刷盘。关键几行写完后调一次：程序要是崩了，
 * 停在系统缓存里的内容会全部丢掉 —— 而那正好是最关键的那几行。 */
void cl_log_sync(void)
{
    if (g_h == INVALID_HANDLE_VALUE) return;
    log_lock_enter();
    FlushFileBuffers(g_h);
    log_lock_leave();
}

void cl_log(const char *fmt, ...)
{
    char buf[512];
    char line[600];
    va_list ap;
    unsigned long ms;
    int n;

    if (g_h == INVALID_HANDLE_VALUE) return;

    log_lock_enter();

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

    log_lock_leave();
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
    /* ⚠ 顺序很关键：【车机内部存储优先，U 盘放最后】。
     *
     *   实锤：程序放在 U 盘上跑、日志也写 U 盘，结果第 4 次 WriteFile
     *   就卡死/崩掉了（日志永远只有 3 行），而当时它跑在主线程上，
     *   于是整个程序跟着卡死 —— 窗口建出来了却一次都没画过，
     *   用户看到的就是「点了完全没反应」。
     *
     *   老 WinCE 的 U 盘驱动在写入上本来就脆弱，加上「从同一个盘读程序、
     *   又往同一个盘写日志」这种用法，出问题的概率更高。
     *   内部存储（iNAND）是车机自己的主存储，驱动成熟得多。
     *
     *   空串 = exe 自己所在目录（也就是 U 盘），放到最后兜底。 */
    static const WCHAR *cand[] = {
        L"\\iNAND",
        L"\\Residentflash2",
        L"\\SDMEM",
        L"\\",
        L"",                 /* 空串 = exe 目录（U 盘），兜底 */
        L"\\Windows"
    };
    WCHAR mod[MAX_PATH];
    WCHAR exedir[MAX_PATH];
    WCHAR path[MAX_PATH + 40];
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

        /* ⚠ BOM 必须是 UTF-8 的 EF BB BF（3 个字节）。
         *   之前写的是 WCHAR 0xFEFF，落盘成了 FF FE —— 那是【UTF-16LE】的 BOM。
         *   于是所有编辑器都把这个文件当 UTF-16 读，把正常的 UTF-8 字节
         *   两两配对成了汉字乱码。
         *   实锤：车机生成的日志打开就是「せ〮㠸⁝㴽㴽…」，
         *   解出来其实是「[0.88] ====…」。 */
        {
            const unsigned char bom3[3] = { 0xEF, 0xBB, 0xBF };
            write_bytes(bom3, 3);
        }

        /* ⚠ 日志头【一次性写完】：五次 WriteFile 就有五次出错机会，
         *   而实测正是在这中间断掉的（永远只留前 3 行）。
         *   拼成一整块、一次落盘，要么全有要么全无 —— 至少不会断在半截。 */
        {
            char head[700];
            int  p2 = 0, k;
            static const char *const h1 =
                "================================================\r\n"
                " CarLife 车机端 · 运行日志\r\n"
                " 日志文件: ";
            static const char *const h2 =
                "\r\n"
                " 这一份就是【请带回来分析】的文件。\r\n"
                "================================================\r\n";
            for (k = 0; h1[k] && p2 < (int)sizeof(head) - 2; k++) head[p2++] = h1[k];
            for (k = 0; g_pathA[k] && p2 < (int)sizeof(head) - 2; k++) head[p2++] = g_pathA[k];
            for (k = 0; h2[k] && p2 < (int)sizeof(head) - 2; k++) head[p2++] = h2[k];
            head[p2] = 0;
            log_lock_enter();
            write_bytes(head, p2);
            log_lock_leave();
        }
        cl_log_sync();
        return 0;
    }

    g_pathA[0] = 0;
    return -1;
}

void cl_log_close(void)
{
    if (g_h != INVALID_HANDLE_VALUE) {
        cl_log("=== 程序退出，日志结束 ===");
        log_lock_enter();
        CloseHandle(g_h);
        g_h = INVALID_HANDLE_VALUE;
        log_lock_leave();
    }
}
