/* check.c — 车机体检程序
 *
 * 为什么要有它：主程序在车机上「闪一下就没了」，而我这边只能靠推理，
 * 已经连续错了好几轮。骨架程序（8192 字节）在同一台车机上却跑得好好的。
 *
 * 所以这个程序【照骨架那套已验证成功的写法来】，逐项测试主程序用到的每一个
 * 能力，把结果同时显示在屏幕上、并写进日志（每步都强制刷盘）。
 *
 * 用法：跑一次，看屏幕。哪一项后面没有「OK」，哪一项就是主程序崩溃的原因。
 * 屏幕上看不清也没关系 —— 每项的结论都写进 carlife-check.txt 了。
 *
 * ⚠ 有意与主程序不同的两点，专门用来验证两个怀疑：
 *   1. 窗口【不带 WS_EX_TOPMOST】（骨架就没有）
 *   2. WM_CLOSE【不销毁窗口】，只计数并显示 ——
 *      如果它能一直留在屏幕上而主程序不行，
 *      那主程序就是被车机外壳用 WM_CLOSE 关掉的。
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MAXLINE 40

static WCHAR  g_line[MAXLINE][96];
static int    g_nline = 0;
static int    g_close_count = 0;      /* 收到过几次 WM_CLOSE */
static int    g_lbut_count   = 0;
static HANDLE g_log = INVALID_HANDLE_VALUE;

/* ── 日志：每写一行立刻刷盘，崩了也留得下 ── */
static void lg(const char *s)
{
    DWORD bw = 0;
    if (g_log == INVALID_HANDLE_VALUE) return;
    WriteFile(g_log, s, (DWORD)strlen(s), &bw, NULL);
    WriteFile(g_log, "\r\n", 2, &bw, NULL);
    FlushFileBuffers(g_log);
}

static void ln(const WCHAR *s)
{
    int k;
    if (g_nline >= MAXLINE) return;
    for (k = 0; k < 95 && s[k]; k++) g_line[g_nline][k] = s[k];
    g_line[g_nline][k] = 0;
    g_nline++;
}

/* ⚠ 上一个版本这里把 ANSI 字符串【逐字节】转成 WCHAR —— 而中文是 UTF-8，
 *   一个汉字三个字节被当成三个字符，屏幕上就是一堆乱码（实测踩过）。
 *   现在改成直接收宽字符串，不再做这种转换。 */
static void ln_ok(const WCHAR *tag, const WCHAR *val)
{
    WCHAR w[96];
    int i = 0, j;
    for (j = 0; tag[j] && i < 90; j++) w[i++] = tag[j];
    for (j = 0; val[j] && i < 94; j++) w[i++] = val[j];
    w[i] = 0;
    ln(w);
}

/* ── 一项测试：先写「开始」，做完写结果 ── */
/* ⚠ 测试名一律用【宽字符串】传进来 —— 窄字符串里放中文再逐字节转 WCHAR
 *   就是上一个版本屏幕乱码的原因。日志那边用窄串（UTF-8）单独拼。 */
static void begin(int n, const WCHAR *name, const char *nameA)
{
    char b[200];
    _snprintf(b, sizeof(b) - 1, "[%d] %s ... ", n, nameA);
    b[sizeof(b) - 1] = 0;
    lg(b);
    ln_ok(L"...  ", name);
}

static void done(int n, const WCHAR *name, const char *nameA, int ok)
{
    char b[220];
    _snprintf(b, sizeof(b) - 1, "[%d] %s -> %s", n, nameA, ok ? "OK" : "FAIL");
    b[sizeof(b) - 1] = 0;
    lg(b);
    ln_ok(ok ? L"OK   " : L"FAIL ", name);
}

/* ══════════ 各项测试 ══════════ */

static DWORD WINAPI dummy_thread(LPVOID p) { (void)p; return 0; }

/* 双线程同时写日志 —— 复现主程序那个「加锁之后反而更早崩」的竞态 */
static volatile int g_race_stop = 0;
static DWORD WINAPI race_thread(LPVOID p)
{
    (void)p;
    while (!g_race_stop)
        lg("  竞态线程写入一行");
    return 0;
}

static int test_malloc(void)
{
    void *p = malloc(512 * 1024);
    if (!p) return 0;
    memset(p, 0, 4096);            /* 碰一下，确保真的能写 */
    free(p);
    return 1;
}

