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

    /* ⑥ 结论 */
    hw_line(L"");
    hw_line(L"── 结论 ──");
    if (found_usbnet) {
        hw_line(L"  ★ 发现疑似 USB 网卡驱动 —— USB 直连路线有戏。");
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
