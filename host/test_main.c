/* 在电脑上驱动【真实的协议代码】跑一遍完整流程，对面是 scripts/protocol_test.py
 * 起的假手机。这个程序只负责按顺序调用 carlife.c，逐字节的断言在 Python 那边做。
 */
#include "clhost.h"

#include "carlife.h"

#include <signal.h>

static int fails = 0;

static void ck(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "✅" : "❌", what);
    if (!ok)
        fails++;
}

int main(int argc, char **argv)
{
    SOCKET cmd, vid, touch;
    unsigned long ip = htonl(0x7F000001UL);   /* 127.0.0.1 */
    int match = -1;
    unsigned long reply = 0, ts = 0, vt = 0;
    int r, len = 0;
    static unsigned char rx[65536];

    (void)argc;
    (void)argv;

    /* 输出必须无缓冲：程序是被管道捕获的，而管道下 stdout 是全缓冲的 ——
     * 一旦中途崩溃，缓冲区里的内容会全部丢失，我们就完全看不到它走到哪了。
     * 这个坑已经踩过一次。 */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* 忽略 SIGPIPE。Windows 的 socket 从不产生这个信号，所以忽略它才是
     * 与车机行为对齐；否则一旦往已被对方关闭的 socket 写数据，进程会当场
     * 被信号杀掉（退出码 -13），看起来像"崩溃"，其实是环境差异。 */
    signal(SIGPIPE, SIG_IGN);

    printf("══ 用真实协议代码连假手机 ══\n");

    /* ⓪ 诊断：先用最朴素的方式连一次。
     * 这样能立刻分清是"兼容层(shim)的问题"还是"协议代码的问题" ——
     * 否则只能对着一句"连不上"猜。 */
    {
        int fd;
        struct sockaddr_in sa;
        int r2;
        fd = socket(AF_INET, SOCK_STREAM, 0);
        printf("     [诊断] socket() = %d\n", fd);
        memset(&sa, 0, sizeof(sa));
        sa.sin_family      = AF_INET;
        sa.sin_port        = htons((unsigned short)CL_PORT_CMD);
        sa.sin_addr.s_addr = ip;
        errno = 0;
        r2 = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
        printf("     [诊断] 朴素阻塞 connect() = %d  errno=%d (%s)\n",
               r2, errno, strerror(errno));
        if (fd >= 0)
            close(fd);
        printf("     [诊断] sizeof(long)=%zu  ip=0x%08lx\n", sizeof(long), ip);
    }

    /* ① 控制通道 + 握手 */
    errno = 0;
    cmd = cl_connect(ip, CL_PORT_CMD, 2000);
    printf("     [诊断] cl_connect 返回 %d  errno=%d (%s)\n",
           cmd, errno, strerror(errno));
    ck("连上控制通道 7240", cmd != INVALID_SOCKET);
    if (cmd == INVALID_SOCKET)
        return 1;

    r = cl_handshake(cmd, &match, &reply);
    ck("握手调用成功", r == CL_OK);
    printf("     手机回的消息 ID = 0x%08lx  matchStatus = %d\n", reply, match);
    ck("收到的是版本匹配消息", reply == CL_MSG_PROTOCOL_VERSION_MATCH);
    ck("matchStatus 解析正确（应为 1）", match == 1);

    /* ② 每个通道使用前重发版本（参考实现的要求） */
    r = cl_resend_version(cmd);
    ck("重发协议版本成功", r == CL_OK);

    /* ③ 视频编码器初始化 / 启动 / 切 JPEG —— 都走控制通道 */
    r = cl_send_video_encoder_init(cmd, 480, 272, 15);
    ck("VIDEO_ENCODER_INIT(480x272@15) 已发送", r == CL_OK);

    r = cl_send_video_encoder_start(cmd);
    ck("VIDEO_ENCODER_START 已发送", r == CL_OK);

    r = cl_send_video_encoder_jpeg(cmd);
    ck("VIDEO_ENCODER_JPEG 已发送", r == CL_OK);

    /* ④ 视频通道收帧 */
    vid = cl_connect(ip, CL_PORT_VIDEO, 2000);
    ck("连上视频通道 8240", vid != INVALID_SOCKET);
    if (vid != INVALID_SOCKET) {
        r = cl_recv_video(vid, &ts, &vt, rx, (int)sizeof(rx), &len, 2000);
        ck("收到一帧视频", r == CL_OK);
        printf("     帧长度 = %d  时间戳 = %lu  vtype = %lu\n", len, ts, vt);
        ck("帧长度与假手机发出的一致（141 字节的测试 JPEG）", len == 141);
        if (len >= 2)
            ck("认出是 JPEG", rx[0] == 0xFF && rx[1] == 0xD8);
        printf("     cl_guess_codec 判定 = %ls\n", (const wchar_t *)cl_guess_codec(rx, len));
        closesocket(vid);
    }

    /* ⑤ 触摸通道：两种写法都发一遍，让假手机核对字节 */
    touch = cl_connect(ip, CL_PORT_TOUCH, 2000);
    ck("连上触摸通道 9340", touch != INVALID_SOCKET);
    if (touch != INVALID_SOCKET) {
        ck("触摸 写法A 按下(120,80) 已发送",
           cl_send_touch_action(touch, 0, 120, 80, 0) == CL_OK);
        ck("触摸 写法A 移动(200,150) 已发送",
           cl_send_touch_action(touch, 2, 200, 150, 0) == CL_OK);
        ck("触摸 写法A 抬起(200,150) 已发送",
           cl_send_touch_action(touch, 1, 200, 150, 0) == CL_OK);
        ck("触摸 写法B 按下(120,80) 已发送",
           cl_send_touch_action(touch, 0, 120, 80, 1) == CL_OK);
        ck("硬按键(下一曲 0x10) 已发送", cl_send_hard_key(touch, 0x10) == CL_OK);
        closesocket(touch);
    }

    closesocket(cmd);

    printf("\n%s（本程序侧的检查 %d 项失败）\n",
           fails ? "❌ 有失败项" : "✅ 本程序侧全部通过", fails);
    return fails ? 1 : 0;
}
