#include "hwdump.h"

#define HWD_CAP   12288
#define MAX_KEYS  64

static WCHAR g_hw[HWD_CAP];
static int   g_hp;

/* ── 报告缓冲 ── */
static void hw_line(const WCHAR *s)
{
    int i;
    for (i = 0; s[i] && g_hp < HWD_CAP - 3; i++)
        g_hw[g_hp++] = s[i];
    if (g_hp < HWD_CAP - 3) {
        g_hw[g_hp++] = L'\r';
        g_hw[g_hp++] = L'\n';
    }
    g_hw[g_hp] = 0;
}

/* 读一个字符串值。注意：查询返回的 cb 是字节数，且不保证以 0 结尾，
 * 所以必须自己按字符数截断并补 0 —— 直接 buf[cb/2-1]=0 在 cb 为奇数
 * 或字符串本就没有结尾时会把最后一个有效字符吃掉。 */
static void read_str(HKEY root, const WCHAR *path, const WCHAR *val,
                     const WCHAR *label)
{
    HKEY  k;
    WCHAR buf[256], t[512];
    DWORD cb = sizeof(buf) - sizeof(WCHAR), type = 0;

    if (RegOpenKeyExW(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return;

    buf[0] = 0;
    if (RegQueryValueExW(k, val, 0, &type, (BYTE *)buf, &cb) == ERROR_SUCCESS
        && cb >= sizeof(WCHAR)) {
        int n = (int)(cb / sizeof(WCHAR));
        if (n > 255) n = 255;
        if (n > 0 && buf[n - 1] == 0)
            n--;                       /* 本来就有结尾，不把 0 算进长度 */
        buf[n] = 0;
        wsprintfW(t, L"  %s = %s", label, buf);
        hw_line(t);
    }
    RegCloseKey(k);
}

/* 枚举子键名。若 collect 非空，同时把名字收集起来供后面判断用。 */
static int enum_subkeys(HKEY root, const WCHAR *path, const WCHAR *title,
                        WCHAR collect[][64], int collect_max)
{
    HKEY  k;
    WCHAR t[256];
    int   n = 0;

    wsprintfW(t, L"── %s ──", title);
    hw_line(t);

    if (RegOpenKeyExW(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS) {
        hw_line(L"  （不存在或打不开）");
        return 0;
    }

    for (;;) {
        WCHAR name[64];
        DWORD nl = 64;
        if (RegEnumKeyExW(k, n, name, &nl, 0, 0, 0, 0) != ERROR_SUCCESS)
            break;

        if (collect && n < collect_max) {
            int i;
            for (i = 0; name[i] && i < 63; i++)
                collect[n][i] = name[i];
            collect[n][i] = 0;
        }

        wsprintfW(t, L"  %s", name);
        hw_line(t);

        n++;
        if (n >= MAX_KEYS) {
            hw_line(L"  ...（还有更多，已省略）");
            break;
        }
    }
    if (n == 0)
        hw_line(L"  （空的）");

    RegCloseKey(k);
    return n;
}


/* 读一个字符串值到调用者缓冲。成功返回长度（字符数），失败返回 -1。
 * 与 read_str 的区别：这个不直接输出，而是把值交回去供判断用。 */
static int read_val(HKEY root, const WCHAR *path, const WCHAR *val,
                    WCHAR *out, int cap)
{
    HKEY  k;
    DWORD cb, type = 0;
    int   n;

    if (cap <= 0) return -1;
    out[0] = 0;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return -1;

    cb = (DWORD)(cap * sizeof(WCHAR) - sizeof(WCHAR));
    if (RegQueryValueExW(k, val, 0, &type, (BYTE *)out, &cb) != ERROR_SUCCESS
        || cb < sizeof(WCHAR)) {
        RegCloseKey(k);
        return -1;
    }
    n = (int)(cb / sizeof(WCHAR));
    if (n > cap - 1) n = cap - 1;
    if (n > 0 && out[n - 1] == 0) n--;
    out[n] = 0;
    RegCloseKey(k);
    return n;
}

/* 某个键存不存在（只判断存在性，不读值） */
static int key_exists(const WCHAR *path)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    RegCloseKey(k);
    return 1;
}

/* has_ci 的实现在本函数之后，先声明一下。
 * （上一版这里漏了声明，编译器当成隐式声明，随后又把 static 定义
 *   当成"与前面不一致"而报错 —— 一个前置声明就解决了。） */
static int has_ci(const WCHAR *hay, const WCHAR *needle);

/* ═══════════════════════════════════════════════════════════════════
 * ADB 能力探测  —— 这是传输层的生死线
 *
 * 背景：本方案要走「车机做 ADB 主机 + adb forward 端口转发」，手机上的
 * CarLife 服务在本地回环端口上监听，车机通过转发连过去。这正是 CarLife
 * 有线模式的标准架构。
 *
 * 它成立的前提是：车机系统里装了 ADB 的 USB 客户端驱动，并且应用层能用
 * CreateFile(L"ADB1:", ...) 打开它。这不是猜测 —— 这台车机上装的
 * EasyConnected 就是这么干的，它解包后的字符串里明确有
 *     CreateFile ADB1: OK
 *     Drivers\USB\ClientDrivers\ADB_Driver
 *     AdbForwardServer::StartAdbForward
 * 详见 docs/传输路线重大修正-ADB直连.md
 *
 * ⚠ 注意：ADB1: 这类设备节点**只有在手机插上并处于 ADB 模式时**才会被加载。
 *   所以本程序要跑两次对比：不插手机一次，插上手机一次。
 * ═══════════════════════════════════════════════════════════════════ */
static void hw_probe_adb(void)
{
    static const WCHAR *cands[] = {
        L"ADB1:", L"ADB0:", L"ADB2:", L"tADB1:", L"ADB:"
    };
    static const WCHAR *drvkeys[] = {
        L"Drivers\\USB\\ClientDrivers\\ADB_Driver",
        L"Drivers\\USB\\ClientDrivers\\ADB_Class",
        L"Drivers\\USB\\ClientDrivers\\Adb",
        L"Drivers\\USB\\LoadClients\\Default\\Default\\255_66_1",
        L"Drivers\\USB\\LoadClients\\Default\\Default\\255_66_1\\ADB_Driver",
    };
    WCHAR t[512], nm[128];
    int   i, drv_found = 0, opened_at = -1, live_adb = 0;

    hw_line(L"");
    hw_line(L"════════════════════════════════════════════");
    hw_line(L"【最关键的一节】ADB 直连能力探测");
    hw_line(L"════════════════════════════════════════════");
    hw_line(L"为什么要测这个：本方案走「车机做 ADB 主机 + 端口转发」，");
    hw_line(L"不需要 USB 网卡。前提是车机里有 ADB 的 USB 驱动。");
    hw_line(L"");

    /* ① 驱动本身在不在 */
    hw_line(L"① 注册表里有没有装 ADB 的 USB 驱动：");
    for (i = 0; i < (int)(sizeof(drvkeys) / sizeof(drvkeys[0])); i++) {
        if (key_exists(drvkeys[i])) {
            drv_found++;
            wsprintfW(t, L"   ★ 存在: HKLM\\%s", drvkeys[i]);
            hw_line(t);
        }
    }
    if (!drv_found)
        hw_line(L"   （一个都没找到）");

    /* ② 当前已加载的驱动里，有没有名字带 ADB 的 —— 顺便把所有设备名列出来，
     *    这样即使设备名不叫 ADB1: 我们也能看出来它叫什么 */
    hw_line(L"");
    hw_line(L"② 当前已加载的设备（HKLM\\Drivers\\Active 的 Name），");
    hw_line(L"   名字里带 ADB 的就是我们要的：");
    {
        int n = 0;
        for (;;) {
            WCHAR sub[16], path[96];
            wsprintfW(sub, L"%d", n);
            wsprintfW(path, L"Drivers\\Active\\%s", sub);
            if (!key_exists(path)) break;
            if (read_val(HKEY_LOCAL_MACHINE, path, L"Name", nm, 128) > 0) {
                if (has_ci(nm, L"ADB")) {
                    wsprintfW(t, L"   ★★ 设备名: %s   ← 这就是 ADB 通道", nm);
                    live_adb = 1;
                } else {
                    wsprintfW(t, L"      %s", nm);
                }
                hw_line(t);
            }
            n++;
            if (n > 200) break;
        }
        if (n == 0)
            hw_line(L"   （枚举不到，或都是空的）");
    }

    /* ③ 最硬的证据：直接尝试打开设备 */
    hw_line(L"");
    hw_line(L"③ 直接尝试打开 ADB 设备（成功就是决定性证据）：");
    for (i = 0; i < (int)(sizeof(cands) / sizeof(cands[0])); i++) {
        HANDLE h = CreateFileW(cands[i], GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD junk = 0;
            wsprintfW(t, L"   ★★★ 能打开 %s  ← ADB 通道可用！", cands[i]);
            hw_line(t);
            /* 顺手试读一下，看有没有报错（说明设备是活的） */
            if (!DeviceIoControl(h, 0, NULL, 0, NULL, 0, &junk, NULL))
                hw_line(L"       （设备存在，但当前没有数据可读 —— 正常）");
            CloseHandle(h);
            opened_at = i;
            break;
        } else {
            wsprintfW(t, L"      打不开 %s  (错误码 %lu)", cands[i],
                      (unsigned long)GetLastError());
            hw_line(t);
        }
    }

    /* ④ 结论 + 下一步该怎么做 */
    hw_line(L"");
    hw_line(L"── ADB 探测结论 ──");
    if (opened_at >= 0) {
        hw_line(L"  ✅ 这台车机的 ADB 通道可用，本方案的主路线成立。");
        hw_line(L"     手机插着的时候跑出这个结果，就可以进入下一步开发了。");
    } else if (drv_found || live_adb) {
        hw_line(L"  ⚠ 驱动/设备信息在，但设备没打开成功。");
        hw_line(L"     最可能的原因：现在没插手机，或手机没进入 ADB 模式。");
        hw_line(L"     ▶ 请插上手机、在手机上开启「USB 调试」，再跑一次本程序。");
        hw_line(L"     那时 ③ 里应该会出现「能打开 ADB1:」。");
    } else {
        hw_line(L"  ❓ 没找到 ADB 驱动的痕迹。");
        hw_line(L"     两种可能，必须插上手机再跑一次才能区分：");
        hw_line(L"       a) 驱动是插手机时才动态加载的 —— 那就插上再跑；");
        hw_line(L"       b) 这台车机真的没有 ADB 驱动 —— 那 ADB 路线不通，");
        hw_line(L"          退回 USB 网卡路线（看上面第一节的结果）。");
    }
}

/* 大小写不敏感的子串查找（只处理 ASCII，驱动名都是 ASCII） */
static int has_ci(const WCHAR *hay, const WCHAR *needle)
{
    int i, j;
    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j]; j++) {
            WCHAR a = hay[i + j], b = needle[j];
            if (a >= L'a' && a <= L'z') a -= 32;
            if (b >= L'a' && b <= L'z') b -= 32;
            if (a != b) break;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}

void hw_dump(void)
{
    WCHAR  names[MAX_KEYS][64];
    WCHAR  t[512];
    int    n, i, found_usbnet = 0;

    g_hp = 0;
    g_hw[0] = 0;

    hw_line(L"══ 车机硬件 / 网络能力清点 ══");
    hw_line(L"（全部直接读注册表，不需要任何额外 DLL）");
    hw_line(L"");

    /* ① 网络驱动清单 —— 有没有 USB 网卡就看这里 */
    hw_line(L"【最重要】HKLM\\Comm 下的网络驱动：");
    hw_line(L"  如果这里出现 RNDIS 之类，说明车机支持 USB 网卡，");
    hw_line(L"  手机开「USB 网络共享」就能让车机拿到 IP。");
    hw_line(L"  如果只有 PPP/串口之类，USB 直连这条路就要另想办法。");
    n = enum_subkeys(HKEY_LOCAL_MACHINE, L"Comm", L"HKLM\\Comm",
                     names, MAX_KEYS);

    /* 判断是否出现疑似 USB 网卡驱动 */
    for (i = 0; i < n; i++) {
        if (has_ci(names[i], L"RNDIS") || has_ci(names[i], L"USBNET") ||
            has_ci(names[i], L"USB8023") || has_ci(names[i], L"USBEth") ||
            has_ci(names[i], L"NDIS")) {
            found_usbnet = 1;
            wsprintfW(t, L"  ★ 疑似 USB 网卡驱动: %s", names[i]);
            hw_line(t);
        }
    }

    /* ② USB 客户端驱动（另一条线索） */
    enum_subkeys(HKEY_LOCAL_MACHINE, L"Drivers\\USB\\LoadClients",
                 L"HKLM\\Drivers\\USB\\LoadClients  USB 客户端驱动", 0, 0);

    /* ③ 当前已加载的驱动 —— 插上手机后再看这一节最有价值 */
    enum_subkeys(HKEY_LOCAL_MACHINE, L"Drivers\\Active",
                 L"HKLM\\Drivers\\Active  当前已加载的驱动", 0, 0);

    /* ④ 内置驱动 */
    enum_subkeys(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn",
                 L"HKLM\\Drivers\\BuiltIn  内置驱动", 0, 0);

    /* ⑤ 常见的 IP 配置位置 */
    hw_line(L"");
    hw_line(L"── 网络接口的 IP 配置 ──");
    for (i = 0; i < n; i++) {
        WCHAR path[256];
        wsprintfW(path, L"Comm\\%s\\Parms\\TcpIp", names[i]);
        read_str(HKEY_LOCAL_MACHINE, path, L"IpAddress", L"IpAddress");
        read_str(HKEY_LOCAL_MACHINE, path, L"Subnetmask", L"Subnetmask");
    }

    /* ⑥ ADB 能力探测 —— 现在这是最关键的一节 */
    hw_probe_adb();

    /* ⑦ 结论 */
    hw_line(L"");
    hw_line(L"── 结论 ──");
    if (found_usbnet) {
        hw_line(L"  ★ 发现疑似 USB 网卡驱动 —— 这是【备选】路线（USB 网络共享）。");
        hw_line(L"    请插上手机、打开「USB 网络共享」，再跑一次本程序，");
        hw_line(L"    看上面的本机 IP 是否变成 192.168.42.x 之类。");
    } else {
        hw_line(L"  ⚠ 没发现明显的 USB 网卡驱动。");
        hw_line(L"    这不等于一定不行（驱动名可能叫别的），但如果插手机开");
        hw_line(L"    USB 共享后本机 IP 依然没有变化，就说明确实缺驱动。");
    }
}

const WCHAR *hw_dump_text(void)
{
    return g_hw;
}
