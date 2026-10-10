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
#include <string.h>

#include "carlife.h"
#include "display.h"
#include "adbproxy.h"
#include "adbio_ce.h"
#include "third_party/nanojpeg.h"
#include "cllog.h"
#include "ui.h"
#ifdef HAS_H264
#include "h264dec.h"
#endif

/* 把一整行原样写进日志。
 * ⚠ 必须走 "%s"，不能把这一行直接当格式串：从注册表读出来的驱动名/设备名
 *   万一含一个 % 就会把 cl_log 的格式化带跑偏。 */
static void cl_log_line(const char *line)
{
    cl_log("%s", line ? line : "");
}

/* UTF-8 → UTF-16（WinCE 的宽字符就是 UTF-16）。
 *
 * ⚠⚠ 这个函数原来写的是「一个字节换一个宽字符」，也就是它根本不认识 UTF-8。
 *   而本项目所有从日志/RSA/协议层来的字符串【全是 UTF-8】——
 *   于是一个 3 字节的汉字被拆成了 3 个互不相干的宽字符。
 *   现场看到的正是这个：
 *     「打不开任何 ADB 设备，试过：ADB1:(错误55) …」
 *   在屏幕上一整行全是看不懂的怪字符，那一串设备名等于白记了。
 *
 *   现在按标准 UTF-8 逐字节解码：
 *     0xxxxxxx                              1 字节
 *     110xxxxx 10xxxxxx                     2 字节
 *     1110xxxx 10xxxxxx 10xxxxxx            3 字节（汉字都走这条）
 *     11110xxx 10xxxxxx 10xxxxxx 10xxxxxx   4 字节（转成 UTF-16 代理对）
 *   解不出来的字节换成 '?' —— 宁可少一个字，也不要整行花掉。 */
static void a2w_ui(const char *a, WCHAR *w, int cap)
{
    int i = 0, o = 0;

    if (!a || !w || cap <= 0) return;

    while (a[i] && o < cap - 1) {
        unsigned char c = (unsigned char)a[i];
        unsigned int  cp;
        int           need, k;

        if (c < 0x80)                { cp = c;        need = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; need = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; need = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; need = 3; }
        else                         { w[o++] = L'?'; i++; continue; }

        for (k = 1; k <= need; k++) {
            unsigned char d = (unsigned char)a[i + k];
            if ((d & 0xC0) != 0x80) break;
            cp = (cp << 6) | (unsigned int)(d & 0x3Fu);
        }
        if (k <= need) { w[o++] = L'?'; i++; continue; }
        i += need + 1;

        if (cp >= 0x10000u) {
            if (o + 2 > cap - 1) break;
            cp -= 0x10000u;
            w[o++] = (WCHAR)(0xD800u + (cp >> 10));
            w[o++] = (WCHAR)(0xDC00u + (cp & 0x3FFu));
        } else {
            w[o++] = (WCHAR)cp;
        }
    }
    w[o] = 0;
}

/* 底部栏的高度、按钮宽度都已归 ui.c 统一管理（见 ui_layout.h）。
 * 这里原来的一套常量已删 —— 两处各写一套必然走样。 */
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

/* CMD 通道的接收缓冲。
 * ⚠ 以前主循环【只收视频通道】，CMD 上手机说什么我们一个字都看不见。
 *   万一它发了一条要我们回的报文（比如认证、错误码、息屏通知），
 *   就等于把天聊死了 —— 而现场只会看到「一直收不到画面」，查不出原因。 */
static unsigned char g_cmd_rx[1024];
static int      g_cmd_msgs = 0;

/* 等手机的总秒数。用户可能是先开程序再插线，所以必须等得住。 */
#define WAIT_SECS  120

static WCHAR    g_status[256] = L"正在连接手机 ...";
static int      g_frames = 0, g_decoded = 0, g_shown = 0;
static unsigned long g_t0 = 0;
static int      g_fps = 0;
static int      g_miss = 0;                /* 连续收到非 JPEG/解码失败的次数 */

/* ⚠ 这两个必须放在 #ifdef HAS_H264 【外面】。
 *   原因：show_frame 与 session_thread 在【不带 H.264】的那份构建里也要编，
 *   而它们都会用到这两个变量。放进 #ifdef 里的话，不带 H.264 的构建会报
 *   undeclared —— 实锤：CI 的 2 号工作流（不带）编不过，4 号（带）却没事。
 *   ⚠ 注意 scripts/check_order.py 【不剥注释】，所以注释里别写
 *   「函数名紧跟左括号」，否则会被误判成「函数用在定义之前」。 */
static int      g_h_toobig   = 0;   /* 分辨率超出车机承受能力，已经报过了 */
static int      g_trunc_warn = 0;   /* 视频帧被收帧缓冲截断，已经报过了 */

/* 会话线程把日志打开好了没有 —— 主线程靠它决定能不能往日志里写。
 * 见 WinMain 里那一段等待：不等就直接写，那几行会被静默丢掉。 */
static volatile int g_log_ready = 0;

/* 主线程的界面初始化走到第几步（0=还没开始 1=建字体中 2=字体好了 3=画界面中 4=画完）。
 * ⚠ 这是一份【不依赖日志】的兜底记录：万一主线程那几行日志因为任何原因丢了，
 *   会话线程在开始连接之前把它的值写进日志，仍然能告诉我们界面走到哪一步了。 */
static volatile int g_boot_step = 0;

/* 收到过几次 WM_CLOSE。车机外壳有可能在偷偷关我们的窗口 ——
 * 体检程序里这一项是关键证据，主程序里也留着，方便从日志看。 */
static int      g_close_seen = 0;

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
/* UTF-16 → UTF-8。
 *
 * ⚠⚠ 原来这里是「非 ASCII 一律换成 '?'」，于是日志里【所有】
 *   `屏幕 | …` 那一行的中文全都变成了问号：
 *       [5.258] 屏幕 | ??????? 4 ?????????????????? ADB ?????????????
 *   而这一行恰恰是「屏幕上当时到底显示了什么」的唯一记录 ——
 *   用户拍屏幕 + 带日志两条线索本来是要互相印证的，
 *   中文一没，这条路就断了。
 *
 *   日志文件本身是 UTF-8（cl_log 直接写字节，文件头带 BOM），
 *   所以这里必须产出真正的 UTF-8，而不是 ASCII 剔除版。 */
