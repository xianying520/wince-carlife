/* viewer.c — 车机端最终程序：手机画面显示 + 触屏回传
 *
 * 链路：连手机 → 协议版本握手 → 视频初始化 → 请求 JPEG → 收帧 →
 *       nanojpeg 解码 → StretchDIBits 显示 → 触摸按比例换算回传
 *
 * 为什么请求 JPEG 而不是 H.264：
 *   已经集成并验证过 nanojpeg（MIT，919 行），车机软解 JPEG 现实可行；
 *   H.264 软解在老 ARM 上很可能跑不动。若手机不理会 JPEG 请求仍发 H.264，
 *   我们会检测出来并在状态栏明说，不会假装在显示。
 */
#include <windows.h>
#include <winsock2.h>

#include "carlife.h"
#include "display.h"
#include "adbproxy.h"
#include "third_party/nanojpeg.h"

#define STATUS_H   22          /* 底部状态栏高度 */
#define EXIT_W     58          /* 状态栏右侧「退出」按钮宽度 */
#define RES_W      96          /* 状态栏「分辨率」切换按钮宽度 */
#define TOG_W      56          /* 状态栏「触摸包头」切换按钮宽度 */
#define RX_CAP     (512 * 1024)

static HWND     g_hwnd = 0;
static DISP     g_disp;
static SOCKET   g_cmd = INVALID_SOCKET;
static SOCKET   g_vid = INVALID_SOCKET;
static SOCKET   g_touch = INVALID_SOCKET;
/* 触摸写法：0 = 专用消息 + SinglePoint（参考实现在用的）
 *           1 = 通用消息 + TouchAction（参考实现里被注释掉的）
 * 两套都实现是因为从参考源码分不出手机接受哪一套，现场点按钮试即可。
 * 包头长度不再是悬念 —— 参考源码 CTRL_HEAD_LEN 8 且注释写明
 * "ctrol channel [HU->MD]"，已实证。 */
static int      g_touch_mode = 0;

/* ── 请求的画面参数预设 ──
 * 为什么要有这个：老 ARM 上软解 JPEG 的能力大致按「每秒像素数」衡量。
 * 768x480@30 = 11.0M 像素/秒，基本不可能；480x272@15 = 2.0M 才现实。
 * 但车机实际能力未知，所以做成按钮现场逐个试，不用重新编译。
 * 默认选 480x272@15（"先能看"），确认流畅后再往上调。 */
typedef struct { int w, h, fps; const WCHAR *name; } PRESET;
static const PRESET g_presets[] = {
    { 320, 240, 10, L"320x240@10" },
    { 400, 240, 12, L"400x240@12" },
    { 480, 272, 15, L"480x272@15" },
    { 640, 360, 20, L"640x360@20" },
    { 768, 480, 30, L"768x480@30" },
};
#define N_PRESETS ((int)(sizeof(g_presets) / sizeof(g_presets[0])))
static int      g_preset = 2;               /* 默认 480x272@15 */
static int      g_quit = 0;
static int      g_captured = 0;
/* 最后一次有效的画面坐标。见 WM_LBUTTONUP 里的说明：手指拖到画面外再
 * 松开时，必须用这个坐标把"抬起"补发出去，否则手机以为手指一直按着。 */
static int      g_last_px = 0, g_last_py = 0, g_have_last = 0;

static unsigned char *g_rx = 0;
static int      g_rxcap = 0;

static WCHAR    g_status[256] = L"正在连接手机 ...";
static int      g_frames = 0, g_decoded = 0, g_shown = 0;
static unsigned long g_t0 = 0;
static int      g_fps = 0;
static int      g_miss = 0;                /* 连续收到非 JPEG 帧的次数 */

/* 把"不是 JPEG"的帧存到 U 盘上，让用户带回来。
 * 为什么必须做：手机推的到底是 JPEG 还是 H.264，决定我们要不要移植一个
 * H.264 解码器 —— 这是整个项目剩下的最大未知数。而只要头几个字节就能
 * 确定（H.264 是 00 00 00 01 + NAL 头）。光看屏幕看不出来，必须存成文件。
 * 文件名和《现场测试说明》里承诺的一致，别改。 */
