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
    ADB_CHAN      ch[ADB_MAX_CHAN];
    unsigned char tmp[24];      /* 组包头用 */

    /* ── 收包进度（跨调用保留）─────────────────────────────────────────
     * ⚠⚠ 这一组字段是「防错位」的关键，别删、也别在收包中途清零：
     *
     *   设备是【字节流】，一个包常常分几次才到齐。如果读包头读到一半
     *   就超时，然后把已经读到的字节丢掉，后面所有包就【永久错位】了 ——
     *   表象是 magic 校验失败，recv_msg 返回 -2。
     *
     *   现场日志里正是这个形状：
     *     [11.495] [握手] 等手机应答超时
     *     [13.530] [握手] 已发 CNXN，等手机应答
     *     [14.134] [握手] 读设备失败  (0x2)      ← magic 对不上，流已经错位
     *
     *   所以：超时就把进度【留着】，下次调用接着读，一个字节都不丢。 */
    unsigned char  hdr[24];     /* 包头累积 */
    int            hdr_have;
    unsigned char *pay;         /* 包体累积（按需增长一次，之后复用） */
    int            pay_cap;
    int            pay_have;
    int            pay_need;    /* >0 表示「包体还没收完，别去读包头」 */
    unsigned int   pend_cmd, pend_a0, pend_a1;   /* 上面那个包的头信息 */

    /* 这条 ADB 连接是否已经握手过。
     * ⚠ 为什么必须有它：车机上那个设备句柄【关不掉】（读线程阻塞在 ReadFile 里，
     *   关一个正在被读的句柄是未定义行为），所以我们改成【整机只开一次、
     *   连接一直留着】。既然连接还活着，就绝不能再发一次 CNXN ——
     *   那会让对端把连接整个重置。新一轮开始时只回收通道即可（见 adb_connect）。 */
    int            connected;
} ADB;

/* 握手追踪钩子。
 * what 是固定短语（纯 ASCII），a/b 是两个数字（命令字、长度、错误码…）。
 * 故意不做 printf 式的变参：省掉在一行日志里再引一次 CRT 格式化的风险，
 * 车机上那一次崩溃就是这么来的（见 cllog.c 里的说明）。
 * 装到 cl_log 上，握手走到哪一步、为什么停下，日志里就一目了然；
 * 电脑上的主机测试不装它，行为一点不变。 */
void adb_set_trace(void (*fn)(const char *what, unsigned int a, unsigned int b));

/* 把这条连接的状态忘掉（通道全部释放、握手标记清零）。
 * 什么时候用：设备句柄【换了一个新的】，旧连接已经不存在了 ——
 * 这时必须让它重新走一遍完整握手，绝不能以为还连着。
 * adbp_start 里判断「这次是新开的设备」后就会调它。 */
void adb_forget(ADB *a);

/* 连接 + 认证。成功返回 0。
 * 失败返回负数，各码含义要分清楚 —— 现场屏幕/日志上显示的就是它：
 *   -1 = 传输错
 *   -2 = 协议错（收到的不是 ADB，或字段对不上）
 *   -3 = 认证被拒（手机上没点「允许」）
 *   -4 = 超时：包发出去了，手机一直没回 —— 多半是手机上没开 USB 调试，
 *        或者「允许 USB 调试」的弹框没点
 *   -5 = 设备读写失败：ADB 设备句柄拿到了，但读/写立刻报错
 *        （以前这些都塌成同一个 -1，现场根本分不清是「手机不理我」
 *         还是「设备不通」；现在分开了，一眼能看出来） */
int  adb_connect(ADB *a, ADB_IO io);

/* 打开一条转发：service 形如 "tcp:7240"。成功返回通道号（>=0），失败 <0。 */
int  adb_open(ADB *a, const char *service);

/* 经由通道发数据。返回 0 成功，<0 失败。 */
int  adb_send(ADB *a, int chan, const unsigned char *data, int len);

/* 收一轮并分发到各通道。返回处理的包数（0 = 本轮没数据，正常），<0 传输错。 */
int  adb_pump(ADB *a, int timeout_ms);

/* 从通道缓冲取数据。返回取到的字节数，0 = 暂时没有，<0 = 通道已关。 */
int  adb_recv(ADB *a, int chan, unsigned char *out, int cap);

/* 用 ADB 的 shell 服务执行一条命令，收集它的输出。
 * cmd 形如 "pm list packages"。out 里放命令输出（超长会截断）。
 * 成功返回 0，失败返回负数。
 *
 * 为什么要这个：手机上的 CarLife/Jovi InCar 不一定会自己启动 ——
 * 如果它没在跑，手机本地 7240 端口就没人监听，转发会被直接拒。
 * 所以车机这边要能主动把手机端拉起来（EasyConnected 是靠往手机推一个
 * carman 程序并执行来做到这件事的，它的字符串里就有这条记录）。 */
int  adb_run_shell(ADB *a, const char *cmd, char *out, int cap, int timeout_ms);

/* 从 "pm list packages" 的输出里找出手机端 CarLife/Jovi InCar 的包名。
 * 找到返回 0 并把包名写进 out；没找到返回 -1。
 * 单独拎出来是为了能在电脑上直接测（纯字符串处理，不碰设备）。 */
int  adb_find_carlife_pkg(const char *pm_output, char *out, int cap);

/* 主动关掉一条通道（本地连接断开时调用） */
int  adb_close_chan(ADB *a, int chan);

/* 通道是否已被对端关闭 */
int  adb_chan_closed(ADB *a, int chan);

/* 协商后的最大载荷（诊断用） */
unsigned int adb_maxdata(const ADB *a);

#endif