static void w2a_log(const WCHAR *w, char *a, int cap)
{
    int i = 0, o = 0;

    if (!w || !a || cap <= 0) return;

    while (w[i] && o < cap - 1) {
        unsigned int cp = (unsigned int)w[i++];

        /* 代理对：先高后低，拼回码点 */
        if (cp >= 0xD800u && cp <= 0xDBFFu &&
            w[i] >= 0xDC00u && w[i] <= 0xDFFFu) {
            cp = 0x10000u + ((cp - 0xD800u) << 10)
                          + ((unsigned int)w[i] - 0xDC00u);
            i++;
        }

        if (cp < 0x80u) {
            a[o++] = (char)cp;
        } else if (cp < 0x800u) {
            if (o + 2 > cap - 1) break;
            a[o++] = (char)(0xC0u | (cp >> 6));
            a[o++] = (char)(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            if (o + 3 > cap - 1) break;
            a[o++] = (char)(0xE0u | (cp >> 12));
            a[o++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            a[o++] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            if (o + 4 > cap - 1) break;
            a[o++] = (char)(0xF0u | (cp >> 18));
            a[o++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            a[o++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            a[o++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    a[o] = 0;
}

static void set_status(const WCHAR *s)
{
    int i;
    char a[300];
    for (i = 0; i < 250 && s[i]; i++)
        g_status[i] = s[i];
    g_status[i] = 0;
    if (g_hwnd)
        InvalidateRect(g_hwnd, 0, FALSE);
    /* ⚠ 屏幕上显示什么，日志里就原样记一份。
     *   用户是「拍屏幕」+「带日志」两条线索一起给我，
     *   两边必须能对上，否则没法互相印证。 */
    w2a_log(s, a, (int)sizeof(a));
    cl_log("屏幕 | %s", a);
}

/* ── ADB 握手过程 → 屏幕 ────────────────────────────────────────────────
 *
 * 这是现场反馈回来的一个真问题：**握手明明成功了，屏幕上却一直停在
 * 「没认到手机」** —— 用户只有把日志拷回来才知道「刚才其实连上过」。
 * 原因是握手过程只写了日志，没有一处去更新界面。
 *
 * 现在每一步都往屏幕上说一句，站在车边上就能看明白走到哪了；
 * 尤其是「发公钥」那一步 —— 手机马上要弹「允许 USB 调试」，
 * 这一句必须让人看见，否则用户根本不知道该看手机。
 *
 * ⚠ 用 strstr 做子串匹配，不用 strcmp：本工具链的 coredll 里没有 strcmp，
 *   strstr 是有的（而且这些标记串本来就是我们自己写的中文，匹配很稳）。 */
static void adb_progress(const char *what, unsigned int a, unsigned int b)
{
    (void)a; (void)b;
    if (!what) return;

    if (strstr(what, "发公钥")) {
        ui_headline(L"手机已经应答，正在认证",
                    L"⚠ 请看手机屏幕，点「允许 USB 调试」");
        set_status(L"⚠ 现在请看手机：会弹出「允许 USB 调试」，"
                   L"勾上「一律允许」再点「允许」");
    } else if (strstr(what, "握手完成")) {
        ui_headline(L"已认到手机", L"正在建立数据通道 …");
        set_status(L"✅ 已认到手机（ADB 握手成功），正在建数据通道 …");
    } else if (strstr(what, "收到 AUTH")) {
        set_status(L"手机已应答，正在做 ADB 认证 …");
    } else if (strstr(what, "已发 CNXN")) {
        set_status(L"已认到 USB 设备，正在和手机握手 …");
    } else if (strstr(what, "超时") || strstr(what, "失败")) {
        set_status(L"握手中断，稍后自动重试（细节见日志）");
    }
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

#ifdef HAS_H264
/* ── H.264 解码 ──────────────────────────────────────────────────────────────
 * 车机没法实测，所以这一段每一步都必须能把「发生了什么」显示到窗口上，
 * 而不是悄悄失败。这也是 h264dec 里那套取证记录存在的理由。 */
static H264DEC       *g_h264     = 0;
static unsigned char *g_hbuf     = 0;
static int            g_hbufcap  = 0;
static int            g_h_reported = 0;

/* 按 SPS 报出来的尺寸准备输出缓冲；SPS 没到之前先给一个 800x480 的档。 */
static int h264_ensure_buf(void)
{
    int w = h264dec_width(g_h264);
    int h = h264dec_height(g_h264);
    int need = (w > 0 && h > 0) ? w * h * 4 : (800 * 480 * 4);

    if (need <= 0 || need > 8 * 1024 * 1024)
        return -1;                       /* 尺寸离谱，先不分配 */
    if (g_hbuf && g_hbufcap >= need)
        return 0;
    if (g_hbuf) { free(g_hbuf); g_hbuf = 0; g_hbufcap = 0; }
    g_hbuf = (unsigned char *)malloc((size_t)need);
    if (!g_hbuf)
        return -1;
    g_hbufcap = need;
    return 0;
}

static int show_h264(const unsigned char *buf, int len)
{
    int w = 0, h = 0, r;
    WCHAR t[200];

    if (!g_h264) {
        g_h264 = h264dec_open();
        if (!g_h264) {
            set_status(L"H.264 解码器初始化失败（内存不够）");
            return 1;
        }
    }
    if (h264_ensure_buf() != 0) {
        set_status(L"H.264 输出缓冲分配失败（内存不够）");
        return 1;
    }

    /* ⚠ 分辨率上限。h264bsd 的参考帧缓冲按「每帧 宽×高×1.5」算：
     *   480x272 一帧约 196KB，没事；1080p 一帧就 3MB，几帧就能把老车机的内存吃光。
     *   内存耗尽的表现是【卡死或崩掉】，而不是给个提示 —— 那样这一趟就白跑了。
     *   所以这里主动卡一道，超了就停下并明确告诉用户去调低手机端画质。 */
    if (h264dec_width(g_h264) > 0) {
        int sw2 = h264dec_width(g_h264), sh2 = h264dec_height(g_h264);
        if (sw2 > 1024 || sh2 > 768 || (long)sw2 * sh2 > 900L * 600L) {
            if (!g_h_toobig) {
                g_h_toobig = 1;
                cl_log("⚠⚠ 手机推的画面太大: %dx%d —— 停止解码，避免把车机内存吃光",
                       sw2, sh2);
                wsprintfW(t, L"手机推的画面太大（%dx%d），继续解会把车机内存吃光。"
                             L"请在手机上把投屏画质调低，或按「分辨率」按钮换一档重连。",
                          sw2, sh2);
                set_status(t);
            }
            return 1;
        }
    }
    if (g_h_toobig)
        return 1;                      /* 已经在超大状态，不再喂 */

    r = h264dec_feed(g_h264, buf, len, g_hbuf, g_hbufcap, &w, &h);

    if (r < 0) {
        g_miss++;
        if (g_miss == 1 || g_miss == 3 || g_miss == 30)
            cl_log("H.264 解码出错（返回 %d），累计失败 %d 次；解码器日志：%s",
                   r, g_miss, h264dec_log(g_h264));
        if (g_miss == 3) {
            WCHAR t2[260];
            WCHAR wl[200];
            const char *lg = h264dec_log(g_h264);
            a2w_ui(lg, wl, 200);        /* ⚠ 必须走 UTF-8 解码，别再逐字节强转 */
            wsprintfW(t2, L"H.264 解码出错：%s", wl);
            set_status(t2);
        }
        return 1;
    }

    if (r == H264DEC_GOT_FRAME) {
        if (disp_init(&g_disp, w, h) != 0)
            return 1;
        disp_set_bgra(&g_disp, g_hbuf, w, h);
        g_decoded++;
        g_shown++;
        g_miss = 0;
        if (!g_h_reported) {
            g_h_reported = 1;
            if (g_shown == 1) {
                cl_log_stage(8, 8, "画面已经出来（首次成功解码并显示）");
            }
            cl_log("   H.264 出画: %dx%d  profile=%d(%s)  切帧识别=%s",
                   w, h, h264dec_profile(g_h264),
                   h264dec_profile_name(h264dec_profile(g_h264)),
                   h264dec_format_name(h264dec_format(g_h264)));
            wsprintfW(t, L"H.264 %dx%d  第 %d 帧  profile=%d",
                      w, h, g_frames, h264dec_profile(g_h264));
            set_status(t);
        }
        if (g_hwnd)
            InvalidateRect(g_hwnd, 0, FALSE);
        return 1;
    }

    /* 还没出帧。SPS 一到就把尺寸和 profile 报出来 ——
     * 万一是 Main/High profile，h264bsd 解不了，这句话就是唯一线索。 */
    if (!g_h_reported && h264dec_width(g_h264) > 0) {
        g_h_reported = 1;
        cl_log("H.264 SPS 已识别: %dx%d  profile=%d(%s)  切帧识别=%s",
               h264dec_width(g_h264), h264dec_height(g_h264),
               h264dec_profile(g_h264),
               h264dec_profile_name(h264dec_profile(g_h264)),
               h264dec_format_name(h264dec_format(g_h264)));
        if (h264dec_profile(g_h264) != 66 && h264dec_profile(g_h264) != 0) {
            cl_log("   ⚠⚠ 关键问题：手机的编码 profile 不是 Baseline(66)。");
            cl_log("       本项目用的解码器 h264bsd 只支持 Baseline，");
            cl_log("       这一条就是黑屏的原因。请把这一行发给开发者。");
        }
        wsprintfW(t, L"H.264 已识别 SPS：%dx%d  profile=%d（h264bsd 只吃 Baseline=66）",
                  h264dec_width(g_h264), h264dec_height(g_h264),
                  h264dec_profile(g_h264));
        set_status(t);
    }
    return 1;
}
#endif

static void show_frame(const unsigned char *buf, int len)
{
    int r;

    /* ⚠ 不管解不解得开，先把收到的【原始字节】存一份（最多 256KB）。
     *   拿回来我就能在电脑上离线判断：到底是 JPEG 还是 H.264、
     *   什么 profile、怎么切帧 —— 这些靠猜永远猜不准，
     *   靠这段原始数据一眼就能看出来。这是最有价值的一个文件。 */
    cl_log_dumpfile("video-raw.bin", buf, len);

    /* ⚠ 收帧缓冲是有限的（RX_CAP）。一旦某帧比它大，收到的就是【被截断的一半】——
     *   JPEG 顶多花一帧，H.264 会直接把解码器带偏（表现为花屏或长时间黑屏）。
     *   这种事在车机上完全看不出来，所以必须记进日志。 */
    if (len >= g_rxcap && !g_trunc_warn) {
        g_trunc_warn = 1;
        cl_log("⚠⚠ 收到的一帧(%d 字节)顶满了收帧缓冲(%d) —— 很可能被截断了。"
               "日志里出现这一行，说明该把手机端分辨率调低。",
               len, g_rxcap);
    }

    if (len >= 2 && buf[0] == 0xFF && buf[1] == 0xD8) {
        /* JPEG */
        njInit();
        r = njDecode(buf, len);
        if (r != NJ_OK) {
            njDone();
            g_miss++;
            if (g_miss == 1)
                cl_log("JPEG 解码失败（njDecode 返回 %d），本帧 %d 字节", r, len);
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
        if (g_shown == 1) {
            cl_log_stage(8, 8, "画面已经出来（首次成功解码并显示）");
            cl_log("   格式=JPEG  尺寸=%dx%d", njGetWidth(), njGetHeight());
        }
        if (g_hwnd)
            InvalidateRect(g_hwnd, 0, FALSE);
    } else {
#ifdef HAS_H264
        /* 不是 JPEG —— 按 H.264 解。手机端多数走 H.264，这条是主路径。 */
        if (show_h264(buf, len))
            return;
#endif
        /* 真不认识。不装作在显示，直接说明并把原始数据存下来。 */
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
/* ANSI → WCHAR，只用于把日志/转发器里的说明搬到界面上。
 * 手写而不用 MultiByteToWideChar：本工具链的 coredll 上那些转换函数不一定有导出。 */

/* 把当前状态同步到界面上按钮的文字。
 * 按钮的矩形和文字都由 ui.c 统一管理，这里只推状态 ——
 * 避免出现「画在 A 处、点在 B 处」这种最难查的错位。 */
static void sync_button_labels(void)
{
    ui_button_label(UI_BTN_TOUCH, g_touch_mode == 0 ? L"触摸 A" : L"触摸 B");
    ui_button_label(UI_BTN_RES, g_presets[g_preset].name);
    ui_button_label(UI_BTN_EXIT, L"退出");
}

/* 阶段轨道 + 日志，一次写完 —— 两边必须一致：
 * 用户拍屏幕和拿日志回来，要对得上号。 */
static void stage_set(int n, int state, const WCHAR *detail)
{
    ui_stage(n, state);
    ui_stage_detail(n, detail);
    if (g_hwnd)
        InvalidateRect(g_hwnd, 0, FALSE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc, area;
        GetClientRect(h, &rc);

        /* 画面区由 ui.c 统一决定（只留底部一条 26px 细栏）*/
        ui_view_rect(&rc, &area);

        if (g_disp.fb) {
            disp_paint(&g_disp, dc, &area);
            ui_paint_statusbar(dc, &rc);      /* 连上了：只剩一条细状态条 */
        } else if (!ui_ready()) {
            /* 界面资源还没建好（或建失败了）—— 走兜底画法，
             * 只用骨架程序在真机上验证过的 API，保证屏幕上有东西。 */
            ui_paint_minimal(dc, &rc);
        } else {
            ui_paint_connect(dc, &rc);        /* 没连上：整屏连接进度板 */
        }

        EndPaint(h, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        RECT rc, area;
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        int px, py;
        GetClientRect(h, &rc);

        /* 底部栏按钮：命中测试交给 ui.c。
         * 绝不能在这里另写一套坐标 —— 一旦和绘制用的矩形不一致，
         * 就会出现「看得见点不着」，而且极难查。 */
        {
            int b = ui_hit_button(x, y, &rc);
            if (b == UI_BTN_EXIT) {
                g_quit = 1;
                DestroyWindow(h);
                return 0;
            }
            if (b == UI_BTN_RES) {
                g_preset = (g_preset + 1) % N_PRESETS;
                save_preset();
                apply_preset();
                sync_button_labels();
                {
                    WCHAR t[200];
                    wsprintfW(t, L"已切换到 %s", g_presets[g_preset].name);
                    set_status(t);
                }
                InvalidateRect(h, 0, FALSE);
                return 0;
            }
            if (b == UI_BTN_TOUCH) {
                g_touch_mode = (g_touch_mode == 0) ? 1 : 0;
                sync_button_labels();
                set_status(g_touch_mode == 0
                           ? L"触摸写法 A：专用消息 + 单点坐标"
                           : L"触摸写法 B：通用消息 + 动作+坐标");
                InvalidateRect(h, 0, FALSE);
                return 0;
            }
        }

        ui_view_rect(&rc, &area);
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
        ui_view_rect(&rc, &area);
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
        ui_view_rect(&rc, &area);
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

    /* 整窗都是我们自己画的（含深色底），让系统先擦成白色只会闪一下。
     * 直接返回 1 = 「背景我已经处理了」。 */
    case WM_ERASEBKGND:
        return 1;

    case WM_KEYDOWN:
        if (w == VK_ESCAPE || w == VK_BACK) {
            g_quit = 1;
            DestroyWindow(h);
        }
        return 0;

    /* ⚠⚠ WM_CLOSE 不再销毁窗口退出。
     *
     *   骨架程序**根本没有这个分支**（它走 DefWindowProc），而它能好好留在屏幕上；
     *   主程序却在这里主动退出。如果车机外壳会往窗口发 WM_CLOSE，
     *   那主程序就是被它关掉的 —— 表现正是「闪一下就没了」。
     *
     *   体检程序专门把 WM_CLOSE 计数显示在屏幕上验证这一点。
     *   现在改成：记一笔、重画，但【绝不退出】。
     *   真正退出只走右下角那个「退出」按钮。 */
    case WM_CLOSE:
        g_close_seen++;
        cl_log("收到 WM_CLOSE（第 %d 次）—— 按设计【不退出】", g_close_seen);
        InvalidateRect(h, 0, FALSE);
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
/* 决定走哪条路。
 * 返回 0 表示已确定 g_ip 与三个端口；返回 -1 表示两条路都不通。
 *
 * ⚠ 这里是【等待循环】，不是只试一次就报错：
 *   用户的实际操作顺序是「先开程序 → 再插线 → 再在手机上点允许 USB 调试」，
 *   也就是【程序必须先跑起来等人】，所以必须一直等、一直把当前该做什么显示在
 *   屏上 —— 屏上那两行字是这个程序唯一的引导手段。
 *
 * ⚠ 本函数跑在【后台线程】里（见 session_thread）。绝不能挪回主线程：
 *   主线程一旦被这里挡住，窗口连一次 WM_PAINT 都处理不了，
 *   「请插 USB 线」这类提示一个字都显示不出来。 */
static int open_transport(void)
{
    static const char *svc[3] = { "tcp:7240", "tcp:8240", "tcp:9340" };
    unsigned short ports[3];
    char   reason[256];
    WCHAR  t[320];
    int    tries = 0;
    DWORD  t0 = GetTickCount();
#define WAITED_SEC ((int)((GetTickCount() - t0) / 1000))

    for (;;) {
        reason[0] = 0;

        if (tries == 0) {
            set_status(L"① 用 USB 线把手机连到车机      "
                       L"② 手机上打开「USB 调试」并点「允许」");
        } else {
            wsprintfW(t, L"① 用 USB 线连手机   ② 打开「USB 调试」并允许"
                         L"      （已等待 %d 秒）", WAITED_SEC);
            set_status(t);
        }

        if (tries == 0) {
            cl_log_stage(1, 8, "打开 ADB 设备 / ADB 认证 / 建立端口转发");
            ui_headline(L"用 USB 线把手机连到车机",
                        L"然后在手机上打开「USB 调试」并点「允许」");
            stage_set(0, UI_ST_ACTIVE, L"正在等待…");
        }

        if (adbp_start(svc, 3, ports, reason, (int)sizeof(reason)) == 0) {
            g_ip         = htonl(0x7F000001UL);   /* 连本机，端口已被转到手机上 */
            g_port_cmd   = ports[0];
            g_port_vid   = ports[1];
            g_port_touch = ports[2];
            g_route      = 1;
            g_adb_ok     = 1;
            cl_log_step("ADB 设备打开 + 认证 + 端口转发", 1, 0);
            {
                WCHAR w[80];
                a2w_ui(reason, w, 80);
                stage_set(0, UI_ST_DONE, w);
            }
            cl_log("   本地转发端口: CMD=%u  VIDEO=%u  TOUCH=%u",
                   (unsigned)ports[0], (unsigned)ports[1], (unsigned)ports[2]);
            cl_log("   接下来连的是 127.0.0.1:%u（数据经 ADB 隧道到手机）",
                   (unsigned)ports[0]);
            return 0;
        }

        cl_log_step("ADB 设备打开 + 认证 + 端口转发", 0, reason);
        {
            WCHAR w[80];
            a2w_ui(reason, w, 80);
            stage_set(0, UI_ST_FAIL, w);
            /* ⚠ 这里原来写的是「换个车机 USB 口，或换一根能传数据的数据线」——
             *   但现场 90% 的失败其实是手机侧的两个开关没配对，
             *   而屏幕上那句话会把用户引去换线换口，白折腾。
             *   现在直接把手机上要做的事写在脸上。 */
            ui_headline(L"没认到手机",
                        L"手机上打开「USB 调试」，USB 用途选「传输文件」");
            {
                WCHAR f[200];
                wsprintfW(f, L"已等 %d 秒。「连接车辆」会把调试接口顶掉，"
                             L"先别选它。", WAITED_SEC);
                ui_footline(f);
            }
        }
        cl_log("   第 %d 次尝试失败（已等 %d 秒）。常见的三个原因：",
               tries + 1, WAITED_SEC);
        cl_log("     · 手机上没开「开发者选项 → USB 调试」"
               "（不开，车机的 ADB 驱动根本不会加载 = 错误 55）");
        cl_log("     · USB 用途选成了「连接车辆」——它会把 ADB 接口顶掉，"
               "请改回「传输文件」再试");
        cl_log("     · 手机没插线 / 线只充电不传数据 / 插的不是支持数据的 USB 口");

        /* ADB 不通 —— 把原因留着，这是现场排查最关键的信息。
         * ⚠ 这里不能用 lstrcpynA：本工具链的 coredll 里只有 Unicode 版的
         *   lstrcpynW，链接时会报 undefined reference。手写循环最稳妥。 */
        {
            int k;
            for (k = 0; k < (int)sizeof(g_adb_reason) - 1 && reason[k]; k++)
                g_adb_reason[k] = reason[k];
            g_adb_reason[k] = 0;
        }

        tries++;
        if (g_quit)
            return -1;                        /* 用户按了退出，别死等 */
        if (WAITED_SEC >= WAIT_SECS)
            break;                            /* 等够时间还没成，退到下面走网共享 */

        /* 等了几轮之后，把失败原因也显示出来，方便现场判断卡在哪一步 */
        if (tries >= 3 && g_adb_reason[0]) {
            WCHAR w[300];
            a2w_ui(g_adb_reason, w, 300);   /* ⚠ UTF-8 → UTF-16，见 a2w_ui 的说明 */
            wsprintfW(t, L"等待手机中（已 %d 秒）…%s", WAITED_SEC, w);
            set_status(t);
        }

        Sleep(2000);                          /* 给窗口留出刷新的时间 */
    }
#undef WAITED_SEC

    /* 退回 USB 网络共享：手机开共享后车机会拿到 IP，扫常见网段 */
    {
        unsigned long ips[16];
        int n = cl_candidate_ips(ips, 16), k;
        for (k = 0; k < n; k++) {
            SOCKET sk = cl_connect(ips[k], CL_PORT_CMD, 1200);
            if (sk != INVALID_SOCKET) {
                closesocket(sk);
                g_ip = ips[k];
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

/* 会话跑在后台线程。主线程必须留在消息循环里，否则窗口根本不刷新。 */
/* 前向声明：下面的线程函数要用到它。
 * ⚠ 必须让这一行【以分号结尾】—— scripts/check_order.py 的正则要求
 *   `^static ... ;\s*$`，分号后面跟注释就匹配不上，CI 的自查会判为缺失。 */
static void run_session(void);
static HANDLE g_hthr = 0;

static DWORD WINAPI session_thread(LPVOID param)
{
    (void)param;

    /* ⚠⚠ 日志在这里打开，【绝不在主线程打开】—— 见 WinMain 里的说明。
     *   往 U 盘写日志会把 WriteFile 卡死，一旦它跑在主线程上，
     *   整个程序就再也画不出窗口（实锤：窗口建了却一次都没画过）。
     *   放在这个线程里，它卡住也只是连接跑不起来，屏幕照样是活的。 */
    if (cl_log_open() != 0)
        set_status(L"日志文件建不出来，程序继续跑（但出问题就没日志可查）");
    cl_log("──────── 启动自检 ────────");
    cl_log("第1步 窗口已显示、界面已就绪（主线程，全程不碰文件）");
    cl_log("第2步 日志已打开");
    cl_log("   日志文件: %s", cl_log_path());
    cl_log("   屏幕 %dx%d, 等手机 %d 秒",
           GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), WAIT_SECS);
    cl_log_sync();

    /* ⚠⚠ 到这里【才】告诉主线程「日志可用」。
     *
     *   原来是在 cl_log_open 一成功就置位，结果主线程立刻开始写
     *   「界面 0/4…」，正好和上面这段启动日志撞在同一时刻 ——
     *   两个线程同时写同一个文件句柄，实测就是在那一下闪退的。
     *   挪到这里之后，启动日志先安安静静写完，主线程才开始写，
     *   两边在时间上错开。 */
    g_log_ready = 1;

    /* 把日志路径显示到屏幕上 —— 用户得知道去哪儿拿这个文件。
     * 日志现在优先写车机内部存储（不再写 U 盘），所以这一行尤其重要。 */
    {
        WCHAR t[300], w[260];
        const char *pp = cl_log_path();
        a2w_ui(pp, w, 260);             /* ⚠ UTF-8 → UTF-16 */
        if (pp[0]) {
            wsprintfW(t, L"日志：%s", w);
            set_status(t);
            Sleep(1200);              /* 让用户看清在哪儿 */
        }
    }

    /* ⚠ 网络栈初始化也在这里做，【不放主线程】——
     *   它在某些 ROM 上会卡住，留在主线程上就是又一次
     *   「窗口建好了但消息循环起不来、屏幕一片空白」。 */
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            cl_log("第3步 网络栈启动失败（车机没有 ws2）—— 没法连手机了");
            set_status(L"网络栈启动失败：车机没有 ws2");
            /* 没网络就连不上，但窗口和界面是活的，用户至少看得见 */
        } else {
            cl_log("第3步 网络栈已启动");
        }
    }
    cl_log_sync();

    /* ⚠ 把主线程的界面进度写进日志 —— 这是日志之外的兜底：
     *   万一主线程那几行日志丢了，这一行仍然能说明界面走到第几步。 */
    cl_log("启动前状态: 主线程界面进度 = %d/4", (int)g_boot_step);
    cl_log("──────── 自检结束，开始连接 ────────");

    /* 把 ADB 握手的每一步接到屏幕上（见 adb_progress 的说明） */
    adbp_set_trace_cb(adb_progress);

    for (;;) {
        run_session();
        if (g_quit)
            break;

        /* ⚠⚠ 这一圈【必须能自己重来】，否则很容易白跑一趟。
         *
         *   会话结束而用户没按退出，就说明中途失败了 —— 而现场最常见的失败
         *   恰恰是「用户第一次没来得及点『允许 USB 调试』」。
         *   那一下错过，ADB 认证就过不去，整条链路到此为止。
         *   不给重试的话，用户只能退出程序、重新启动一次，
         *   而人正坐在车里 —— 这一趟就白跑了。
         *
         *   所以：失败不是终点，等几秒把上一轮收干净，从头再来。 */
        set_status(L"没连上，8 秒后自动重来（按「退出」结束这一轮）");
        cl_log("=== 本轮会话结束但没连上，8 秒后自动重来 ===");
        cl_log("    如果手机还没插线或还没点「允许 USB 调试」，现在补上就行。");

        {
            int k;
            for (k = 0; k < 80 && !g_quit; k++)
                Sleep(100);
        }
        if (g_quit)
            break;

        /* 重来之前把上一轮的东西收干净，免得残留一半的状态卡住下一轮 */
        if (g_cmd   != INVALID_SOCKET) { closesocket(g_cmd);   g_cmd   = INVALID_SOCKET; }
        if (g_vid   != INVALID_SOCKET) { closesocket(g_vid);   g_vid   = INVALID_SOCKET; }
        if (g_touch != INVALID_SOCKET) { closesocket(g_touch); g_touch = INVALID_SOCKET; }
        if (g_adb_ok) { adbp_stop(); g_adb_ok = 0; }
        /* 设备句柄每轮重开一次：手机拔插过之后旧句柄可能已经失效。
         * 一轮一开既能自愈，又不会像以前那样每 2 秒就折腾它一次
         * （那个驱动的关闭是脏的，见 adbio_ce.c 的说明）。 */
        adbio_ce_reopen();
        g_route      = 0;
        g_miss       = 0;
        g_trunc_warn = 0;
        g_h_toobig   = 0;          /* 下一轮手机可能已经调低画质了 */
        g_frames     = 0;
        g_decoded    = 0;
        g_shown      = 0;
        g_fps        = 0;
        g_t0         = GetTickCount();

        ui_stages_reset();
        ui_footline(L"");
        sync_button_labels();
        ui_headline(L"用 USB 线把手机连到车机",
                    L"然后在手机上打开「USB 调试」并点「允许」");
        stage_set(0, UI_ST_ACTIVE, L"正在等待…");
        if (g_hwnd)
            InvalidateRect(g_hwnd, 0, FALSE);

        cl_log("=== 开始新的一轮 ===");

        /* 每一轮开头把车机的 USB/ADB 状态记一遍。
         * 这是判断「ADB1: 为什么打不开」的根：
         *   错误 55 = 驱动根本没注册（手机上没开 USB 调试）；
         *   错误 110 = 驱动在、但 ADB 接口还没就绪。
         * 两种原因的处理办法完全不同，凭屏幕上的「打不开」是分不出来的。 */
        {
            unsigned long ib, ob, re, we, nd, le;
            int  ru = 0, rc = 0;
            unsigned long rd = 0;

            adbio_ce_stats(&ib, &ob, &re, &we, &nd, &le);
            adbio_ce_ring(&ru, &rc, &rd);
            cl_log("   上一轮设备统计: 收到 %lu 字节 / 发出 %lu 字节 / "
                   "读失败 %lu 次 / 写失败 %lu 次 / 空读 %lu 次 / 末次错误 %lu",
                   ib, ob, re, we, nd, le);
            /* 「读线程还活着吗」+ 环形缓冲水位：
             * 这两个数一出来，就能立刻分清「设备不行了」和「我们自己的读线程没了」 */
            cl_log("   读线程 %s，环形缓冲还剩 %d 字节（容量 %d），缓冲满丢过 %lu 次",
                   adbio_ce_reader_alive() ? "活着" : "已退出", ru, rc, rd);
            adbio_ce_stats_reset();
        }
        adbio_ce_dump_usb_state(cl_log_line);
    }

    WSACleanup();
    return 0;
}

static void run_session(void)
{
    SOCKET cmd;
    int st;
    unsigned long t0 = 0;

    if (open_transport() != 0) {
        WCHAR t[400];
        WCHAR wreason[300];
        a2w_ui(g_adb_reason, wreason, 300);   /* ⚠ UTF-8 → UTF-16 */
        wsprintfW(t, L"两条路都不通。ADB: %s ／ USB网卡: 也没找到手机 "
                      L"（请确认已开 USB 调试或 USB 网络共享，且 Jovi InCar 已启动）",
                  wreason);
        set_status(t);
        return;
    }

    /* ── 拉起手机端的智能车载 ──────────────────────────────────────────────
     * ⚠ 这一步原来是【完全缺失】的，而它正是「车机提示需要打开手机端 CarLife」
     *   那个症状的根源：
     *   车机连上 ADB 之后直接就去连 7240，可手机上的智能车载
     *   （vivo Jovi InCar / OPPO 车联 / 小米 CarWith，底层都是百度 CarLife 组件）
     *   当时根本没启动，也就没有人监听 7240，连接当然失败。
     *   百度自己的车机程序是靠 `am start -n com.baidu.carlife/...` 把手机端拉起来的，
     *   这一步不能省。
     *
     * 交给 adbp_launch_phone_app：它会先 `cmd package resolve-activity` 问 Android
     * 要启动项（对"无界面组件"也有效），再 am start，最后用 pidof 确认真的起来了 ——
     * 不是发个命令就当成功。 */
    if (g_route == 1) {
        char detail[256];
        int  k;

        /* ── 趁着 ADB 通了，【直接问手机】它现在是什么状态 ──────────────────
         *
         * 为什么要这一步：现场一直在争「USB 用途该选传输文件还是连接车辆」。
         * 与其猜，不如问 —— ADB 一旦通，手机对我们就不再是黑盒了，
         * getprop 能把它此刻的 USB 配置、机型、系统版本原样报出来。
         * 这样「哪个选项能让车机认出手机」就从一个观点变成了一条数据。
         *
         * 这几个属性名是安卓一贯的，各家 ROM 都在：
         *   sys.usb.config / sys.usb.state  → 当前 USB 功能组合，例如 "mtp,adb"
         *   里面有没有 "adb" 才是关键 —— 有 adb 就说明 ADB 接口在，
         *   而 ADB 接口在不在，和「传输文件 / 连接车辆」是两码事。 */
        {
            static const char *Q[] = {
                "getprop sys.usb.config",
                "getprop sys.usb.state",
                "getprop persist.sys.usb.config",
                "getprop ro.product.brand",
                "getprop ro.product.model",
                "getprop ro.build.version.release"
            };
            char got[160];
            int  qi;

            cl_log("── 手机自报家门（用 ADB shell 问出来的，不是猜的）──");
            for (qi = 0; qi < (int)(sizeof(Q) / sizeof(Q[0])); qi++) {
                got[0] = 0;
                if (adbp_shell(Q[qi], got, (int)sizeof(got), 5000) == 0)
                    cl_log("   %s = [%s]", Q[qi], got);
                else
                    cl_log("   %s = （问不出来）", Q[qi]);
            }
            cl_log("   提示: sys.usb.config 里只要含 adb，就说明 ADB 接口在；"
                   "它和「传输文件 / 连接车辆」是两件事。");
        }

        cl_log_stage(2, 8, "拉起手机端智能车载（ADB shell）");
        stage_set(1, UI_ST_ACTIVE, L"正在启动…");
        ui_headline(L"手机已连上，正在启动车载系统",
                    L"如果手机弹出权限框，请点「允许」");
        ui_footline(L"");
        {
            const char *pk = adbp_phone_packages();
            if (pk && pk[0]) {
                cl_log("   手机里的包名清单（原样记录，供离线核对）：");
                cl_log("   ──────────── 8< ────────────");
                cl_log("%s", pk);
                cl_log("   ──────────── >8 ────────────");
            } else {
                cl_log("   （还没拿到包名清单）");
            }
        }

        for (k = 0; k < 3; k++) {          /* 最多拉 3 次，给手机留出启动时间 */
            if (adbp_launch_phone_app(detail, (int)sizeof(detail)) == 0) {
                cl_log_step("拉起手机端智能车载", 1, 0);
                cl_log("   %s", detail);
                {
                    WCHAR w[80];
                    a2w_ui(detail, w, 80);
                    stage_set(1, UI_ST_DONE, w);
                }
                break;
            }
            cl_log_step("拉起手机端智能车载", 0, detail);
            {
                WCHAR w[80];
                a2w_ui(detail, w, 80);
                stage_set(1, UI_ST_FAIL, w);
            }
            {
                WCHAR t[420], w[300];
                a2w_ui(detail, w, 300);       /* ⚠ UTF-8 → UTF-16 */
                wsprintfW(t, (k < 2) ? L"正在启动手机端的智能车载…  %s"
                                     : L"手机端没能启动：%s", w);
                set_status(t);
            }
            if (k < 2 && !g_quit)
                Sleep(3000);
        }
    }

    /* 手机端起来之后还要一两秒才会开始监听 7240，所以这里必须重试，
     * 不能连一次不通就放弃。 */
    cl_log_stage(3, 8, "连接控制通道（经 ADB 转发到手机的 7240）");
    if (g_route == 1) stage_set(2, UI_ST_ACTIVE, L"正在连…");
    {
        int k;
        cmd = INVALID_SOCKET;
        for (k = 0; k < 10; k++) {
            cmd = connect_cmd();
            if (cmd != INVALID_SOCKET) {
                cl_log_step("连上控制通道 7240", 1, 0);
                cl_log("   第 %d 次尝试成功", k + 1);
                stage_set(2, UI_ST_DONE, L"7240");
                break;
            }
            cl_log("   第 %d 次连 7240 不通（手机端可能还没开始监听）", k + 1);
            if (g_quit)
                return;
            if (g_route == 1) {
                WCHAR t[220];
                wsprintfW(t, L"手机端已启动，正在等它开始监听 7240（第 %d 次）…", k + 1);
                set_status(t);
            }
            Sleep(1000);
        }
    }
    if (cmd == INVALID_SOCKET) {
        cl_log_step("连上控制通道 7240", 0,
                    "连了 10 次都不通，手机端始终没监听");
        stage_set(2, UI_ST_FAIL, L"手机端没监听");
        ui_headline(L"手机连上了，但车载系统没响应",
                    L"看一眼手机上是不是弹了权限框，点「允许」");
        cl_log("   ⚠ 到这里就卡住了。可能原因：");
        cl_log("     · 手机端智能车载没被拉起来（看上面一阶段的结果）");
        cl_log("     · 手机上弹了权限框没点「允许」");
        cl_log("     · 手机的智能车载不认这种连接方式（有的机型只支持无线）");
        set_status(g_route == 1
                   ? L"ADB 已就绪，但手机端始终没在监听 7240 —— 手机上可能弹了权限框，请点「允许」"
                   : L"没找到手机（7240 端口都不通）");
        return;
    }
    set_status(g_route == 1
               ? L"已通过 ADB 直连手机，正在握手 ..."
               : L"已通过 USB 网络共享连上手机，正在握手 ...");

    cl_log_stage(4, 8, "CarLife 协议握手（问手机支持哪个协议版本）");
    stage_set(3, UI_ST_ACTIVE, L"协商中…");
    {
        int match = -1;
        unsigned long reply = 0;
        st = cl_handshake(cmd, &match, &reply);
        if (st == CL_OK) {
            cl_log_step("协议握手", 1, 0);
            cl_log("   手机回的版本匹配状态 = %d，原始值 = %lu", match, reply);
            {
                WCHAR w[40];
                wsprintfW(w, L"版本 %d", match);
                stage_set(3, UI_ST_DONE, w);
            }
        } else {
            cl_log_step("协议握手", 0, "手机没回应协议版本");
            stage_set(3, UI_ST_FAIL, L"手机没回应");
        }
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

    cl_log_stage(5, 8, "打开视频通道（8240）");
    stage_set(4, UI_ST_ACTIVE, L"正在连…");
    g_vid = cl_connect(g_ip, (int)g_port_vid, 2000);
    if (g_vid == INVALID_SOCKET) {
        cl_log_step("打开视频通道 8240", 0, "连不上");
        set_status(L"视频通道 8240 连不上");
        return;
    }
    cl_log_step("打开视频通道 8240", 1, 0);
    stage_set(4, UI_ST_DONE, L"8240");

    /* 参考实现要求：每个通道使用前都要重发一次协议版本。 */
    cl_resend_version(cmd);

    /* 这三个都走 CMD 通道（已由参考源码证实：
     * 它们是 CCmdChannelModule 的成员方法，不是视频通道模块的），
     * 所以只需要传控制 socket。 */
    cl_log_stage(6, 8, "向手机请求画面参数（分辨率 / 帧率 / 编码格式）");
    stage_set(5, UI_ST_ACTIVE, L"协商中…");
    load_preset();
    cl_log("   本次请求 %dx%d @ %d 帧",
           g_presets[g_preset].w, g_presets[g_preset].h, g_presets[g_preset].fps);
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

        /* 顺手把 CMD 通道也收一遍。
         * 只在 select 说「真有数据」时才去读：
         *   · 没数据时一分钱不花（不会拖慢收帧）；
         *   · 真有数据时给 500 毫秒把整条报文收完 ——
         *     报文头 8 字节 + 报文体，从本地回环过来是微秒级的，
         *     给 500 毫秒是为了万一只到了一半，不至于把流读散。 */
        {
            fd_set rf;
            struct timeval tv0;
            FD_ZERO(&rf);
            FD_SET(g_cmd, &rf);
            tv0.tv_sec = 0;
            tv0.tv_usec = 0;
            if (select((int)g_cmd + 1, &rf, NULL, NULL, &tv0) > 0) {
                unsigned long cmid = 0;
                int clen = 0;
                if (cl_recv_cmd(g_cmd, &cmid, g_cmd_rx,
                                (int)sizeof(g_cmd_rx), &clen, 500) == CL_OK) {
                    g_cmd_msgs++;
                    cl_log("CMD 通道收到消息 id = 0x%X（%d 字节，累计 %d 条）",
                           cmid, clen, g_cmd_msgs);
                    if (clen > 0)
                        cl_log_hex("  CMD 内容", g_cmd_rx, clen < 32 ? clen : 32);
                }
            }
        }

        r = cl_recv_video(g_vid, &ts, &vt, g_rx, g_rxcap, &len, 20);
        if (r == CL_OK) {
            if (g_frames == 0) {
                cl_log_stage(7, 8, "开始接收画面 —— 后面就看画面能不能出来了");
                stage_set(5, UI_ST_DONE, L"已协商");
                stage_set(6, UI_ST_ACTIVE, L"已收 0 帧");
                cl_log("   第一帧 %d 字节", len);
                /* 首帧的头几十个字节是最关键的证据：能直接看出
                 * 是 JPEG 还是 H.264、什么 profile、怎么切帧。 */
                cl_log_hex("首帧头部", g_rx, (len < 64) ? len : 64);
            }
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
            /* 每 50 帧往日志里记一行，用来判断"到底有没有在动、丢多少" */
            /* 每 10 帧更新一次界面上的「接收画面」那一行与底部统计 ——
             * 数字在动，就说明程序活着，而不是卡死了 */
            if (g_frames % 10 == 0) {
                WCHAR w[40], f[200];
                wsprintfW(w, L"已收 %d 帧", g_frames);
                stage_set(6, UI_ST_ACTIVE, w);
                wsprintfW(f, L"%dx%d · %d 帧/秒 · 已收 %d 帧",
                          g_disp.sw, g_disp.sh, g_fps, g_frames);
                ui_footline(f);
                if (g_hwnd) InvalidateRect(g_hwnd, 0, FALSE);
            }
            if (g_frames % 50 == 0) {
                cl_log("收帧统计: 共 %d 帧 / 解码成功 %d / 显示 %d / 连续失败 %d / 约 %d 帧每秒",
                       g_frames, g_decoded, g_shown, g_miss, g_fps);
            }
        } else if (r == CL_ERR_TIMEOUT) {
            continue;                      /* 正常：暂时没数据 */
        } else {
            WCHAR t[200];
            cl_log_step("接收画面", 0, "收帧通道中断");
            cl_log("   错误码 %d，已经收了 %d 帧（解码成功 %d）", r, g_frames, g_decoded);
            wsprintfW(t, L"收帧中断（错误 %d），已收 %d 帧", r, g_frames);
            set_status(t);
            return;
        }
    }
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmdline, int show)
{
    WNDCLASSW wc;
    RECT rc;
    MSG msg;

    (void)hp; (void)cmdline; (void)show;

    /* ══════════════════════════════════════════════════════════════════════
     *  第一优先：【把窗口弄出来】。这一段绝不能碰任何文件。
     *
     *  ⚠⚠ 这是用一趟白跑换来的结论 ⚠⚠
     *
     *  车机上的实测：程序确实启动了（日志是它写的），但窗口一直没出现。
     *  日志只留下 3 行 —— 时间戳 [0.088] [0.095] [0.101]，
     *  而日志头一共 5 行，第 3 行和第 4 行之间【没有任何代码】。
     *  逐条排除之后只剩一个解释：
     *    第 4 次往 U 盘 WriteFile 时卡住或失败了
     *    —— 程序从 U 盘运行、又往 U 盘写日志，老 WinCE 的 U 盘驱动在这里很容易出问题。
     *
     *  而当时的顺序是「先写日志 → 再建窗口」，于是写盘一出问题，
     *  窗口的创建【永远轮不到】，用户看到的就是「点了完全没反应」。
     *
     *  现在把顺序倒过来：窗口先出来，之后再碰盘。
     *  盘再怎么出问题，屏幕上至少已经有东西 —— 用户能看见、能判断、能拍给我。
     * ══════════════════════════════════════════════════════════════════════ */
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

    /* ⚠⚠ 扩展样式必须是 0 —— 和骨架、和体检程序完全一致。
     *
     *   原来这里写了 WS_EX_TOPMOST（0x8），本意是防止被车机桌面盖住。
     *   但车机实测结果是：
     *     · 骨架（CreateWindowExW(0, …)）      → 窗口好好留着
     *     · 体检程序（CreateWindowExW(0, …)）   → 12 项全过，窗口好好留着
     *     · 主程序（WS_EX_TOPMOST）             → 闪一下就没了
     *   「置顶窗口」在有些车机外壳上会被当成异常窗口处理甚至直接关掉。
     *   实测优先于理论：改回 0。 */
    g_hwnd = CreateWindowExW(0, L"CarLifeView", L"XianyCar+互联",
                             WS_POPUP | WS_VISIBLE,
                             0, 0, rc.right, rc.bottom,
                             0, 0, hi, 0);
    if (!g_hwnd) {
        /* 窗口都建不出来，日志多半也指望不上，直接弹窗 —— 这是唯一还能用的通道 */
        MessageBoxW(0,
            L"窗口创建失败。\n"
            L"请把这句话拍下来发给开发者，这一条就够了。",
            L"XianyCar+互联", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOW);
    /* 这一次 UpdateWindow 会让 WM_PAINT 走【兜底画法】——
     * 此时 g_ready 还是 0，用的全是骨架验证过的 API。
     * 于是【屏幕在任何新代码之前就已经有东西了】。 */
    UpdateWindow(g_hwnd);

    /* ══ 主线程到此为止：下面只建界面、然后跑消息循环，【一律不碰文件】══
     *
     * ⚠⚠ 这是用两趟白跑换来的教训 ⚠⚠
     *
     * 原来 cl_log_open() 和那几行启动日志都跑在主线程上。车机上实测的结果是：
     *   日志永远只留下 3 行（时间戳 81 / 88 / 94 毫秒），窗口一次都没画出来。
     * 逐条排除后只剩一个解释：往 U 盘写日志时，第 4 次 WriteFile 卡死或崩掉了。
     * 而它跑在主线程上 —— 于是消息循环【从来没跑起来】，
     * 窗口虽然建出来了，却永远等不到一次 WM_PAINT，屏幕上一片空白。
     *
     * 所以现在：窗口和界面归主线程，【所有文件操作都搬进会话线程】。
     * 那边再怎么卡，主线程照样活着、窗口照样会重画。
     * 另：日志目录也改成车机内部存储优先，U 盘放到最后兜底。 */

    /* 收帧缓冲（纯内存）*/
    g_rxcap = RX_CAP;
    g_rx = (unsigned char *)malloc(g_rxcap);
    if (!g_rx) {
        MessageBoxW(0, L"内存不够，收帧缓冲分配失败。",
                    L"XianyCar+互联", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* ⚠⚠ 会话线程【必须在 ui_init 之前启动】。
     *
     *   实锤：上一版窗口闪一下就没了、而且【一个日志文件都没有】——
     *   因为会话线程（也就是打开日志的那一段）排在 ui_init 后面，
     *   界面一崩，日志就永远没机会被创建，我们手上一点线索都没有。
     *
     *   现在反过来：先把会话线程拉起来（它第一件事就是开日志、写一行
     *   「窗口已显示」），之后再建界面。这样界面哪怕再崩，
     *   日志也已经躺在盘上，明确写着走到哪一步了。
     *
     *   ⚠ 界面初始化的每一步之间也写一行日志 —— 崩在哪一步一目了然。
     *     这几行日志写在主线程上是有意的：万一写盘卡住，
     *     屏幕上【已经】有兜底画面了（那一步在更前面），窗口不会全黑。 */
    {
        DWORD tid = 0;
        g_hthr = CreateThread(0, 0, session_thread, 0, 0, &tid);
        if (!g_hthr) {
            /* 绝不退回「在主线程上跑会话」：会话里有 120 秒等待，一放主线程
             * 消息循环就到不了，窗口又会变成一片空白 —— 那正是踩过两次的坑。 */
            set_status(L"线程创建失败：车机资源不足，请重启车机后再试");
        }
    }
    /* ⚠ 等会话线程把启动日志写完（最多 5 秒）。
     *   不等的话：① 那几行会因为句柄还没打开被静默丢掉（恰好丢掉最关键的线索）；
     *             ② 更重要的是会和会话线程在同一时刻抢着写同一个文件。 */
    {
        int k;
        for (k = 0; k < 50 && !g_log_ready; k++)
            Sleep(100);
    }
    g_boot_step = 1;
    cl_log("界面 1/4 开始建字体（CreateFontIndirectW ×5）");
    cl_log_sync();   /* ⚠ 每一步之后都强制刷盘：万一下一步就崩，
                      *   这一行必须已经在盘上，否则日志等于白记。 */
    ui_init(g_hwnd);
    g_boot_step = 2;
    cl_log("界面 2/4 字体和画刷已建好");
    cl_log_sync();
    sync_button_labels();
    ui_headline(L"用 USB 线把手机连到车机",
                L"然后在手机上打开「USB 调试」并点「允许」");
    stage_set(0, UI_ST_ACTIVE, L"正在准备…");
    g_boot_step = 3;
    cl_log("界面 3/4 开始画完整界面（已全部改用 FillRect 一类的验证过的 API）");
    cl_log_sync();
    InvalidateRect(g_hwnd, 0, FALSE);
    UpdateWindow(g_hwnd);            /* 现在才是新界面（走 ui_paint_connect）*/
    g_boot_step = 4;
    cl_log("界面 4/4 完整界面绘制完成");
    cl_log_sync();


    /* 留在窗口里，让用户看清最后的状态 */
    while (!g_quit && GetMessageW(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    cl_log("");
    cl_log("=== 收尾 ===");
    cl_log("总计: 收到 %d 帧 / 解码成功 %d / 显示 %d", g_frames, g_decoded, g_shown);
    cl_log("日志文件: %s", cl_log_path());
    cl_log("请把【整个文件夹】拷回来（日志 + video-raw.bin）");

    if (g_adb_ok) adbp_stop();          /* 关掉转发线程并释放 ADB 设备 */
    if (g_cmd   != INVALID_SOCKET) closesocket(g_cmd);
    if (g_vid   != INVALID_SOCKET) closesocket(g_vid);
    if (g_touch != INVALID_SOCKET) closesocket(g_touch);
    disp_free(&g_disp);
    ui_free();
    if (g_rx) free(g_rx);
    cl_log_close();
    return 0;
}
