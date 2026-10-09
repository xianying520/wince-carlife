/* ui.h — 界面绘制
 *
 * 设计原则（一句话）：**连上之后，界面就该消失。**
 *   没连上时，屏幕是一块「连接进度板」，一眼看出卡在第几步；
 *   连上之后，只剩画面 + 一条 26 像素的细状态条。
 */
#ifndef UI_H
#define UI_H

#include <windows.h>

#define UI_ST_PENDING 0
#define UI_ST_ACTIVE  1
#define UI_ST_DONE    2
#define UI_ST_FAIL    3

/* 底栏按钮编号 */
#define UI_BTN_TOUCH  0
#define UI_BTN_RES    1
#define UI_BTN_EXIT   2
#define UI_BTN_NONE  -1

void ui_init(HWND hwnd);
void ui_free(void);
void ui_set_view(HWND hwnd, int full);      /* 1=显示进度板 0=显示画面 */

/* 阶段轨道 */
void ui_stage(int n, int state);
void ui_stage_detail(int n, const WCHAR *text);
void ui_stages_reset(void);

/* 进度板上的两行大字与底部说明 */
void ui_headline(const WCHAR *line1, const WCHAR *line2);
void ui_footline(const WCHAR *text);

/* 底栏右侧按钮的文字（触摸写法 / 分辨率 / 退出）*/
void ui_button_label(int which, const WCHAR *text);

/* 画进度板（整屏） */
void ui_paint_connect(HDC dc, const RECT *rc);

/* 最小安全绘制：【只用骨架程序在真机上验证过的那几个 API】
 * （FillRect / GetStockObject / SelectObject / SetBkMode / SetTextColor / DrawTextW）
 *
 * 为什么要它：新界面引入了 CreateFontIndirectW / CreatePen / Ellipse / Rectangle，
 * 这几个在这台车机上【从未被验证过】。万一它们有问题，
 * 用户看到的又会是「点了没反应」—— 正是已经踩过两次的那个坑。
 * 所以先用验证过的画法把「程序已启动」画出来，再上新界面。 */
void ui_paint_minimal(HDC dc, const RECT *rc);

/* 界面资源是否已就绪（0 = 还没建好，此时应当走上面的最小绘制） */
int  ui_ready(void);

/* 画画面区之外的细状态条 */
void ui_paint_statusbar(HDC dc, const RECT *rc);

/* 画面区（连上之后画面铺这一块） */
void ui_view_rect(const RECT *rc, RECT *out);

/* 按钮命中测试：返回 UI_BTN_* 或 UI_BTN_NONE */
int  ui_hit_button(int x, int y, const RECT *rc);

#endif /* UI_H */
