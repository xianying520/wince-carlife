/* test_integration.c — 全链路集成测试
 *
 *   假手机(CarLife) ← 假 adbd(tcp 转发) ← 真实转发器 ← 真实 CarLife 客户端 → JPEG 解码
 *
 * 为什么需要它：零件各自都测过了 —— carlife.c 的字节编码逐字节核对过、
 * 转发器的事件循环端到端跑过 —— 但【两者装在一起】从没跑过。端口怎么接、
 * 时序对不对、两边的阻塞方式会不会互相卡住，这些只有合起来跑才看得出来。
 *
 * 除掉车机的 GDI 画面和真机 USB，这条链路和现场跑的完全是同一份代码。
 */
#include "clhost.h"
#include "hostplat.h"
#include "adbproxy.h"
#include "adb.h"
#include "carlife.h"
#include "third_party/nanojpeg.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static int g_devfd = -1;

static int dev_read(void *ctx, unsigned char *buf, int len, int timeout_ms)
{
    int fd = *(int *)ctx;
    fd_set rf;
    struct timeval tv;
    int r;

    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    r = select(fd + 1, &rf, NULL, NULL, &tv);
    if (r == 0) return 0;
    if (r < 0)  return -1;
    r = (int)recv(fd, buf, (size_t)len, 0);
    if (r == 0) return -1;
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    return r;
}

static int dev_write(void *ctx, const unsigned char *buf, int len)
{
    int fd = *(int *)ctx, off = 0;
    while (off < len) {
        int r = (int)send(fd, buf + off, (size_t)(len - off), 0);
        if (r <= 0) return -1;
        off += r;
    }
    return off;
}

static int connect_to(int port, int timeout_ms)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv;

    if (fd < 0) return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(0x7F000001UL);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

int main(int argc, char **argv)
{
    static const char *svc[3] = { "tcp:7240", "tcp:8240", "tcp:9340" };
    unsigned short lports[3];
    char reason[256];
    struct sockaddr_in sa;
    ADB_IO io;
    SOCKET cmd = -1, vid = -1, touch = -1;
    int r, fd, i;

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    njInit();                       /* nanojpeg 要求先初始化 */

    if (argc < 2) { printf("用法: %s <假adbd端口>\n", argv[0]); return 2; }

    /* ① 设备这一侧：一条普通 socket 顶替 USB 设备 */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons((unsigned short)atoi(argv[1]));
    sa.sin_addr.s_addr = htonl(0x7F000001UL);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        printf("DEV_CONNECT=失败\n");
        return 1;
    }
    g_devfd = fd;
    io.read  = dev_read;
    io.write = dev_write;
    io.ctx   = &g_devfd;

    /* ② 起转发器 —— 和车机上 adbp_start 走的是同一个核心 */
    r = adbp_start_with_io(io, svc, 3, lports, reason, (int)sizeof(reason));
    printf("ADBP_START=%d\n", r);
    if (r != 0) { printf("REASON=%s\n", reason); return 1; }
    printf("LPORT_CMD=%u\n", lports[0]);
    printf("LPORT_VID=%u\n", lports[1]);
    printf("LPORT_TOUCH=%u\n", lports[2]);

    /* ③ 控制通道 + 握手（这里开始全是真的 carlife.c） */
    cmd = connect_to((int)lports[0], 5000);
    printf("CMD_CONNECT=%s\n", cmd >= 0 ? "成功" : "失败");
    if (cmd < 0) goto done;

    {
        int match = -1;
        unsigned long reply = 0;
        int st = cl_handshake(cmd, &match, &reply);
        printf("HANDSHAKE=%d\n", st);
        printf("HANDSHAKE_MATCH=%d\n", match);
        if (st != CL_OK) goto done;
    }

    /* ④ 视频通道 + 视频初始化 */
    vid = connect_to((int)lports[1], 5000);
    printf("VID_CONNECT=%s\n", vid >= 0 ? "成功" : "失败");
    if (vid < 0) goto done;

    cl_resend_version(cmd);
    printf("ENC_INIT=%d\n", cl_send_video_encoder_init(cmd, 32, 16, 5));
    printf("ENC_START=%d\n", cl_send_video_encoder_start(cmd));
    printf("ENC_JPEG=%d\n", cl_send_video_encoder_jpeg(cmd));

    /* ⑤ 触摸通道 */
    touch = connect_to((int)lports[2], 5000);
    printf("TOUCH_CONNECT=%s\n", touch >= 0 ? "成功" : "失败");
    if (touch >= 0) cl_resend_version(cmd);

    /* ⑥ 收视频帧并解码 —— 这是现场"看到画面"的完整路径 */
    {
        static unsigned char frame[512 * 1024];
        static unsigned char decoded[512 * 1024];
        unsigned long ts = 0, vtype = 0;
        int len = 0, got = 0;

        for (i = 0; i < 40 && !got; i++) {
            r = cl_recv_video(vid, &ts, &vtype, frame, (int)sizeof(frame),
                              &len, 800);
            if (r == CL_OK && len > 0) {
                printf("FRAME_LEN=%d\n", len);
                printf("FRAME_HEAD=%02X%02X\n", frame[0], frame[1]);
                printf("FRAME_IS_JPEG=%s\n",
                       (frame[0] == 0xFF && frame[1] == 0xD8) ? "是" : "否");
                got = 1;
            }
        }
        printf("FRAME_GOT=%d\n", got);

        if (got) {
            int nj = njDecode(frame, len);
            printf("NJDECODE=%d\n", nj);
            (void)decoded;
            if (nj == 0) {
                int w = njGetWidth(), h = njGetHeight();
                const unsigned char *img = njGetImage();
                int sz = njGetImageSize();
                printf("DECODED_W=%d\n", w);
                printf("DECODED_H=%d\n", h);
                printf("DECODED_SIZE=%d\n", sz);
                printf("DECODED_COLOR=%d\n", njIsColor());
                printf("DECODED_PX0=%d\n", img ? img[0] : -1);
                printf("DECODED_PXLAST=%d\n", img && sz > 0 ? img[sz - 1] : -1);
                if (img && sz > 0) {
                    int k, all = 1;
                    for (k = 0; k < sz; k++)
                        if (img[k] != 128) { all = 0; break; }
                    printf("DECODED_ALL_128=%d\n", all);
                }
            }
        }
    }

    /* ⑦ 触摸回传（走真实的触摸编码） */
    if (touch >= 0) {
        int st = cl_send_touch_action(touch, 1, 100, 80, 0);
        printf("TOUCH_SEND=%d\n", st);
    }

done:
    {
        unsigned long tx = 0, rx = 0;
        adbp_stats(&tx, &rx);
        printf("STATS_TX=%lu\n", tx);
        printf("STATS_RX=%lu\n", rx);
    }
    printf("ADBP_STATUS=%s\n", adbp_status());
    if (cmd   >= 0) close(cmd);
    if (vid   >= 0) close(vid);
    if (touch >= 0) close(touch);
    adbp_stop();
    njDone();
    close(fd);
    printf("DONE\n");
    return 0;
}
