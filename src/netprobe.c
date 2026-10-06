/* netprobe.c — WinCE 车机端「CarLife 连接探测」工具
 *
 * 一趟车机现场回答两件事：
 *   ① 车机插上开了「USB 网络共享」的手机后能否拿到 IP？  → 决定 M2a 走不走得通
 *   ② 若能拿到，手机的 CarLife 端口(7240 等)可达吗？      → 决定协议层能否直接开工
 *
 * 只依赖 WinCE 自带 ws2.dll（真机实证存在）。
 * 严禁链接 libws2_32.a —— 那是桌面 WS2_32.dll，车机上不存在。
 *
 * UI：单个真窗口 + DrawTextW 自绘。
 * 严禁 MessageBox —— 真机实证车机只显示第一个弹窗，第二个会被吞。
 *
 * 注意：WinCE 下 wsprintfW 的 %s 要求宽字符指针，本文件所有传给 %s 的
 *       实参都是 WCHAR*，绝不传 char*。
 */

#include <winsock2.h>
#include <windows.h>
#include <string.h>
#include "carlife.h"
#include "uicommon.h"

#define REPORT_CAP 8192
#define MAX_IP     8

static WCHAR g_report[REPORT_CAP];
static int   g_len = 0;
static HWND  g_hwnd = 0;
static int   g_first = 0;   /* 当前页从第几行开始 */
static int   g_total = 0;

/* ── 追加一行到报告缓冲 ── */
static void app(const WCHAR *s)
{
    int n = (int)wcslen(s);
    if (g_len + n + 2 >= REPORT_CAP)
        return;
    wcscpy(g_report + g_len, s);
    g_len += n;
    g_report[g_len++] = L'\r';
    g_report[g_len++] = L'\n';
    g_report[g_len]   = 0;
}

/* ── IP(网络字节序 ulong) → 点分字符串 ── */
static void ip_str(unsigned long ip_be, WCHAR *out)
{
    const unsigned char *p = (const unsigned char *)&ip_be;
    wsprintfW(out, L"%d.%d.%d.%d", (int)p[0], (int)p[1], (int)p[2], (int)p[3]);
}

/* ── 带超时的 TCP 连接测试：非阻塞 connect + select ──
 * 1 = 连上, 0 = 拒绝或超时, -1 = socket 创建失败 */
static int tcp_test(unsigned long ip_be, int port, int timeout_ms)
{
    SOCKET s = cl_connect(ip_be, port, timeout_ms);
    if (s == INVALID_SOCKET)
        return 0;
    closesocket(s);
    return 1;
}

