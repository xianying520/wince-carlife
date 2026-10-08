/* ui.c — 见 ui.h 的设计原则 */
#include "ui.h"
#include "ui_layout.h"

#include <string.h>

/* ── 阶段名：站在【司机】的角度说，不用系统内部的叫法。
 *    技术细节放右边一列（ui_stage_detail），两列信息分工明确。 ── */
static const WCHAR *g_st_name[UI_STEPS] = {
    L"认到手机（USB）",
    L"启动手机车载系统",
    L"接通控制通道",
    L"与手机协商协议",
    L"接通画面通道",
    L"协商画面规格",
    L"接收画面",
    L"显示画面"
};

static HFONT  g_f_head, g_f_sub, g_f_step, g_f_detail, g_f_bar;
static HBRUSH g_br_bg, g_br_panel;
static HBRUSH g_br_dot[4];
static HPEN   g_pn_dot[4];

static int   g_sw = UI_BASE_W, g_sh = UI_BASE_H;
static int   g_st[UI_STEPS];
static WCHAR g_st_detail[UI_STEPS][72];
static WCHAR g_head1[96], g_head2[96], g_foot[200];
static WCHAR g_btn[3][24];
static int   g_btn_pressed = -1;

/* 按屏宽等比缩放（车机就是 800x480，这里只是留个余量）*/
static int px(int v) { return v * g_sw / UI_BASE_W; }

static void reset_labels(void)
{
    int i;
    static const WCHAR *d0 = L"";
    for (i = 0; i < UI_STEPS; i++) {
        g_st[i] = UI_ST_PENDING;
        g_st_detail[i][0] = 0;
        (void)d0;
    }
    g_head1[0] = 0; g_head2[0] = 0; g_foot[0] = 0;
    {
        static const WCHAR *b0 = L"触摸 A", *b1 = L"480x272", *b2 = L"退出";
        int k;
        for (k = 0; k < 20 && b0[k]; k++) g_btn[0][k] = b0[k]; g_btn[0][k] = 0;
        for (k = 0; k < 20 && b1[k]; k++) g_btn[1][k] = b1[k]; g_btn[1][k] = 0;
        for (k = 0; k < 20 && b2[k]; k++) g_btn[2][k] = b2[k]; g_btn[2][k] = 0;
    }
}

void ui_init(HWND hwnd)
{
    RECT rc;
    int i;
    static const COLORREF st[4] = { 0, 0, 0, 0 };  /* 占位，下面逐个建 */
    COLORREF dotc[4];
    HDC dc;
    RECT scr;

    (void)st;
    if (hwnd) {
        GetClientRect(hwnd, &rc);
        if (rc.right > 0) g_sw = rc.right;
        if (rc.bottom > 0) g_sh = rc.bottom;
    } else {
        scr.left = 0; scr.top = 0;
        scr.right = GetSystemMetrics(SM_CXSCREEN);
        scr.bottom = GetSystemMetrics(SM_CYSCREEN);
        if (scr.right > 0) g_sw = scr.right;
        if (scr.bottom > 0) g_sh = scr.bottom;
    }

    /* 字体：显式给像素高（负值 = 字符高度），
     * 不再靠 DC 的默认字体 —— 原来就是这样，所以字又小又平。
     * 字面用 NULL（走系统默认字面），这是各种 WinCE ROM 上最稳的做法。*/
    dc = GetDC(0);
    g_f_head   = CreateFontW(-px(UI_F_HEAD),   0, 0, 0, 700, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                             0, NULL);
    g_f_sub    = CreateFontW(-px(UI_F_SUB),    0, 0, 0, 400, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                             0, NULL);
    g_f_step   = CreateFontW(-px(UI_F_STEP),   0, 0, 0, 400, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                             0, NULL);
    g_f_detail = CreateFontW(-px(UI_F_DETAIL), 0, 0, 0, 400, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                             0, NULL);
    g_f_bar    = CreateFontW(-px(UI_F_BAR),    0, 0, 0, 400, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                             0, NULL);
    ReleaseDC(0, dc);

    g_br_bg    = CreateSolidBrush(UI_BG);
    g_br_panel = CreateSolidBrush(UI_PANEL);

    dotc[UI_ST_PENDING] = UI_DOT_OFF;
    dotc[UI_ST_ACTIVE]  = UI_AMBER;
    dotc[UI_ST_DONE]    = UI_GREEN;
    dotc[UI_ST_FAIL]    = UI_RED;
    for (i = 0; i < 4; i++) {
        g_br_dot[i] = CreateSolidBrush(dotc[i]);
        g_pn_dot[i] = CreatePen(PS_SOLID, 2, dotc[i]);
    }

    reset_labels();
}

