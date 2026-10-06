/* adb.h — ADB 主机协议（自足实现，用于 CarLife 有线模式的端口转发）
 *
 * 为什么需要它：CarLife 有线模式的标准架构是「车机做 ADB 主机 + adb forward」，
 * 手机上的 CarLife 服务在手机本地回环端口上监听，车机通过转发连过去。
 * 这台车机的系统里已经装了 ADB 的 USB 客户端驱动（由 EasyConnected 实证），
 * 应用层用一个 CreateFile("ADB1:") 就能打开它。
 *
 * 重要：ADB 协议的所有字段都是【小端】，而 CarLife 协议是【大端】——
 * 两套混在一起写极易出错，所以本文件里每个打包/解包函数都注明字节序。
 *
 * 读写通过函数指针传入，好处是车机上接设备、电脑上接 socket，
 * 同一份协议代码两边都能跑、都能测。
 */
#ifndef ADB_H
#define ADB_H

/* ── 设备读写抽象 ── */
typedef struct {
    /* 返回实际读到的字节数；0 = 超时；<0 = 出错或对端关闭 */
    int (*read)(void *ctx, unsigned char *buf, int len, int timeout_ms);
    /* 返回实际写入字节数；<0 = 出错 */
    int (*write)(void *ctx, const unsigned char *buf, int len);
    void *ctx;
} ADB_IO;

/* ── ADB 命令字（按小端存放的 4 个 ASCII 字符）── */
#define ADB_CNXN 0x4E584E43u   /* "CNXN" */
#define ADB_AUTH 0x48545541u   /* "AUTH" */
#define ADB_OPEN 0x4E45504Fu   /* "OPEN" */
#define ADB_OKAY 0x59414B4Fu   /* "OKAY" */
#define ADB_CLSE 0x45534C43u   /* "CLSE" */
#define ADB_WRTE 0x45545257u   /* "WRTE" */

#define ADB_VERSION      0x01000000u
#define ADB_AUTH_TOKEN       1u
#define ADB_AUTH_SIGNATURE   2u
#define ADB_AUTH_RSAPUBKEY   3u

/* 我们请求的最大载荷。现代 adbd 能接受更大的值，最终取双方较小者。 */
#define ADB_MAXDATA_REQ  (256 * 1024)

#define ADB_MAX_CHAN 8

typedef struct {
    int  used;
    int  remote_id;
    char service[64];
    unsigned char *rx;      /* 该通道的接收缓冲（按需增长） */
    int  rx_cap;
    int  rx_len;            /* 已缓存字节数 */
    int  rx_head;           /* 已取走的位置 */
    int  closed;            /* 对端关了这条通道 */
} ADB_CHAN;

typedef struct {
    ADB_IO        io;
    unsigned int  maxdata;      /* 协商后的最大载荷 */
    unsigned int  next_id;
    ADB_CHAN      ch[ADB_MAX_CHAN];
    unsigned char tmp[24];      /* 组包头用 */
} ADB;

/* 连接 + 认证。成功返回 0。
 * 失败返回负数：-1 = 传输错，-2 = 协议错，-3 = 认证被拒。 */
int  adb_connect(ADB *a, ADB_IO io);

/* 打开一条转发：service 形如 "tcp:7240"。成功返回通道号（>=0），失败 <0。 */
int  adb_open(ADB *a, const char *service);

/* 经由通道发数据。返回 0 成功，<0 失败。 */
int  adb_send(ADB *a, int chan, const unsigned char *data, int len);

/* 收一轮并分发到各通道。返回处理的包数，<0 传输错。 */
int  adb_pump(ADB *a, int timeout_ms);

/* 从通道缓冲取数据。返回取到的字节数，0 = 暂时没有，<0 = 通道已关。 */
int  adb_recv(ADB *a, int chan, unsigned char *out, int cap);

/* 通道是否已被对端关闭 */
int  adb_chan_closed(ADB *a, int chan);

/* 协商后的最大载荷（诊断用） */
unsigned int adb_maxdata(const ADB *a);

#endif
