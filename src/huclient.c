/* huclient.c — WinCE 车机端 CarLife 客户端（第一版）
 *
 * 目标：打通到「确实收到手机推来的视频帧」为止。
 * 本版不解码显示（H.264 解码是下一步），先把链路证明出来。
 *
 * 顺序严格照参考实现 CConnectionSetupModule::connectionSetup()：
 *   先把 socket 建齐，再跑协议握手，而不是边握手边建连接。
 *
 *   1. 找手机（试连 7240）
 *   2. 把视频通道 8240 也连上
 *   3. CMD 通道握手：发车机协议版本 1.0
 *   4. CMD 通道：视频编码初始化 768x480@30
 *   5. CMD 通道：开始推流
 *   6. VIDEO 通道：收帧，统计帧数与字节数
 *
 * UI：单个真窗口自绘。绝不用 MessageBox（车机只显示第一个弹窗）。
 */

#include <winsock2.h>
#include <windows.h>
#include <string.h>
#include "carlife.h"
#include "uicommon.h"

#define REPORT_CAP 8192
#define MAX_FRAMES 40
#define FRAME_TIME 10000UL
#define SHOW_FRAMES 8

static WCHAR g_report[REPORT_CAP];
static int   g_len = 0;
static HWND  g_hwnd = 0;
static int   g_first = 0;   /* 当前页从第几行开始 */
static int   g_total = 0;

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

static void ip_str(unsigned long ip_be, WCHAR *out)
{
    const unsigned char *p = (const unsigned char *)&ip_be;
    wsprintfW(out, L"%d.%d.%d.%d", (int)p[0], (int)p[1], (int)p[2], (int)p[3]);
}

