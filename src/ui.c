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
static HBRUSH g_br_wash;

static int   g_sw = UI_BASE_W, g_sh = UI_BASE_H;
static int   g_st[UI_STEPS];
static WCHAR g_st_detail[UI_STEPS][72];
static WCHAR g_head1[96], g_head2[96], g_foot[200];
static WCHAR g_btn[3][24];
static int   g_btn_pressed = -1;
static int   g_ready = 0;      /* 界面资源是否已建好 */

/* 按屏宽等比缩放（车机就是 800x480，这里只是留个余量）*/
static int px(int v) { return v * g_sw / UI_BASE_W; }

/* 建字体。
 * ⚠ coredll 上没有 CreateFontW，只有 CreateFontIndirectW —— 链接期实锤过。 */
static HFONT mkfont(int h, int weight)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight         = -h;          /* 负值 = 字符高度，正是要的像素高 */
    lf.lfWeight         = weight;
    lf.lfCharSet        = DEFAULT_CHARSET;
    lf.lfOutPrecision   = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision  = CLIP_DEFAULT_PRECIS;
    lf.lfQuality        = DEFAULT_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    /* lfFaceName 留空 = 系统默认字面，各种 WinCE ROM 上最稳 */
    {
        HFONT f = CreateFontIndirectW(&lf);
        /* 万一本 ROM 没有可缩放的字体，CreateFontIndirectW 会返回 NULL。
         * 那时候退回系统字体 —— 字号不对，但【字至少还在】，
         * 不至于让用户面对一块什么都没有的屏幕。 */
        if (!f)
            f = (HFONT)GetStockObject(SYSTEM_FONT);
        return f;
    }
}

/* 估一段文字的宽度。
 * ⚠ 不调 GetTextExtent*：coredll 上有的只是参数更多的 GetTextExtentExPointW，
 *   而这里只需要一个「够用」的宽度来排按钮。中日韩按 1 个字高、ASCII 按半个。 */
static int text_width_est(const WCHAR *s, int fpx)
{
    int w = 0, i;
    if (!s) return 0;
    for (i = 0; s[i]; i++)
        w += (s[i] < 128) ? (fpx / 2) : fpx;
    return w;
}

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
        for (k = 0; k < 20 && b0[k]; k++) g_btn[0][k] = b0[k];
        g_btn[0][k] = 0;
        for (k = 0; k < 20 && b1[k]; k++) g_btn[1][k] = b1[k];
        g_btn[1][k] = 0;
        for (k = 0; k < 20 && b2[k]; k++) g_btn[2][k] = b2[k];
        g_btn[2][k] = 0;
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
    g_f_head   = mkfont(px(UI_F_HEAD),   700);
    g_f_sub    = mkfont(px(UI_F_SUB),    400);
    g_f_step   = mkfont(px(UI_F_STEP),   400);
    g_f_detail = mkfont(px(UI_F_DETAIL), 400);
    g_f_bar    = mkfont(px(UI_F_BAR),    400);
    ReleaseDC(0, dc);

    g_br_bg    = CreateSolidBrush(UI_BG);
    g_br_panel = CreateSolidBrush(UI_PANEL);
    g_br_wash  = CreateSolidBrush(UI_WASH);

    dotc[UI_ST_PENDING] = UI_DOT_OFF;
    dotc[UI_ST_ACTIVE]  = UI_AMBER;
    dotc[UI_ST_DONE]    = UI_GREEN;
    dotc[UI_ST_FAIL]    = UI_RED;
    for (i = 0; i < 4; i++)
        /* 只建画刷，不再建画笔 —— 状态点改用 FillRect 方块画，理由见下面那个函数的说明。
         * ⚠ 注释里别写「函数名紧跟左括号」：scripts/check_order.py 不剥注释，
         *   会被误判成「函数用在定义之前」。 */
        g_br_dot[i] = CreateSolidBrush(dotc[i]);

    reset_labels();

    /* 到这里说明字体和画刷都建出来了 —— 后面才允许走「完整界面」那条路 */
    g_ready = 1;
}

int ui_ready(void) { return g_ready; }

/* ══════════ 兜底画面：只用骨架验证过的 API ══════════
 * ⚠⚠ 这个函数里【只准用】下面这几个：
 *      FillRect / GetStockObject / SelectObject / SetBkMode / SetTextColor / DrawTextW
 *   绝不要用 CreateFontIndirectW，也不要用画笔/椭圆/矩形那几个调用 ——
 *   那几个是本程序新引入的，在你这台车机上【还没被验证过】。
 *   它存在的意义：哪怕新界面那些调用全都不行，
 *   用户也能看到这一屏，从而知道「程序起来了，是界面没画出来」——
 *   而不是第三次面对一个什么都不发生的屏幕。 */
