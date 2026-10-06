/*
 * wince-carlife / M1 骨架
 * ---------------------------------------------------------------------------
 * 目的有两个：
 *   1) 验证云端 CeGCC(arm-mingw32ce) 工具链产出的 exe 能在真车机上运行；
 *   2) 把车机的屏幕分辨率、WinCE 版本、CPU、内存画在窗口上（这些数据
 *      是后面做 CarLife 显示层所必需的，之前一直拿不到）。
 *
 * 为什么用真窗口 + 自绘，而不是 MessageBox：
 *   实测这台车机**只能显示一个 MessageBox**（连续两次 MessageBoxW 只出第一个）。
 *   真窗口不受这个限制，也是后面 CarLife 视频渲染要走的路，所以从骨架就奠定它。
 *
 * 退出：点屏幕，或按车机任意键。
 */

#include <windows.h>
#include <string.h>
#include <wchar.h>

#define MAX_LINES 80
#define LINE_LEN  128

static WCHAR g_lines[MAX_LINES][LINE_LEN];
static int   g_nlines = 0;

static void Add(LPCWSTR s)
{
    if (g_nlines >= MAX_LINES) return;
    /* 用 wcsncpy 而不是 lstrcpynW：COREDLL.DEF 导出表中没有 lstrcpynW */
    wcsncpy(g_lines[g_nlines], s, LINE_LEN - 1);
    g_lines[g_nlines][LINE_LEN - 1] = L'\0';
    g_nlines++;
}

/* ---------------------------------------------------------------- 信息采集 */
static void CollectInfo(void)
{
    WCHAR buf[LINE_LEN];
    int   cx, cy;
    HDC   hdc;

    /* --- 屏幕分辨率 --- */
    cx = GetSystemMetrics(SM_CXSCREEN);
    cy = GetSystemMetrics(SM_CYSCREEN);
    wsprintfW(buf, L"[屏幕] %d x %d  (SM_CXSCREEN/CYSCREEN)", cx, cy);
    Add(buf);

    hdc = GetDC(NULL);
    if (hdc)
    {
        int hx = GetDeviceCaps(hdc, HORZRES);
        int vy = GetDeviceCaps(hdc, VERTRES);
        int bpp = GetDeviceCaps(hdc, BITSPIXEL);
        int pxc = GetDeviceCaps(hdc, PLANES);
        wsprintfW(buf, L"[屏幕] %d x %d  %d位色  planes=%d (GetDeviceCaps)", hx, vy, bpp, pxc);
        Add(buf);
        ReleaseDC(NULL, hdc);
    }

    /* --- WinCE 版本 --- */
    {
        OSVERSIONINFOW osv;
        memset(&osv, 0, sizeof(osv));
        osv.dwOSVersionInfoSize = sizeof(osv);
        if (GetVersionExW(&osv))
        {
            wsprintfW(buf, L"[系统] WinCE %u.%u build %u",
                      (unsigned)osv.dwMajorVersion, (unsigned)osv.dwMinorVersion,
                      (unsigned)osv.dwBuildNumber);
            Add(buf);
        }
        else
        {
            Add(L"[系统] GetVersionExW 失败");
        }
    }

    /* --- 平台类型字符串 --- */
    {
        WCHAR plat[64];
        plat[0] = L'\0';
        if (SystemParametersInfoW(SPI_GETPLATFORMTYPE, sizeof(plat), plat, 0) && plat[0])
        {
            wsprintfW(buf, L"[平台] %s", plat);
            Add(buf);
        }
    }

    /* --- CPU --- */
    {
        SYSTEM_INFO si;
        memset(&si, 0, sizeof(si));
        GetSystemInfo(&si);
        wsprintfW(buf, L"[CPU ] 架构=%u 级别=%u 页面=%uKB 核数=%u",
                  (unsigned)si.wProcessorArchitecture,
                  (unsigned)si.wProcessorLevel,
                  (unsigned)(si.dwPageSize / 1024),
                  (unsigned)si.dwNumberOfProcessors);
        Add(buf);
    }

    /* --- 内存 --- */
    {
        MEMORYSTATUS ms;
        memset(&ms, 0, sizeof(ms));
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatus(&ms);
        wsprintfW(buf, L"[内存] 物理 %uMB  可用 %uMB",
                  (unsigned)(ms.dwTotalPhys / (1024 * 1024)),
                  (unsigned)(ms.dwAvailPhys / (1024 * 1024)));
        Add(buf);
    }

    Add(L"");
    Add(L"wince-carlife M1 骨架 —— 云端 CeGCC 工具链产物");
    Add(L"点屏幕或按任意键退出");
}

/* ------------------------------------------------------------------ 窗口过程 */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC   hdc = BeginPaint(hwnd, &ps);
        RECT  rc, rl;
        TEXTMETRIC tm;
        int   i, lh, y;
        HBRUSH bg;
        HGDIOBJ old;

        GetClientRect(hwnd, &rc);

        /* 深色背景（车机屏幕通常很亮，深底更好读） */
        bg  = CreateSolidBrush(RGB(16, 20, 28));
        old = SelectObject(hdc, bg);
        FillRect(hdc, &rc, bg);
        SelectObject(hdc, old);
        DeleteObject(bg);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(120, 220, 160));

        /* 注意：coredll.dll 不导出 TextOutW / GetTextExtentPoint32W，
           只能用 DrawTextW + GetTextMetricsW（已核对 COREDLL.DEF 导出表）。 */
        memset(&tm, 0, sizeof(tm));
        GetTextMetricsW(hdc, &tm);
        lh = tm.tmHeight + tm.tmExternalLeading;
        if (lh <= 0) lh = 18;

        y = 6;
        for (i = 0; i < g_nlines; i++)
        {
            rl.left   = 8;
            rl.top    = y;
            rl.right  = rc.right - 8;
            rl.bottom = y + lh + 2;
            DrawTextW(hdc, g_lines[i], -1, &rl,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            y += lh;
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_KEYDOWN:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* -------------------------------------------------------------------- 入口 */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
    WNDCLASS wc;
    HWND     hwnd;
    MSG      msg;
    int      cx, cy;

    (void)hPrev; (void)lpCmd;

    CollectInfo();

    memset(&wc, 0, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = NULL;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"WinCECarLifeHU";

    if (!RegisterClassW(&wc)) return 1;

    /* 全屏窗口 —— 车机上就该铺满 */
    cx = GetSystemMetrics(SM_CXSCREEN);
    cy = GetSystemMetrics(SM_CYSCREEN);

    hwnd = CreateWindowExW(0, L"WinCECarLifeHU", L"WinCE CarLife HU",
                           WS_POPUP | WS_VISIBLE,
                           0, 0, cx, cy,
                           NULL, NULL, hInst, NULL);
    if (!hwnd) return 2;

    ShowWindow(hwnd, nShow ? nShow : SW_SHOW);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