void ui_free(void)
{
    int i;
    if (g_f_head)   DeleteObject(g_f_head);
    if (g_f_sub)    DeleteObject(g_f_sub);
    if (g_f_step)   DeleteObject(g_f_step);
    if (g_f_detail) DeleteObject(g_f_detail);
    if (g_f_bar)    DeleteObject(g_f_bar);
    if (g_br_bg)    DeleteObject(g_br_bg);
    if (g_br_panel) DeleteObject(g_br_panel);
    for (i = 0; i < 4; i++) {
        if (g_br_dot[i]) DeleteObject(g_br_dot[i]);
        if (g_pn_dot[i]) DeleteObject(g_pn_dot[i]);
    }
    memset(&g_f_head, 0, sizeof(g_f_head));
}

void ui_set_view(HWND hwnd, int full) { (void)hwnd; (void)full; }

void ui_stage(int n, int state)
{
    if (n < 0 || n >= UI_STEPS) return;
    g_st[n] = state;
}

void ui_stage_detail(int n, const WCHAR *text)
{
    int k;
    if (n < 0 || n >= UI_STEPS) return;
    if (!text) { g_st_detail[n][0] = 0; return; }
    for (k = 0; k < 70 && text[k]; k++) g_st_detail[n][k] = text[k];
    g_st_detail[n][k] = 0;
}

void ui_stages_reset(void) { reset_labels(); }

void ui_headline(const WCHAR *l1, const WCHAR *l2)
{
    int k;
    g_head1[0] = 0; g_head2[0] = 0;
    if (l1) { for (k = 0; k < 90 && l1[k]; k++) g_head1[k] = l1[k]; g_head1[k] = 0; }
    if (l2) { for (k = 0; k < 90 && l2[k]; k++) g_head2[k] = l2[k]; g_head2[k] = 0; }
}

void ui_footline(const WCHAR *t)
{
    int k;
    g_foot[0] = 0;
    if (!t) return;
    for (k = 0; k < 190 && t[k]; k++) g_foot[k] = t[k];
    g_foot[k] = 0;
}

void ui_button_label(int which, const WCHAR *text)
{
    int k;
    if (which < 0 || which > 2 || !text) return;
    for (k = 0; k < 20 && text[k]; k++) g_btn[which][k] = text[k];
    g_btn[which][k] = 0;
}

/* ── 一条极细分隔线 ── */
static void hr(HDC dc, int x1, int y, int x2, COLORREF c)
{
    RECT r;
    HBRUSH b = CreateSolidBrush(c);
    r.left = x1; r.right = x2; r.top = y; r.bottom = y + 1;
    FillRect(dc, &r, b);
    DeleteObject(b);
}

/* ── 状态圆点：实心=已发生，空心=还没轮到 ── */
static void dot(HDC dc, int cx, int cy, int r, int state)
{
    HGDIOBJ ob, op;
    ob = SelectObject(dc, g_br_dot[state]);
    op = SelectObject(dc, g_pn_dot[state]);
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
}

static void text(HDC dc, HFONT f, const WCHAR *s, RECT *r,
                 COLORREF c, UINT fmt)
{
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, r, fmt);
    SelectObject(dc, of);
}

