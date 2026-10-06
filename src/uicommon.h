/* uicommon.h — 两个探测程序共用的界面/落盘小工具
 *
 * 都是 static 函数，放在头文件里由各程序各自编译一份，
 * 这样不必额外建一个模块、也不会产生重复符号。
 *
 * 为什么需要这些：
 *  1) 探测输出可能超出一屏，而车机上没有滚动条 —— 必须能翻页，
 *     否则用户在车上根本看不到后半段结果。
 *  2) 拍照容易糊、容易漏行；写成文本文件拷出来才可靠。
 *     用 UTF-16LE + BOM，Windows 记事本可直接正确打开。
 */
#ifndef UICOMMON_H
#define UICOMMON_H

#include <windows.h>

#define UIC_CAP 12288

/* 把文本写成 UTF-16LE 带 BOM 的文件。失败就静默返回（不阻塞主流程）。 */
static void uic_dump_file(const WCHAR *path, const WCHAR *text)
{
    static const unsigned char bom[2] = { 0xFF, 0xFE };
    HANDLE h;
    DWORD wrote = 0;

    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    WriteFile(h, bom, 2, &wrote, NULL);
    WriteFile(h, text, (DWORD)(wcslen(text) * sizeof(WCHAR)), &wrote, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
}

/* 报告缓冲里有多少行 */
static int uic_count_lines(const WCHAR *s)
{
    int n = 1;
    int i;
    for (i = 0; s[i]; i++)
        if (s[i] == L'\n')
            n++;
    return n;
}

/* 返回第 n 行（0 = 第一行）的起始指针 */
static const WCHAR *uic_line_n(const WCHAR *s, int n)
{
    int i, c = 0;
    if (n <= 0)
        return s;
    for (i = 0; s[i]; i++) {
        if (s[i] == L'\n') {
            c++;
            if (c == n)
                return s + i + 1;
        }
    }
    return s + i;   /* 到头了，返回末尾空串 */
}

/* 按当前字体高度算一屏能放几行 */
static int uic_lines_per_page(HDC dc, int client_h)
{
    TEXTMETRICW tm;
    int per;

    if (GetTextMetricsW(dc, &tm) && tm.tmHeight > 0)
        per = (client_h - 8) / tm.tmHeight;
    else
        per = (client_h - 8) / 16;
    if (per < 1)
        per = 1;
    return per;
}

#endif /* UICOMMON_H */