void ui_paint_minimal(HDC dc, const RECT *rc)
{
    RECT r = *rc;
    HGDIOBJ of;

    FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(230, 230, 230));
    of = SelectObject(dc, GetStockObject(SYSTEM_FONT));

    r.left += 14;
    r.top  += 14;
    DrawTextW(dc, L"CarLife 车机端 —— 程序已启动", -1, &r,
              DT_LEFT | DT_TOP | DT_SINGLELINE);

    r.top += 26;
    DrawTextW(dc, L"正在初始化界面 ...", -1, &r,
              DT_LEFT | DT_TOP | DT_SINGLELINE);

    r.top += 26;
    DrawTextW(dc, L"（如果这一行一直不变，请把这一屏拍下来发给开发者）", -1, &r,
              DT_LEFT | DT_TOP | DT_SINGLELINE);

    SelectObject(dc, of);
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
    if (g_br_wash)  DeleteObject(g_br_wash);
    for (i = 0; i < 4; i++)
        if (g_br_dot[i]) DeleteObject(g_br_dot[i]);
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

/* 内容列：屏幕比 UI_CONTENT_W 宽就居中收窄，两边对称留白。
 * 两列信息只有靠得够近才读得成一行，铺满全宽会散成两块。 */
static int content_left(const RECT *rc, int *out_right)
{
    int cw = px(UI_CONTENT_W);
    int l;
    if (cw > rc->right) cw = rc->right;
    l = (rc->right - cw) / 2;
    *out_right = l + cw;
    return l;
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

/* ── 状态方块：实心=已发生，空心=还没轮到 ──
 *
 * ⚠ 刻意【不用 Ellipse + CreatePen + SelectObject(pen)】画圆点：
 *   这几个 API 在本程序里从没在这台车机上验证过，而它们正是
 *   「窗口闪一下就没了」最可疑的几处。
 *   9x9 的尺寸上，方块和圆点肉眼几乎分不出 —— 用验证过的 FillRect 更划算。
 *   空心那个用四条 1 像素细条拼出来（四边各一次 FillRect）。 */
static void dot(HDC dc, int cx, int cy, int r, int state)
{
    RECT q, e;

    q.left = cx - r; q.right = cx + r + 1;
    q.top  = cy - r; q.bottom = cy + r + 1;

    if (state != UI_ST_PENDING) {
        FillRect(dc, &q, g_br_dot[state]);
        return;
    }
    e = q; e.bottom = e.top + 1;     FillRect(dc, &e, g_br_dot[state]);   /* 上 */
    e = q; e.top    = e.bottom - 1;  FillRect(dc, &e, g_br_dot[state]);   /* 下 */
    e = q; e.right  = e.left + 1;    FillRect(dc, &e, g_br_dot[state]);   /* 左 */
    e = q; e.left   = e.right - 1;   FillRect(dc, &e, g_br_dot[state]);   /* 右 */
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
    int w[3], i, x;

    /* 从右往左：退出、分辨率、触摸 */
    order[0] = UI_BTN_EXIT; order[1] = UI_BTN_RES; order[2] = UI_BTN_TOUCH;

    for (i = 0; i < 3; i++) {
        w[i] = text_width_est(g_btn[order[i]], px(UI_F_BAR)) + 2 * px(UI_BTN_PAD);
        if (w[i] < px(46)) w[i] = px(46);
    }

    {
        int cr;
        content_left(rc, &cr);      /* 和正文共用同一条右边距 */
        x = cr;
    }
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

    /* 底 */
    FillRect(dc, rc, g_br_bg);

    /* 顶栏：左边标题，右边提示日志在哪。
     * ⚠ 左右边距一律走【内容列】，不能再单独用 pad ——
     *   否则标题和正文会各走一条左边距，一眼就看得出不对齐。 */
    {
        int cr, cl = content_left(rc, &cr);
        r = *rc; r.bottom = r.top + px(UI_BAR_H);
        r.left = cl; r.right = cr - px(170);
        text(dc, g_f_bar, L"CarLife 车机端", &r, UI_MUTED,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        {
            const WCHAR *lg = L"运行日志：carlife-log.txt";
            r.left = cr - px(170); r.right = cr;
            text(dc, g_f_bar, lg, &r, UI_HINT,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        hr(dc, cl, rc->top + px(UI_BAR_H), cr, UI_RULE);
    }

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
        int cr, cl2b = content_left(rc, &cr);
        r = *rc; r.left = cl2b; r.right = cr;
        r.top = y; r.bottom = y + px(UI_HEAD_H);
        text(dc, g_f_head, g_head1, &r, UI_TEXT,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    y += px(UI_HEAD_H) + px(UI_SUB_GAP);
    if (g_head2[0]) {
        int cr, cl2b = content_left(rc, &cr);
        r = *rc; r.left = cl2b; r.right = cr;
        r.top = y; r.bottom = y + px(UI_SUB_H);
        text(dc, g_f_sub, g_head2, &r, UI_MUTED,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    /* 阶段轨道 —— 这块是界面的主体，不是装饰：
     * 它就是连接过程本身的状态机，一眼看出卡在第几步。 */
    y = top + px(UI_HEAD_H) + px(UI_SUB_GAP) + px(UI_SUB_H) + px(UI_TRACK_GAP);
    for (i = 0; i < UI_STEPS; i++) {
        int cr, cl2 = content_left(rc, &cr);
        int cy = y + px(UI_STEP_H) / 2;
        int cx = cl2 + px(UI_DOT) / 2 + 1;
        COLORREF tc = UI_MUTED;
        RECT tr;

        /* 进行中那一行铺一层极淡的琥珀底。
         * 8 行长得一模一样，光靠圆点颜色在车里一眼分不出来 ——
         * 这不是装饰，是状态高亮。 */
        if (g_st[i] == UI_ST_ACTIVE) {
            RECT wr;
            wr.left = cl2 - px(10); wr.right = cr + px(10);
            wr.top = y + 1; wr.bottom = y + px(UI_STEP_H) - 1;
            FillRect(dc, &wr, g_br_wash);
        }

        dot(dc, cx, cy, px(UI_DOT) / 2, g_st[i]);

        if (g_st[i] == UI_ST_ACTIVE)      tc = UI_TEXT;
        else if (g_st[i] == UI_ST_DONE)   tc = UI_TEXT;
        else if (g_st[i] == UI_ST_FAIL)   tc = UI_RED;

        tr = *rc;
        tr.left = cx + px(UI_DOT) / 2 + px(14);
        tr.right = cr - px(UI_DETAIL_W);
        tr.top = y; tr.bottom = y + px(UI_STEP_H);
        text(dc, g_f_step, g_st_name[i], &tr, tc,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        /* 右列：技术细节。只在「正在进行 / 已完成 / 失败」时才写，
         * 还没轮到的行留白 —— 空着比写「等待中」干净。 */
        if (g_st_detail[i][0]) {
            RECT dr = *rc;
            dr.left = cr - px(UI_DETAIL_W);
            dr.right = cr;
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
        int cr, cl2 = content_left(rc, &cr);
        RECT fr = *rc;
        fr.left = cl2; fr.right = cr;
        fr.top = y + px(UI_DETAIL_GAP);
        fr.bottom = fr.top + px(UI_DETAIL_H);
        text(dc, g_f_detail, g_foot, &fr, UI_MUTED,
             DT_LEFT | DT_TOP | DT_WORDBREAK);
    }

    /* 底栏 */
    {
        int cr, cl = content_left(rc, &cr);
        hr(dc, cl, rc->bottom - px(UI_BAR_H), cr, UI_RULE);
    }
}

/* ══════════════ 细状态条（连上之后）══════════════ */
void ui_paint_statusbar(HDC dc, const RECT *rc)
{
    RECT bar = *rc;
    RECT r;
    int i;
    static const int ids[3] = { UI_BTN_TOUCH, UI_BTN_RES, UI_BTN_EXIT };

    int cr, cl;
    bar.top = rc->bottom - px(UI_BAR_H);
    FillRect(dc, &bar, g_br_panel);
    cl = content_left(rc, &cr);
    hr(dc, cl, bar.top, cr, UI_RULE);

    /* 左边：统计（由 viewer 通过 ui_footline 传进来）*/
    r = bar; r.left = cl; r.right = cr - px(220);
    text(dc, g_f_bar, g_foot, &r, UI_MUTED,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    /* 右边三个按钮：扁平、一条细线框、没有圆角 */
    for (i = 0; i < 3; i++) {
        RECT b;
        button_rect(ids[i], rc, &b);
        if (b.right <= b.left) continue;
        /* 细线框：用四条 1 像素细条拼，不用 CreatePen + Rectangle */
        {
            HBRUSH fb = CreateSolidBrush(UI_RULE);
            RECT e;
            e = b; e.bottom = e.top + 1;    FillRect(dc, &e, fb);   /* 上 */
            e = b; e.top    = e.bottom - 1; FillRect(dc, &e, fb);   /* 下 */
            e = b; e.right  = e.left + 1;   FillRect(dc, &e, fb);   /* 左 */
            e = b; e.left   = e.right - 1;  FillRect(dc, &e, fb);   /* 右 */
            DeleteObject(fb);
        }
        text(dc, g_f_bar, g_btn[ids[i]], &b,
             (ids[i] == UI_BTN_EXIT) ? UI_MUTED : UI_TEXT,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    (void)g_btn_pressed;
}