/* ── 底栏三个按钮的矩形（从右往左排）── */
static void button_rect(int which, const RECT *rc, RECT *out)
{
    int order[3];
    HDC dc;
    int w[3], i, x;

    /* 从右往左：退出、分辨率、触摸 */
    order[0] = UI_BTN_EXIT; order[1] = UI_BTN_RES; order[2] = UI_BTN_TOUCH;

    dc = GetDC(0);
    for (i = 0; i < 3; i++) {
        RECT t; SIZE sz;
        HGDIOBJ of = SelectObject(dc, g_f_bar);
        t.left = 0; t.right = 0; t.top = 0; t.bottom = 0;
        GetTextExtentPointW(dc, g_btn[order[i]], -1, &sz);
        SelectObject(dc, of);
        w[i] = (int)sz.cx + 2 * px(UI_BTN_PAD);
        if (w[i] < px(46)) w[i] = px(46);
    }
    ReleaseDC(0, dc);

    x = rc->right - px(UI_PAD);
    for (i = 0; i < 3; i++) {
        if (order[i] == which) {
            out->right = x;
            out->left  = x - w[i];
            out->top    = rc->bottom - px(UI_BAR_H) + (px(UI_BAR_H) - px(UI_BTN_H)) / 2;
            out->bottom = out->top + px(UI_BTN_H);
            return;
        }
        x -= w[i] + px(UI_BTN_GAP);
    }
    out->left = out->right = out->top = out->bottom = 0;
}

int ui_hit_button(int x, int y, const RECT *rc)
{
    RECT b;
    int i;
    static const int ids[3] = { UI_BTN_TOUCH, UI_BTN_RES, UI_BTN_EXIT };
    if (y < rc->bottom - px(UI_BAR_H)) return UI_BTN_NONE;
    for (i = 0; i < 3; i++) {
        button_rect(ids[i], rc, &b);
        if (x >= b.left && x < b.right && y >= b.top && y < b.bottom)
            return ids[i];
    }
    return UI_BTN_NONE;
}

void ui_view_rect(const RECT *rc, RECT *out)
{
    *out = *rc;
    out->bottom -= px(UI_BAR_H);
}

