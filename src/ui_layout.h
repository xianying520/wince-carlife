/* ui_layout.h — 界面配色与几何【唯一来源】
 *
 * 为什么要单独一个文件、还要被 Python 脚本读：
 *   车机上我看不到效果，所以我在电脑上用 tools/render_ui_mock.py 把界面
 *   渲染成 PNG 先看一遍再改。两边读【同一份常量】，
 *   才不会出现「稿子好看、实机走样」。
 *
 * 配色取的是【汽车仪表盘】的语义，不是网页那套：
 *   琥珀 = 进行中（汽车上「注意 / 工作中」的经典颜色）
 *   绿   = 已完成
 *   红   = 失败
 *   灰   = 还没轮到
 * 汽车屏幕要在白天看得清、夜里不刺眼，所以底色用很深的冷黑而不是纯黑，
 * 正文用略偏灰的白而不是纯白（纯白在车里会晃眼）。
 */
#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

/* ── 配色（RGB 宏，Python 脚本按同样格式解析）── */
#define UI_BG        RGB(0x0E,0x11,0x16)   /* 仪表黑 */
#define UI_PANEL     RGB(0x16,0x1B,0x22)   /* 略亮的分区 */
#define UI_TEXT      RGB(0xE6,0xED,0xF3)   /* 正文（偏灰的白，不刺眼）*/
#define UI_MUTED     RGB(0x7D,0x85,0x90)   /* 次要文字 */
#define UI_AMBER     RGB(0xE8,0xA3,0x3D)   /* 进行中 */
#define UI_GREEN     RGB(0x3F,0xB9,0x50)   /* 已完成 */
#define UI_RED       RGB(0xF8,0x51,0x49)   /* 失败 */
#define UI_RULE      RGB(0x2A,0x31,0x3C)   /* 极细分隔线 */
#define UI_DOT_OFF   RGB(0x39,0x41,0x4C)   /* 未开始的圆点 */

/* ── 几何（按 800x480 设计，运行时按屏宽等比缩放）── */
#define UI_BASE_W    800
#define UI_BASE_H    480

#define UI_BAR_H     26      /* 顶栏 / 底栏高度 */
#define UI_PAD       34      /* 左右留白 */

#define UI_HEAD_H    30      /* 主标题字高 */
#define UI_SUB_GAP   12      /* 主标题 → 副标题 */
#define UI_SUB_H     20      /* 副标题字高 */
#define UI_TRACK_GAP 28      /* 副标题 → 阶段轨道 */
#define UI_STEP_H    25      /* 阶段行高 */
#define UI_DOT       9       /* 状态圆点直径 */
#define UI_DETAIL_GAP 20     /* 轨道 → 细节行 */
#define UI_DETAIL_H  40      /* 细节行高 */

#define UI_BTN_H     20      /* 底栏按钮高 */
#define UI_BTN_PAD   13      /* 按钮内左右留白 */
#define UI_BTN_GAP   8       /* 按钮间距 */

/* 字体像素高（CreateFontW 用负值 = 精确像素高）*/
#define UI_F_HEAD    22
#define UI_F_SUB     15
#define UI_F_STEP    14
#define UI_F_DETAIL  13
#define UI_F_BAR     12

#define UI_STEPS     8       /* 阶段总数 */

#endif /* UI_LAYOUT_H */