#define DUMP_CAP (32 * 1024)
static DWORD    g_dumped = 0;

/* ── 传输路线 ──
 * 优先走 ADB 端口转发（车机做 ADB 主机 → 手机端口搬到本机 127.0.0.1）。
 * 这条路不需要 USB 网卡，是 CarLife 有线模式的标准架构。
 * 万一车机没有 ADB 驱动，再退回 USB 网络共享（手机开共享 → 车机拿 IP）。 */
static unsigned long    g_ip         = 0;
static unsigned short   g_port_cmd   = CL_PORT_CMD;
static unsigned short   g_port_vid   = CL_PORT_VIDEO;
static unsigned short   g_port_touch = CL_PORT_TOUCH;
static int              g_route      = 0;   /* 0 未定 / 1 ADB转发 / 2 USB网卡 */
static int              g_adb_ok     = 0;
static char             g_adb_reason[256] = "";

/* ── 状态栏 ── */
static void set_status(const WCHAR *s)
{
    int i;
    for (i = 0; i < 250 && s[i]; i++)
        g_status[i] = s[i];
    g_status[i] = 0;
    if (g_hwnd)
        InvalidateRect(g_hwnd, 0, FALSE);
}

/* ── 把一帧画出来 ── */
static void dump_unknown_frame(const unsigned char *buf, int len)
{
    HANDLE h;
    DWORD  bw = 0;
    int    n;

    if (g_dumped >= DUMP_CAP || len <= 0)
        return;
    n = len;
    if ((DWORD)n > DUMP_CAP - g_dumped)
        n = (int)(DUMP_CAP - g_dumped);

    /* 第一帧用 CREATE_ALWAYS（覆盖上次的旧文件），之后追加 */
    h = CreateFileW(L"video-dump.bin",
                    (g_dumped == 0) ? GENERIC_WRITE : FILE_APPEND_DATA,
                    0, NULL,
                    (g_dumped == 0) ? CREATE_ALWAYS : OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, buf, (DWORD)n, &bw, NULL);
    CloseHandle(h);
    g_dumped += bw;
}

static void show_frame(const unsigned char *buf, int len)
{
    int r;

    if (len >= 2 && buf[0] == 0xFF && buf[1] == 0xD8) {
        /* JPEG */
        njInit();
        r = njDecode(buf, len);
        if (r != NJ_OK) {
            njDone();
            g_miss++;
            return;
        }
        if (disp_init(&g_disp, njGetWidth(), njGetHeight()) != 0) {
            njDone();
            return;
        }
        if (njIsColor())
            disp_set_rgb24(&g_disp, njGetImage(), njGetWidth(), njGetHeight());
        else
            disp_set_gray8(&g_disp, njGetImage(), njGetWidth(), njGetHeight());
        njDone();
        g_decoded++;
        g_shown++;
        g_miss = 0;
        if (g_hwnd)
            InvalidateRect(g_hwnd, 0, FALSE);
    } else {
        /* 不是 JPEG —— 极可能是 H.264。不装作在显示，直接说明。 */
        dump_unknown_frame(buf, len);
        g_miss++;
        if (g_miss == 3) {
            const WCHAR *what = cl_guess_codec(buf, len);
            WCHAR t[240];
            wsprintfW(t, L"手机推的是 %s，不是 JPEG（已存 video-dump.bin，请带回）",
                      what);
            set_status(t);
        }
    }
}

/* ── 触摸回传 ── */
static void send_touch(int action, int x, int y)
{
    if (g_touch == INVALID_SOCKET)
        return;
    cl_send_touch_action(g_touch, action, x, y, g_touch_mode);
}

/* 按当前预设重新初始化视频编码器。
 * 序列 RESET → INIT → START → JPEG 是推断：RESET 的消息存在（0x0001800B）
 * 但未见参考代码演示中途改参数的完整时序，所以这里算尽力而为，
 * 失败了也不致命 —— 用户重启程序即按新预设重新走一遍。 */