static int test_brushes(void)
{
    HBRUSH b[4];
    int i;
    for (i = 0; i < 4; i++) {
        b[i] = CreateSolidBrush(RGB(20 * i, 60, 90));
        if (!b[i]) return 0;
    }
    for (i = 0; i < 4; i++) DeleteObject(b[i]);
    return 1;
}

static HFONT make_font(int h, int weight)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -h;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = DEFAULT_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    return CreateFontIndirectW(&lf);
}

static int test_font1(void)
{
    HFONT f = make_font(22, 700);
    if (!f) return 0;
    DeleteObject(f);
    return 1;
}

static int test_font5(void)
{
    HFONT f[5];
    int i;
    for (i = 0; i < 5; i++) {
        f[i] = make_font(12 + i * 4, i == 0 ? 700 : 400);
        if (!f[i]) return 0;
    }
    for (i = 0; i < 5; i++) DeleteObject(f[i]);
    return 1;
}

static int test_thread(void)
{
    DWORD tid = 0;
    HANDLE h = CreateThread(0, 0, dummy_thread, 0, 0, &tid);
    if (!h) return 0;
    WaitForSingleObject(h, 2000);
    CloseHandle(h);
    return 1;
}

static int test_log_open(void)
{
    g_log = CreateFileW(L"\\iNAND\\carlife-check.txt",
                        GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE) {
        /* 内部存储不行就退回程序自己所在目录（U 盘） */
        g_log = CreateFileW(L"carlife-check.txt",
                            GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    return (g_log != INVALID_HANDLE_VALUE);
}

static int test_race(void)
{
    DWORD tid = 0;
    HANDLE h;
    int i;

    g_race_stop = 0;
    h = CreateThread(0, 0, race_thread, 0, 0, &tid);
    if (!h) return 0;
    for (i = 0; i < 500; i++)
        lg("  主线程写入一行");
    g_race_stop = 1;
    WaitForSingleObject(h, 3000);
    CloseHandle(h);
    return 1;
}

static int test_getdc(void)
{
    HDC dc = GetDC(0);
    if (!dc) return 0;
    ReleaseDC(0, dc);
    return 1;
}

/* ══════════ WndProc ══════════ */
static LRESULT CALLBACK CheckProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc, r;
        TEXTMETRICW tm;
        int i, lh, y;

        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(120, 220, 160));
        memset(&tm, 0, sizeof(tm));
        GetTextMetricsW(dc, &tm);
        lh = tm.tmHeight + tm.tmExternalLeading;
        if (lh <= 0) lh = 18;

        y = 6;
        for (i = 0; i < g_nline; i++) {
            r.left = 8; r.top = y; r.right = rc.right - 8; r.bottom = y + lh + 2;
            DrawTextW(dc, g_line[i], -1, &r,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            y += lh;
        }
        /* 把 WM_CLOSE 计数显示在最显眼的位置 —— 这是本轮的关键怀疑点 */
        {
            WCHAR t[80];
            wsprintfW(t, L"WM_CLOSE 收到 %d 次   点击 %d 次", g_close_count, g_lbut_count);
            SetTextColor(dc, RGB(255, 200, 80));
            r.left = 8; r.top = rc.bottom - lh - 8; r.right = rc.right - 8;
            r.bottom = rc.bottom - 4;
            DrawTextW(dc, t, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        }
        EndPaint(h, &ps);
        return 0;
    }

    /* ⚠⚠ 关键：故意【不】销毁窗口。
     *   主程序在这里会 DestroyWindow 退出。如果车机外壳确实会发 WM_CLOSE，
     *   那主程序就是被它关掉的 —— 而这个计数会证明这一点。 */
    case WM_CLOSE:
        g_close_count++;
        ln(L"!! WM_CLOSE 收到（故意不关，只计数）");
        InvalidateRect(h, 0, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
        g_lbut_count++;
        InvalidateRect(h, 0, FALSE);
        return 0;

    case WM_KEYDOWN:
        /* 也故意不退出 —— 看看是不是按键把它弄没的 */
        g_lbut_count++;
        InvalidateRect(h, 0, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ══════════ WinMain：结构完全照骨架来 ══════════ */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
    WNDCLASSW wc;
    HWND      hwnd;
    MSG       msg;
    int       cx, cy, ok;

    (void)hPrev; (void)lpCmd;

    /* 第 1 项：先把日志开起来（后面每项都要记） */
    ok = test_log_open();
    lg("================================================");
    lg(" CarLife 车机体检程序");
    lg(" 每一项做完就刷盘；崩在哪一项，那一项就只有「...」没有结果。");
    lg("================================================");
    begin(1, L"打开日志文件", "打开日志文件");
    done(1, L"打开日志文件", "打开日志文件", ok);

    /* 第 2 项：建窗口（完全照骨架：memset + 不检查扩展样式） */
    begin(2, L"建窗口（memset 清零 + WS_POPUP，不带 TOPMOST）", "建窗口（不带 TOPMOST）");
    memset(&wc, 0, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = CheckProc;
    wc.hInstance     = hInst;
    wc.hCursor       = NULL;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"WinCECarLifeCheck";
    ok = RegisterClassW(&wc) ? 1 : 0;
    done(2, L"注册窗口类", "注册窗口类", ok);
    if (!ok) { MessageBoxW(0, L"RegisterClassW 失败", L"体检", MB_OK); return 1; }

    cx = GetSystemMetrics(SM_CXSCREEN);
    cy = GetSystemMetrics(SM_CYSCREEN);
    hwnd = CreateWindowExW(0, L"WinCECarLifeCheck", L"CarLife 体检",
                           WS_POPUP | WS_VISIBLE, 0, 0, cx, cy,
                           NULL, NULL, hInst, NULL);
    done(3, L"CreateWindowExW", "CreateWindowExW", hwnd ? 1 : 0);
    if (!hwnd) { MessageBoxW(0, L"CreateWindowExW 失败", L"体检", MB_OK); return 2; }

    ShowWindow(hwnd, nShow ? nShow : SW_SHOW);
    UpdateWindow(hwnd);
    done(4, L"ShowWindow + UpdateWindow", "ShowWindow + UpdateWindow", 1);

    /* 后面这些就是主程序用到、而骨架没用到的能力，逐项验 */
    begin(5, L"malloc 512KB", "malloc 512KB");
    done(5, L"malloc 512KB", "malloc 512KB", test_malloc());
    begin(6, L"CreateSolidBrush ×4", "CreateSolidBrush x4");
    done(6, L"CreateSolidBrush ×4", "CreateSolidBrush x4", test_brushes());
    begin(7, L"CreateFontIndirectW ×1", "CreateFontIndirectW x1");
    done(7, L"CreateFontIndirectW ×1", "CreateFontIndirectW x1", test_font1());
    begin(8, L"CreateFontIndirectW ×5", "CreateFontIndirectW x5");
    done(8, L"CreateFontIndirectW ×5", "CreateFontIndirectW x5", test_font5());
    begin(9, L"CreateThread", "CreateThread");
    done(9, L"CreateThread", "CreateThread", test_thread());
    begin(10, L"GetDC(0)/ReleaseDC", "GetDC(0)/ReleaseDC");
    done(10, L"GetDC(0)/ReleaseDC", "GetDC(0)/ReleaseDC", test_getdc());

    {   /* 第 11 项：双线程同时写日志 —— 主程序那个竞态 */
        int i;
        begin(11, L"双线程同时写日志 500 轮（竞态复现）", "双线程同时写日志 500 轮");
        ok = test_race();
        done(11, L"双线程同时写日志 500 轮", "双线程同时写日志 500 轮", ok);
        for (i = 0; i < 50; i++) { InvalidateRect(hwnd, 0, FALSE); UpdateWindow(hwnd); }
    }

    begin(12, L"InvalidateRect + UpdateWindow ×50", "InvalidateRect + UpdateWindow x50");
    done(12, L"InvalidateRect + UpdateWindow ×50", "InvalidateRect + UpdateWindow x50", 1);

    ln(L"────────────────────────────");
    ln(L"全部 12 项跑完，没有崩。");
    ln(L"下面看 WM_CLOSE 计数：如果它一直在涨，");
    ln(L"说明车机外壳在偷偷关我们的窗口 —— 那就是主程序的死因。");
    lg("全部 12 项跑完，没有崩。");
    InvalidateRect(hwnd, 0, FALSE);
    UpdateWindow(hwnd);

    /* 主循环：有意不设任何退出条件（除了窗口真被销毁） */
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    lg("=== 窗口被销毁，程序退出 ===");
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    return 0;
}
