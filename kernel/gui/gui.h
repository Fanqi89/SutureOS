/* SutureOS module: GUI framework - 移植自 VimtuOS gui64
 * Source: C:\Users\fanqi\Desktop\自研操作系统\VimtuOS\kernel\gui64.h/cpp
 * Original license: GPL-3.0
 * Changes: Adapted to SutureOS; prefix gui_; stub implementation for compilation
 */
#ifndef STITCH_GUI_H
#define STITCH_GUI_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GUI 常量 */
#define GUI_MAX_WINDOWS    16
#define GUI_TITLE_H        24
#define GUI_BORDER         1
#define GUI_DECO_H         (GUI_TITLE_H + GUI_BORDER)
#define GUI_DECO_W         (GUI_BORDER * 2)
#define GUI_TASKBAR_H      40
#define GUI_BTN_W          30
#define GUI_ICON_W         48
#define GUI_ICON_CELL_W    88
#define GUI_ICON_CELL_H    84

/* 窗口状态 */
#define WIN_VISIBLE   (1 << 0)
#define WIN_ACTIVE    (1 << 1)
#define WIN_MINIMIZED (1 << 2)
#define WIN_MAXIMIZED (1 << 3)
#define WIN_CLOSING   (1 << 4)

/* 应用 ID */
#define APP_ID_NONE      0
#define APP_ID_CALC      1
#define APP_ID_MINES     2
#define APP_ID_TERM      3
#define APP_ID_TMGR      4
#define APP_ID_SETTINGS  5

/* 鼠标光标形状 */
#define MOUSE_CUR_ARROW    0
#define MOUSE_CUR_IBEAM    1
#define MOUSE_CUR_WAIT     2
#define MOUSE_CUR_CROSS    3
#define MOUSE_CUR_RESIZE_L 4
#define MOUSE_CUR_RESIZE_R 5
#define MOUSE_CUR_RESIZE_T 6
#define MOUSE_CUR_RESIZE_B 7
#define MOUSE_CUR_RESIZE_LT 8
#define MOUSE_CUR_RESIZE_RT 9
#define MOUSE_CUR_RESIZE_LB 10
#define MOUSE_CUR_RESIZE_RB 11
#define MOUSE_CUR_MOVE     12
#define MOUSE_CUR_HAND     13

/* 窗口结构 */
struct gui_window {
    int x, y, w, h;
    int client_x, client_y;
    int client_w, client_h;
    int min_w, min_h;
    char title[32];
    uint32_t flags;
    bool visible;
    bool active;
    bool minimized;
    bool maximized;
    int norm_x, norm_y, norm_w, norm_h;
    
    /* 回调函数 */
    void (*draw)(struct gui_window*);
    void (*on_key)(struct gui_window*, char c);
    void (*on_click)(struct gui_window*, int cx, int cy);
    void (*on_click2)(struct gui_window*, int cx, int cy, int button);
    void (*on_tick)(struct gui_window*);
    void (*on_close)(struct gui_window*);
    
    void* userdata;
    uint8_t app_id;
    uint8_t mem_owner;
    uint64_t cpu_cycles;
    uint8_t cpu_pct;
    struct gui_window* next;
    uint32_t anim_t0, anim_dur;
    uint8_t anim_kind;
    uint8_t closing;
    uint8_t resizing;
};

/* 全局状态 */
extern int g_gui_screen_w;
extern int g_gui_screen_h;
extern bool g_gui_initialized;

/* GUI 初始化 */
int gui_init(int screen_w, int screen_h);
int gui_run(void);  /* 主循环，不返回 */

/* 窗口管理 */
struct gui_window* gui_create_window(const char* title, int x, int y, int w, int h,
                                     void (*draw)(struct gui_window*),
                                     void (*on_key)(struct gui_window*, char),
                                     void (*on_click)(struct gui_window*, int, int),
                                     int app_id);
void gui_destroy_window(struct gui_window* w);
void gui_set_active(struct gui_window* w);
void gui_set_title(struct gui_window* w, const char* title);
void gui_set_min_size(struct gui_window* w, int mw, int mh);
void gui_set_click2(struct gui_window* w, void (*fn)(struct gui_window*, int, int, int));
void gui_set_tick(struct gui_window* w, void (*fn)(struct gui_window*));
void gui_set_geom(struct gui_window* w, int x, int y, int cw, int ch);
int  gui_fit_window_to_client(struct gui_window* w, int cw, int ch);
void gui_invalidate_window(struct gui_window* w);
void gui_flip_window(struct gui_window* w);

/* 系统查询 */
int gui_screen_w(void);
int gui_screen_h(void);
int gui_taskbar_h(void);
struct gui_window* gui_top_window(void);
bool gui_click_is_right(void);
int gui_window_count(void);
struct gui_window* gui_window_at(int i);
int gui_app_windows(int app_id);
void gui_close_all_windows(void);
int gui_close_app(int app_id);

/* 语言 */
bool gui_lang_zh(void);
void gui_set_lang_zh(bool zh);
const char* gui_tr(const char* en, const char* zh);

/* 重启/关机 */
void sys_reboot(void);
void sys_shutdown(void);

/* 脏矩形 */
void gui_dirty(int x, int y, int w, int h);
void gui_invalidate(void);

/* 性能 */
uint32_t gui_fps(void);
uint8_t gui_cpu_busy_pct(void);

/* 光标 */
void gui_set_cursor(int shape);
int gui_get_cursor(void);

/* 自检 */
int gui_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_GUI_H */