static void apply_preset(void)
{
    if (g_cmd == INVALID_SOCKET)
        return;
    cl_send_cmd(g_cmd, CL_MSG_VIDEO_ENCODER_RESET, 0, 0);
    cl_send_video_encoder_init(g_cmd, g_presets[g_preset].w,
                               g_presets[g_preset].h, g_presets[g_preset].fps);
    cl_send_video_encoder_start(g_cmd);
    cl_send_video_encoder_jpeg(g_cmd);
}

/* 预设记住到文件，重启程序也能沿用上次的选择 */
static void save_preset(void)
{
    HANDLE h = CreateFileW(L"viewer-res.txt", GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD bw = 0;
    char txt[16];
    int n = 0, v = g_preset;
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (v == 0) txt[n++] = '0';
    while (v > 0) { txt[n++] = (char)('0' + v % 10); v /= 10; }
    /* 倒序 */
    { int i; for (i = 0; i < n / 2; i++) { char t = txt[i]; txt[i] = txt[n-1-i]; txt[n-1-i] = t; } }
    txt[n++] = '\n';
    WriteFile(h, txt, (DWORD)n, &bw, NULL);
    CloseHandle(h);
}

static void load_preset(void)
{
    HANDLE h = CreateFileW(L"viewer-res.txt", GENERIC_READ, 0, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    char b[8];
    DWORD br = 0;
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (ReadFile(h, b, sizeof(b), &br, NULL) && br > 0) {
        int v = 0, i;
        for (i = 0; i < (int)br && b[i] >= '0' && b[i] <= '9'; i++)
            v = v * 10 + (b[i] - '0');
        if (v >= 0 && v < N_PRESETS)
            g_preset = v;
    }
    CloseHandle(h);
}

/* ── 窗口过程 ── */
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc, area;
        GetClientRect(h, &rc);

        area = rc;
        area.bottom -= STATUS_H;

        if (g_disp.fb) {
            disp_paint(&g_disp, dc, &area);
        } else {
            RECT f = area;
            FillRect(dc, &f, (HBRUSH)GetStockObject(WHITE_BRUSH));
        }

        /* 状态栏 */
        {
            RECT bar = rc;
            HBRUSH br;
            bar.top = rc.bottom - STATUS_H;
            br = CreateSolidBrush(RGB(32, 32, 32));
            FillRect(dc, &bar, br);
            DeleteObject(br);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(230, 230, 230));
            bar.left += 6;
            DrawTextW(dc, g_status, -1, &bar,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            /* 触摸写法切换按钮：
             * 参考实现里触摸有两套写法，但只能看出其中一套在被使用
             * （另一套在示例代码里是注释状态）。从源码分不出手机接受哪套，
             * 做成按钮现场直接试，不用重新编译。 */
            {
                RECT tg = rc;
                HBRUSH tb = CreateSolidBrush(g_touch_mode == 0
                                             ? RGB(40, 90, 40) : RGB(120, 90, 20));
                WCHAR lbl[24];
                tg.top = rc.bottom - STATUS_H;
                tg.right = rc.right - EXIT_W;
                tg.left = tg.right - TOG_W;
                FillRect(dc, &tg, tb);
                DeleteObject(tb);
                SetTextColor(dc, RGB(255, 255, 255));
                wsprintfW(lbl, L"触摸%s", g_touch_mode == 0 ? L"A" : L"B");
                DrawTextW(dc, lbl, -1, &tg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            /* 分辨率预设按钮 */
            {
                RECT rb = rc;
                HBRUSH bb = CreateSolidBrush(RGB(40, 60, 110));
                rb.top = rc.bottom - STATUS_H;
                rb.right = rc.right - EXIT_W - TOG_W;
                rb.left = rb.right - RES_W;
                FillRect(dc, &rb, bb);
                DeleteObject(bb);
                SetTextColor(dc, RGB(255, 255, 255));
                DrawTextW(dc, g_presets[g_preset].name, -1, &rb,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            /* 退出按钮 */
            {
                RECT ex = rc;
                HBRUSH eb = CreateSolidBrush(RGB(150, 40, 40));
                ex.top = rc.bottom - STATUS_H;
                ex.left = rc.right - EXIT_W;
                FillRect(dc, &ex, eb);
                DeleteObject(eb);
                SetTextColor(dc, RGB(255, 255, 255));
                DrawTextW(dc, L"退出", -1, &ex,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        EndPaint(h, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        RECT rc, area;
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        int px, py;
        GetClientRect(h, &rc);

        /* 右下角退出按钮 */
        if (x >= rc.right - EXIT_W && y >= rc.bottom - STATUS_H) {
            g_quit = 1;
            DestroyWindow(h);
            return 0;
        }
        /* 分辨率预设切换 */
        if (x >= rc.right - EXIT_W - TOG_W - RES_W && x < rc.right - EXIT_W - TOG_W
            && y >= rc.bottom - STATUS_H) {
            g_preset = (g_preset + 1) % N_PRESETS;
            save_preset();
            apply_preset();
            {
                WCHAR t[200];
                wsprintfW(t, L"已切换到 %s", g_presets[g_preset].name);
                set_status(t);
            }
            InvalidateRect(h, 0, FALSE);
            return 0;
        }
        /* 触摸写法切换 */
        if (x >= rc.right - EXIT_W - TOG_W && x < rc.right - EXIT_W
            && y >= rc.bottom - STATUS_H) {
            g_touch_mode = (g_touch_mode == 0) ? 1 : 0;
            set_status(g_touch_mode == 0
                       ? L"触摸写法 A：专用消息 + 单点坐标（参考实现在用）"
                       : L"触摸写法 B：通用消息 + 动作+坐标（参考实现里注释掉的）");
            InvalidateRect(h, 0, FALSE);
            return 0;
        }

        area = rc;
        area.bottom -= STATUS_H;
        if (disp_map_touch(&g_disp, &area, x, y, &px, &py) == 0) {
            SetCapture(h);
            g_captured = 1;
            g_last_px = px;
            g_last_py = py;
            g_have_last = 1;
            send_touch(0, px, py);            /* 0 = 按下 */
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        RECT rc, area;
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        int px, py;
        if (!g_captured)
            return 0;
        GetClientRect(h, &rc);
        area = rc;
        area.bottom -= STATUS_H;
        if (disp_map_touch(&g_disp, &area, x, y, &px, &py) == 0) {
            g_last_px = px;
            g_last_py = py;
            send_touch(2, px, py);            /* 2 = 移动 */
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        RECT rc, area;
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        int px, py;
        if (!g_captured)
            return 0;
        g_captured = 0;
        ReleaseCapture();
        GetClientRect(h, &rc);
        area = rc;
        area.bottom -= STATUS_H;
        /* ⚠ "抬起"必须无条件发出去。
         *
         *   原来这里和按下一样做了边界判断：手指拖到画面外再松开就【不发
         *   抬起】—— 而在触摸屏上这太容易了（从底部往上滑、松手时落在状态
         *   栏上）。手机那边会以为你的手指一直按着没放，之后所有操作都不对。
         *   落点在外面时，就用最后一个有效坐标把抬起补上。 */
        if (disp_map_touch(&g_disp, &area, x, y, &px, &py) == 0) {
            g_last_px = px;
            g_last_py = py;
        } else if (!g_have_last) {
            return 0;                        /* 从头到尾就没落到画面里，忽略 */
        }
        send_touch(1, g_last_px, g_last_py);  /* 1 = 抬起 */
        g_have_last = 0;
        return 0;
    }

    case WM_KEYDOWN:
        if (w == VK_ESCAPE || w == VK_BACK) {
            g_quit = 1;
            DestroyWindow(h);
        }
        return 0;

    case WM_CLOSE:
        g_quit = 1;
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ── 连接手机（找地址 + 握手）── */
/* 决定走哪条路。
 * 返回 0 表示已确定 g_ip 与三个端口；返回 -1 表示两条路都不通。 */
static int open_transport(void)
{
    static const char *svc[3] = { "tcp:7240", "tcp:8240", "tcp:9340" };
    unsigned short ports[3];
    char reason[256];

    set_status(L"正在尝试 ADB 直连（USB）...");

    if (adbp_start(svc, 3, ports, reason, (int)sizeof(reason)) == 0) {
        g_ip         = htonl(0x7F000001UL);      /* 连本机，端口已被转到手机上 */
        g_port_cmd   = ports[0];
        g_port_vid   = ports[1];
        g_port_touch = ports[2];
        g_route      = 1;
        g_adb_ok     = 1;
        return 0;
    }

    /* ADB 不通 —— 把原因留着，一会儿显示给用户看，这是现场排查的关键信息。
     * ⚠ 这里不能用 lstrcpynA：本工具链的 coredll 里只有 Unicode 版的
     *   lstrcpynW，链接时会报 undefined reference。手写循环最稳妥。 */
    {
        int i;
        for (i = 0; i < (int)sizeof(g_adb_reason) - 1 && reason[i]; i++)
            g_adb_reason[i] = reason[i];
        g_adb_reason[i] = 0;
    }

    /* 退回 USB 网络共享：手机开共享后车机会拿到 IP，扫常见网段 */
    {
        unsigned long ips[16];
        int n = cl_candidate_ips(ips, 16), i;
        for (i = 0; i < n; i++) {
            SOCKET s = cl_connect(ips[i], CL_PORT_CMD, 1200);
            if (s != INVALID_SOCKET) {
                closesocket(s);
                g_ip = ips[i];
                g_route = 2;
                return 0;
            }
        }
    }
    return -1;
}

static SOCKET connect_cmd(void)
{
    return cl_connect(g_ip, (int)g_port_cmd, 2000);
}

static void run_session(void)
{
    SOCKET cmd;
    int st;
    unsigned long t0 = 0;

    if (open_transport() != 0) {
        WCHAR t[400];
        WCHAR wreason[256];
        int i;
        for (i = 0; i < 255 && g_adb_reason[i]; i++)
            wreason[i] = (WCHAR)(unsigned char)g_adb_reason[i];
        wreason[i] = 0;
        wsprintfW(t, L"两条路都不通。ADB: %s ／ USB网卡: 也没找到手机 "
                      L"（请确认已开 USB 调试或 USB 网络共享，且 Jovi InCar 已启动）",
                  wreason);
        set_status(t);
        return;
    }

    cmd = connect_cmd();
    if (cmd == INVALID_SOCKET) {
        set_status(g_route == 1
                   ? L"ADB 转发已就绪，但本机端口连不上（转发线程可能已退出）"
                   : L"没找到手机（7240 端口都不通）");
        return;
    }
    set_status(g_route == 1
               ? L"已通过 ADB 直连手机，正在握手 ..."
               : L"已通过 USB 网络共享连上手机，正在握手 ...");

    {
        int match = -1;
        unsigned long reply = 0;
        st = cl_handshake(cmd, &match, &reply);
        if (st != CL_OK) {
            WCHAR t[200];
            wsprintfW(t, L"握手失败（%d）—— 手机没回应协议版本", st);
            set_status(t);
            return;
        }
        if (match >= 0) {
            WCHAR t[200];
            wsprintfW(t, L"握手成功，手机认可协议版本（状态 %d）", match);
            set_status(t);
        }
    }
    g_cmd = cmd;

    g_vid = cl_connect(g_ip, (int)g_port_vid, 2000);
    if (g_vid == INVALID_SOCKET) {
        set_status(L"视频通道 8240 连不上");
        return;
    }

    /* 参考实现要求：每个通道使用前都要重发一次协议版本。 */
    cl_resend_version(cmd);

    /* 这三个都走 CMD 通道（已由参考源码证实：
     * 它们是 CCmdChannelModule 的成员方法，不是视频通道模块的），
     * 所以只需要传控制 socket。 */
    load_preset();
    if (cl_send_video_encoder_init(cmd, g_presets[g_preset].w,
                                   g_presets[g_preset].h,
                                   g_presets[g_preset].fps) != CL_OK) {
        set_status(L"VIDEO_ENCODER_INIT 发送失败");
        return;
    }
    if (cl_send_video_encoder_start(cmd) != CL_OK) {
        set_status(L"VIDEO_ENCODER_START 发送失败");
        return;
    }
    /* 明确要求 JPEG —— 我们只有 JPEG 解码器 */
    cl_send_video_encoder_jpeg(cmd);
    {
        WCHAR t[200];
        wsprintfW(t, L"已请求 %s，等画面中 ...", g_presets[g_preset].name);
        set_status(t);
    }

    g_touch = cl_connect(g_ip, (int)g_port_touch, 1500);
    if (g_touch != INVALID_SOCKET)
        cl_resend_version(cmd);       /* 触摸通道使用前也要求重发 */

    t0 = GetTickCount();
    g_t0 = t0;

    for (;;) {
        MSG msg;
        unsigned long ts = 0, vt = 0;
        int len = 0, r;

        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit)
            return;

        r = cl_recv_video(g_vid, &ts, &vt, g_rx, g_rxcap, &len, 20);
        if (r == CL_OK) {
            g_frames++;
            show_frame(g_rx, len);
            if (g_frames % 5 == 0) {
                unsigned long el = GetTickCount() - g_t0;
                WCHAR t[256];
                if (el >= 1000) {
                    g_fps = (int)((long)g_frames * 1000 / (long)el);
                    wsprintfW(t, L"%d 帧  解码 %d  显示 %d  约 %d 帧/秒",
                              g_frames, g_decoded, g_shown, g_fps);
                    set_status(t);
                }
            }
        } else if (r == CL_ERR_TIMEOUT) {
            continue;                      /* 正常：暂时没数据 */
        } else {
            WCHAR t[200];
            wsprintfW(t, L"收帧中断（错误 %d），已收 %d 帧", r, g_frames);
            set_status(t);
            return;
        }
    }
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmdline, int show)
{
    WNDCLASSW wc;
    WSADATA wsa;
    RECT rc;
    MSG msg;

    (void)hp; (void)cmdline; (void)show;

    g_rxcap = RX_CAP;
    g_rx = (unsigned char *)malloc(g_rxcap);
    if (!g_rx)
        return 1;

    memset(&g_disp, 0, sizeof(g_disp));

    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.cbClsExtra    = 0;
    wc.cbWndExtra    = 0;
    wc.hInstance     = hi;
    wc.hIcon         = 0;
    wc.hCursor       = 0;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszMenuName  = 0;
    wc.lpszClassName = L"CarLifeView";
    RegisterClassW(&wc);

    rc.left = 0; rc.top = 0;
    rc.right = GetSystemMetrics(SM_CXSCREEN);
    rc.bottom = GetSystemMetrics(SM_CYSCREEN);

    g_hwnd = CreateWindowExW(0, L"CarLifeView", L"CarLife 车机端",
                             WS_POPUP | WS_VISIBLE,
                             0, 0, rc.right, rc.bottom,
                             0, 0, hi, 0);
    if (!g_hwnd)
        return 1;

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        set_status(L"WSAStartup 失败：车机没有 ws2 网络栈");
    } else {
        run_session();
        WSACleanup();
    }

    /* 留在窗口里，让用户看清最后的状态 */
    while (!g_quit && GetMessageW(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_adb_ok) adbp_stop();          /* 关掉转发线程并释放 ADB 设备 */
    if (g_cmd   != INVALID_SOCKET) closesocket(g_cmd);
    if (g_vid   != INVALID_SOCKET) closesocket(g_vid);
    if (g_touch != INVALID_SOCKET) closesocket(g_touch);
    disp_free(&g_disp);
    if (g_rx) free(g_rx);
    return 0;
}