static void do_run(void)
{
    WSADATA wsa;
    WCHAR tmp[256], ips[24];
    unsigned long cand[16];
    unsigned long phone = 0;
    SOCKET cmd = INVALID_SOCKET;
    SOCKET vid = INVALID_SOCKET;
    int n, i, found = 0;

    /* 256KB：H.264 的 I 帧在 768x480 下可能到 100KB 左右，
     * 64KB 会频繁触发「帧比缓冲区大」的丢数据路径。 */
    static unsigned char vbuf[262144];

    g_len = 0;
    g_report[0] = 0;

    wsprintfW(tmp, L"屏幕 %dx%d", (int)GetSystemMetrics(SM_CXSCREEN),
              (int)GetSystemMetrics(SM_CYSCREEN));
    app(tmp);
    app(L"");

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        app(L"[X] WSAStartup 失败，ws2.dll 不可用");
        return;
    }
    app(L"[OK] ws2 初始化成功");

    /* ── 1. 找手机 ── */
    n = cl_candidate_ips(cand, 16);
    wsprintfW(tmp, L"候选地址 %d 个，试连 7240 ...", n);
    app(tmp);

    for (i = 0; i < n; i++) {
        cmd = cl_connect(cand[i], CL_PORT_CMD, 500);
        if (cmd != INVALID_SOCKET) {
            phone = cand[i];
            ip_str(phone, ips);
            wsprintfW(tmp, L"[OK] 找到 %s:7240", ips);
            app(tmp);
            found = 1;
            break;
        }
    }
    if (!found) {
        app(L"[X] 没找到 CarLife 服务");
        app(L"    检查：手机是否开了 USB 网络共享、");
        app(L"    Jovi InCar 是否在连接等待状态。");
        return;
    }

    /* ── 2. 先把视频通道也连上（照参考实现：socket 先建齐）── */
    vid = cl_connect(phone, CL_PORT_VIDEO, 2000);
    if (vid == INVALID_SOCKET)
        app(L"[!] 视频通道 8240 连不上，仍继续试协议");

    /* ── 3. 握手 ── */
    app(L"");
    app(L"[3/6] 发车机协议版本 1.0 ...");
    {
        int ms = -1, r;
        unsigned long rid = 0;
        r = cl_handshake(cmd, &ms, &rid);
        if (r != CL_OK) {
            wsprintfW(tmp, L"[X] 握手失败，错误码 %d  (-3=被关 -4=超时)", r);
            app(tmp);
            closesocket(cmd);
            if (vid != INVALID_SOCKET)
                closesocket(vid);
            return;
        }
        wsprintfW(tmp, L"[OK] 手机回复 msgId=0x%x matchStatus=%d", rid, ms);
        app(tmp);
        if (rid != CL_MSG_PROTOCOL_VERSION_MATCH)
            app(L"     注意：ID 不是版本匹配，但继续");
    }

    /* ── 4. 视频编码初始化 ── */
    app(L"");
    app(L"[4/6] 发视频编码初始化 768x480@30 ...");
    {
        int r;
        unsigned long rid = 0;
        unsigned char rb[512];
        int blen = 0;

        r = cl_send_video_encoder_init(cmd, CL_VIDEO_W, CL_VIDEO_H, CL_VIDEO_FPS);
        if (r != CL_OK) {
            wsprintfW(tmp, L"[X] 发送失败 %d", r);
            app(tmp);
        } else {
            r = cl_recv_cmd(cmd, &rid, rb, (int)sizeof(rb), &blen, 4000);
            if (r == CL_OK) {
                wsprintfW(tmp, L"[OK] 手机回复 msgId=0x%x 体长=%d", rid, blen);
                app(tmp);
                if (rid == CL_MSG_VIDEO_ENCODER_INIT_DONE) {
                    /* 手机回传实际采用的分辨率/帧率：宽=1 高=2 帧率=3，均为 varint */
                    int w = -1, hh = -1, fr = -1, k = 0;
                    app(L"     = 编码器已就绪");
                    while (k + 1 < blen) {
                        int field = (rb[k] >> 3) & 0x1f;
                        int val = 0, shift = 0;
                        k++;
                        while (k < blen && (rb[k] & 0x80)) {
                            val |= (rb[k] & 0x7f) << shift;
                            shift += 7;
                            k++;
                        }
                        if (k < blen) {
                            val |= (rb[k] & 0x7f) << shift;
                            k++;
                        }
                        if (field == 1) w = val;
                        else if (field == 2) hh = val;
                        else if (field == 3) fr = val;
                    }
                    wsprintfW(tmp, L"     手机采用 %dx%d @%d帧", w, hh, fr);
                    app(tmp);
                }
            } else {
                wsprintfW(tmp, L"[!] 没等到回复（%d），继续", r);
                app(tmp);
            }
        }
    }

    /* ── 5. 开始推流 ── */
    app(L"");
    app(L"[5/6] 发视频开始 ...");
    {
        int r = cl_send_video_encoder_start(cmd);
        wsprintfW(tmp, L"     结果 %d", r);
        app(tmp);
    }

    /* ── 6. 收帧 ── */
    app(L"");
    app(L"[6/6] 收视频帧 ...");
    if (vid == INVALID_SOCKET) {
        app(L"[X] 视频通道没连上，收不到帧");
    } else {
        int frames = 0, total = 0, r;
        unsigned long t0 = GetTickCount();

        while (frames < MAX_FRAMES) {
            unsigned long ts = 0, vt = 0;
            int len = 0;

            if (GetTickCount() - t0 > FRAME_TIME)
                break;

            r = cl_recv_video(vid, &ts, &vt, vbuf, (int)sizeof(vbuf), &len, 3000);
            if (r != CL_OK) {
                wsprintfW(tmp, L"[!] 收帧中断，错误码 %d", r);
                app(tmp);
                break;
            }
            frames++;
            total += len;
            if (frames == 1) {
                int k, lim = len < 12 ? len : 12;
                WCHAR hex[64];
                hex[0] = 0;
                for (k = 0; k < lim; k++)
                    wsprintfW(hex + wcslen(hex), L"%02x ", (int)vbuf[k]);
                wsprintfW(tmp, L"   首帧头部: %s", hex);
                app(tmp);
                wsprintfW(tmp, L"   格式判断: %s", cl_guess_codec(vbuf, len));
                app(tmp);
            }
            if (frames <= SHOW_FRAMES) {
                wsprintfW(tmp, L"   帧%d: %d 字节 type=0x%x", frames, len, vt);
                app(tmp);
            }
        }
        wsprintfW(tmp, L"[OK] 共 %d 帧，合计 %d 字节", frames, total);
        app(tmp);
        if (frames > 0) {
            app(L"");
            app(L"*** 链路打通！手机确实在推视频 ***");
            wsprintfW(tmp, L"    平均每帧约 %d 字节", total / frames);
            app(tmp);
        } else {
            app(L"");
            app(L"没收到帧。可能需要先确认协议版本匹配结果。");
        }
    }

    if (cmd != INVALID_SOCKET)
        closesocket(cmd);
    if (vid != INVALID_SOCKET)
        closesocket(vid);
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

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmdline, int show)
{
    WNDCLASSW wc;
    MSG msg;
    int W, H;

    (void)hp; (void)cmdline; (void)show;

    W = (int)GetSystemMetrics(SM_CXSCREEN);
    H = (int)GetSystemMetrics(SM_CYSCREEN);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hi;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"HUClientWnd";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, L"HUClientWnd", L"CarLife 车机客户端",
                             WS_VISIBLE, 0, 0, W, H, 0, 0, hi, 0);
    if (!g_hwnd)
        return 1;
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    do_run();
    g_total = uic_count_lines(g_report);
    uic_dump_file(L"client-result.txt", g_report);   /* 结果写文件，便于拷出来发给开发方 */
    InvalidateRect(g_hwnd, 0, TRUE);

    while (GetMessageW(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    WSACleanup();
    return 0;
}