/* ── 探测主体 ── */
static void do_probe(void)
{
    WSADATA wsa;
    char host[128];
    struct hostent *he;
    WCHAR tmp[256];
    WCHAR ips[MAX_IP][24];
    unsigned long raw[MAX_IP];
    int nip = 0, i, j, hit = 0;
    unsigned long cmd_ip = 0;   /* 首个 CarLife 控制端口可达的手机地址 */

    static const int    ports[3] = { 7240, 8240, 5555 };
    static const WCHAR *pname[3] = { L"CarLife控制", L"CarLife视频", L"ADB" };
    static const unsigned char tail[3] = { 1, 129, 100 };
    static const unsigned char fixip[6][4] = {
        { 192, 168,  42, 129 }, { 192, 168,  42,   1 }, { 192, 168, 43,   1 },
        { 192, 168, 137,   1 }, { 192, 168,   0,   1 }, { 192, 168,  1,   1 }
    };

    g_len = 0;
    g_report[0] = 0;

    wsprintfW(tmp, L"屏幕 %dx%d", (int)GetSystemMetrics(SM_CXSCREEN),
              (int)GetSystemMetrics(SM_CYSCREEN));
    app(tmp);
    app(L"");

    /* ① Winsock 初始化 */
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        app(L"[X] WSAStartup 失败：ws2.dll 不可用");
        return;
    }
    app(L"[OK] WSAStartup 成功，ws2.dll 正常");

    /* ② 主机名 */
    host[0] = 0;
    if (gethostname(host, sizeof(host)) != 0)
        host[0] = 0;
    if (host[0]) {
        WCHAR wh[128];
        int k;
        for (k = 0; k < 127 && host[k]; k++)
            wh[k] = (WCHAR)(unsigned char)host[k];
        wh[k] = 0;
        wsprintfW(tmp, L"主机名 %s", wh);
        app(tmp);
    }

    /* ③ 本机 IP 列表 —— 判断「有没有网络」的关键 */
    he = host[0] ? gethostbyname(host) : NULL;
    if (he && he->h_addr_list) {
        for (i = 0; he->h_addr_list[i] && nip < MAX_IP; i++) {
            memcpy(&raw[nip], he->h_addr_list[i], 4);
            ip_str(raw[nip], ips[nip]);
            nip++;
        }
    }

    app(L"");
    if (nip == 0) {
        app(L"[X] 没拿到任何 IP");
        app(L"    车机没有网络 => M2a(IP 路径)不通");
        app(L"    需要转 M2b：自己实现 ADB host");
        return;
    }
    wsprintfW(tmp, L"[OK] 拿到 %d 个 IP 地址：", nip);
    app(tmp);
    for (i = 0; i < nip; i++) {
        wsprintfW(tmp, L"      %s", ips[i]);
        app(tmp);
    }

    /* ④ 组装候选手机地址并逐个探测 */
    app(L"");
    app(L"--- 探测手机端口 ---");
    {
        unsigned long cand[16];
        int nc = 0;

        /* (a) 同网段推导：本机第一个 IP 的前三字节 + 末字节 1/129/100 */
        if (nip > 0) {
            unsigned char b[4];
            for (j = 0; j < 3 && nc < 16; j++) {
                unsigned long c;
                memcpy(b, &raw[0], 4);
                b[3] = tail[j];
                memcpy(&c, b, 4);
                cand[nc++] = c;
            }
        }
        /* (b) 常见 USB 网络共享地址 */
        for (i = 0; i < 6 && nc < 16; i++) {
            unsigned long c;
            memcpy(&c, fixip[i], 4);
            cand[nc++] = c;
        }

        for (i = 0; i < nc; i++) {
            WCHAR s[24];
            int any = 0;
            ip_str(cand[i], s);
            if (cmd_ip == 0 && tcp_test(cand[i], CL_PORT_CMD, 450) == 1)
                cmd_ip = cand[i];
            for (j = 0; j < 3; j++) {
                if (tcp_test(cand[i], ports[j], 450) == 1) {
                    if (!any) {
                        wsprintfW(tmp, L"  %s :", s);
                        app(tmp);
                        any = 1;
                    }
                    wsprintfW(tmp, L"       端口 %d (%s) 通了", ports[j], pname[j]);
                    app(tmp);
                    hit++;
                }
            }
            if (!any) {
                wsprintfW(tmp, L"  %s : 无响应", s);
                app(tmp);
            }
        }
    }

    /* ⑤ 探测车机上有哪些解码器 DLL —— 决定 H.264 怎么解
     * 注意：TCC89X_VDEC/VDEC_785 这类是亿连自带的硬解 DLL，
     * 只有在「与本程序同一个目录」时 LoadLibrary 才找得到，
     * 所以务必把本程序放在车机上亿连的目录里运行。 */
    app(L"");
    app(L"--- 解码器 DLL 探测 ---");
    {
        static const WCHAR *dlls[9];
        int k;
        dlls[0] = L"TCC89X_VDEC.dll";
        dlls[1] = L"VDEC_785.dll";
        dlls[2] = L"VDEC_DCH60.dll";
        dlls[3] = L"vdec.dll";
        dlls[4] = L"h264dec.dll";
        dlls[5] = L"imgdecmp.dll";
        dlls[6] = L"ddraw.dll";
        dlls[7] = L"ws2.dll";
        dlls[8] = L"aygshell.dll";

        for (k = 0; k < 9; k++) {
            HMODULE hm = LoadLibraryW(dlls[k]);
            wsprintfW(tmp, L"  %s %s", hm ? L"[有]" : L"[无]", dlls[k]);
            app(tmp);
        }
    }

    /* ⑥ CarLife 协议握手 —— 决定性一步 */
    app(L"");
    app(L"--- CarLife 协议握手 ---");
    if (cmd_ip == 0) {
        app(L"  控制端口 7240 不可达，跳过握手");
    } else {
        SOCKET cs = cl_connect(cmd_ip, CL_PORT_CMD, 1500);
        if (cs == INVALID_SOCKET) {
            app(L"[X] 再连 7240 失败");
        } else {
            int ms = -1, r;
            unsigned long rid = 0;
            app(L"[OK] 已连上 7240，发送车机协议版本 1.0 ...");
            r = cl_handshake(cs, &ms, &rid);
            if (r == CL_OK) {
                wsprintfW(tmp, L"[OK] 手机回复 msgId=0x%x", rid);
                app(tmp);
                wsprintfW(tmp, L"      matchStatus=%d", ms);
                app(tmp);
                if (rid == CL_MSG_PROTOCOL_VERSION_MATCH)
                    app(L"*** 握手成功！手机认我们了 ***");
                else
                    app(L"收到回复但消息 ID 不是版本匹配，看上面");
            } else {
                wsprintfW(tmp, L"[X] 握手失败，错误码 %d", r);
                app(tmp);
                app(L"   (-3=连接被关,-4=等回复超时)");
            }
            closesocket(cs);
        }
    }

    app(L"");
    if (hit) {
        app(L"*** 有端口响应 => M2a 可行，可开始协议层 ***");
    } else {
        app(L"无端口响应。可能是：手机没开 USB 网络共享，");
        app(L"或 CarLife 未在该网卡上监听。");
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0, 0, 0));
        /* 只画当前页 —— 车机没有滚动条，靠点屏幕翻页 */
        DrawTextW(dc, uic_line_n(g_report, g_first), -1, &rc,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_KEYDOWN: {
        HDC dc = GetDC(h);
        RECT rc;
        int per;
        GetClientRect(h, &rc);
        per = uic_lines_per_page(dc, rc.bottom);
        ReleaseDC(h, dc);
        g_first += per;
        if (g_first >= g_total)
            DestroyWindow(h);          /* 看到底了再点才退出 */
        else
            InvalidateRect(h, 0, TRUE);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmd, int show)
{
    WNDCLASSW wc;
    MSG msg;
    int W, H;

    (void)hp; (void)cmd; (void)show;

    W = (int)GetSystemMetrics(SM_CXSCREEN);
    H = (int)GetSystemMetrics(SM_CYSCREEN);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hi;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"NetProbeWnd";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, L"NetProbeWnd", L"CarLife 连接探测",
                             WS_VISIBLE, 0, 0, W, H, 0, 0, hi, 0);
    if (!g_hwnd)
        return 1;
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    do_probe();                        /* 窗口已显示，探测期间可看到空窗 */
    g_total = uic_count_lines(g_report);
    uic_dump_file(L"netprobe-result.txt", g_report);   /* 结果写文件，便于拷出来发给开发方 */
    InvalidateRect(g_hwnd, 0, TRUE);   /* 结果画上去 */

    while (GetMessageW(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    WSACleanup();
    return 0;
}