/* ══════════════ 进度板（还没连上时整屏显示）══════════════ */
void ui_paint_connect(HDC dc, const RECT *rc)
{
    RECT r;
    int i, y, tracks, block, top;
    int pad = px(UI_PAD);

    /* 底 */
    FillRect(dc, rc, g_br_bg);

    /* 顶栏：左边标题，右边提示日志在哪 */
    r = *rc; r.bottom = r.top + px(UI_BAR_H);
    r.left = pad;
    text(dc, g_f_bar, L"CarLife 车机端", &r, UI_MUTED,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    {
        const WCHAR *lg = L"运行日志：carlife-log.txt";
        int n = 0; while (lg[n]) n++;
        r.left = rc->right - pad - px(7) * n / 2;
        text(dc, g_f_bar, lg, &r, UI_RULE,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    hr(dc, pad, rc->top + px(UI_BAR_H), rc->right - pad, UI_RULE);

    /* 内容块整体垂直居中：先算总高 */
    tracks = UI_STEPS * px(UI_STEP_H);
    block  = px(UI_HEAD_H) + px(UI_SUB_GAP) + px(UI_SUB_H)
           + px(UI_TRACK_GAP) + tracks
           + px(UI_DETAIL_GAP) + px(UI_DETAIL_H);
    top = rc->top + px(UI_BAR_H) + (g_sh - 2 * px(UI_BAR_H) - block) / 2;
    if (top < rc->top + px(UI_BAR_H) + px(8)) top = rc->top + px(UI_BAR_H) + px(8);

    /* 主标题 / 副标题 */
    y = top;
    if (g_head1[0]) {
        r = *rc; r.left = pad; r.right = rc->right - pad;
        r.top = y; r.bottom = y + px(UI_HEAD_H);
        text(dc, g_f_head, g_head1, &r, UI_TEXT,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    y += px(UI_HEAD_H) + px(UI_SUB_GAP);
    if (g_head2[0]) {
        r = *rc; r.left = pad; r.right = rc->right - pad;
        r.top = y; r.bottom = y + px(UI_SUB_H);
        text(dc, g_f_sub, g_head2, &r, UI_MUTED,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    /* 阶段轨道 —— 这块是界面的主体，不是装饰：
     * 它就是连接过程本身的状态机，一眼看出卡在第几步。 */
    y = top + px(UI_HEAD_H) + px(UI_SUB_GAP) + px(UI_SUB_H) + px(UI_TRACK_GAP);
    for (i = 0; i < UI_STEPS; i++) {
        int cy = y + px(UI_STEP_H) / 2;
        int cx = pad + px(UI_DOT) / 2 + 1;
        COLORREF tc = UI_MUTED;
        RECT tr;

        dot(dc, cx, cy, px(UI_DOT) / 2, g_st[i]);

        if (g_st[i] == UI_ST_ACTIVE)      tc = UI_TEXT;
        else if (g_st[i] == UI_ST_DONE)   tc = UI_TEXT;
        else if (g_st[i] == UI_ST_FAIL)   tc = UI_RED;

        tr = *rc;
        tr.left = cx + px(UI_DOT) / 2 + px(14);
        tr.right = rc->right - pad - px(190);
        tr.top = y; tr.bottom = y + px(UI_STEP_H);
        text(dc, g_f_step, g_st_name[i], &tr, tc,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        /* 右列：技术细节。只在「正在进行 / 已完成 / 失败」时才写，
         * 还没轮到的行留白 —— 空着比写「等待中」干净。 */
        if (g_st_detail[i][0]) {
            RECT dr = *rc;
            dr.left = rc->right - pad - px(190);
            dr.right = rc->right - pad;
            dr.top = y; dr.bottom = y + px(UI_STEP_H);
            text(dc, g_f_detail, g_st_detail[i], &dr,
                 (g_st[i] == UI_ST_FAIL) ? UI_RED :
                 (g_st[i] == UI_ST_ACTIVE) ? UI_AMBER : UI_MUTED,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        y += px(UI_STEP_H);
    }

    /* 细节行：失败原因 / 当前提示 */
    if (g_foot[0]) {
        RECT fr = *rc;
        fr.left = pad; fr.right = rc->right - pad;
        fr.top = y + px(UI_DETAIL_GAP);
        fr.bottom = fr.top + px(UI_DETAIL_H);
        text(dc, g_f_detail, g_foot, &fr, UI_MUTED,
             DT_LEFT | DT_TOP | DT_WORDBREAK);
    }

    /* 底栏 */
    hr(dc, pad, rc->bottom - px(UI_BAR_H), rc->right - pad, UI_RULE);
    {
        RECT br = *rc;
        br.left = pad;
        br.right = rc->right - px(UI_PAD) - px(240);
        br.top = rc->bottom - px(UI_BAR_H);
        br.bottom = rc->bottom;
        if (g_foot[0] == 0)
            text(dc, g_f_bar, L"", &br, UI_MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

/* ══════════════ 细状态条（连上之后）══════════════ */
void ui_paint_statusbar(HDC dc, const RECT *rc)
{
    RECT bar = *rc;
    RECT r;
    int i;
    static const int ids[3] = { UI_BTN_TOUCH, UI_BTN_RES, UI_BTN_EXIT };

    bar.top = rc->bottom - px(UI_BAR_H);
    FillRect(dc, &bar, g_br_panel);
    hr(dc, 0, bar.top, rc->right, UI_RULE);

    /* 左边：统计（由 viewer 通过 ui_footline 传进来）*/
    r = bar; r.left = px(UI_PAD) - px(12);
    text(dc, g_f_bar, g_foot, &r, UI_MUTED,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    /* 右边三个按钮：扁平、一条细线框、没有圆角 */
    for (i = 0; i < 3; i++) {
        RECT b;
        HGDIOBJ op;
        button_rect(ids[i], rc, &b);
        if (b.right <= b.left) continue;
        /* 细线框 */
        op = SelectObject(dc, GetStockObject(NULL_BRUSH));
        {
            HPEN pn = CreatePen(PS_SOLID, 1, (ids[i] == UI_BTN_EXIT) ? UI_RULE : UI_RULE);
            HGDIOBJ op2 = SelectObject(dc, pn);
            Rectangle(dc, b.left, b.top, b.right, b.bottom);
            SelectObject(dc, op2);
            DeleteObject(pn);
        }
        SelectObject(dc, op);
        text(dc, g_f_bar, g_btn[ids[i]], &b,
             (ids[i] == UI_BTN_EXIT) ? UI_MUTED : UI_TEXT,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    (void)g_btn_pressed;
}
