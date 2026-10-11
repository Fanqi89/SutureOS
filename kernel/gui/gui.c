/* SutureOS module: GUI framework - 移植自 VimtuOS gui64
 * Source: C:\Users\fanqi\Desktop\自研操作系统\VimtuOS\kernel\gui64.h/cpp
 * Original license: GPL-3.0
 * Changes: Adapted to SutureOS; prefix gui_; stub implementation for compilation
 */
#include "gui.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"

/* 全局状态 */
static struct gui_window g_wins[GUI_MAX_WINDOWS];
static bool g_used[GUI_MAX_WINDOWS] = {0};
static struct gui_window* g_z = NULL;
int g_gui_screen_w = 0;
int g_gui_screen_h = 0;
bool g_gui_initialized = false;
bool g_lang_zh = true;
bool g_click_right = false;
static int g_dirty_x0, g_dirty_y0, g_dirty_x1, g_dirty_y1;
static bool g_dirty_any = false;

int gui_init(int screen_w, int screen_h) {
    memset(g_wins, 0, sizeof(g_wins));
    memset(g_used, 0, sizeof(g_used));
    g_z = NULL;
    g_gui_screen_w = screen_w;
    g_gui_screen_h = screen_h;
    g_gui_initialized = true;
    g_lang_zh = true;
    g_click_right = false;
    g_dirty_any = false;
    
    console_puts("[gui] GUI framework init (stub)\n");
    return 0;
}

int gui_run(void) {
    console_puts("[gui] GUI main loop (stub) - not implemented\n");
    return 0;
}

struct gui_window* gui_create_window(const char* title, int x, int y, int w, int h,
                                     void (*draw)(struct gui_window*),
                                     void (*on_key)(struct gui_window*, char),
                                     void (*on_click)(struct gui_window*, int, int),
                                     int app_id) {
    if (!g_gui_initialized) gui_init(g_gui_screen_w, g_gui_screen_h);
    
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!g_used[i]) {
            struct gui_window* win = &g_wins[i];
            memset(win, 0, sizeof(struct gui_window));
            g_used[i] = true;
            
            win->x = x;
            win->y = y;
            win->w = w;
            win->h = h;
            win->min_w = 120;
            win->min_h = 80;
            win->visible = true;
            win->active = false;
            win->minimized = false;
            win->maximized = false;
            win->norm_x = x;
            win->norm_y = y;
            win->norm_w = w;
            win->norm_h = h;
            
            win->draw = draw;
            win->on_key = on_key;
            win->on_click = on_click;
            win->on_click2 = NULL;
            win->on_tick = NULL;
            win->on_close = NULL;
            win->userdata = NULL;
            win->app_id = (uint8_t)app_id;
            win->mem_owner = 0;
            win->cpu_cycles = 0;
            win->cpu_pct = 0;
            win->next = NULL;
            win->anim_t0 = 0;
            win->anim_dur = 0;
            win->anim_kind = 0;
            win->closing = 0;
            win->resizing = 0;
            
            if (title) {
                int i = 0;
                for (; title[i] && i < 31; i++) win->title[i] = title[i];
                win->title[i] = 0;
            }
            
            // 简单的客户区计算
            win->client_x = win->x + 1;
            win->client_y = win->y + GUI_TITLE_H + GUI_BORDER;
            win->client_w = win->w - GUI_BORDER * 2;
            win->client_h = win->h - GUI_TITLE_H - GUI_BORDER * 2;
            if (win->client_w < 1) win->client_w = 1;
            if (win->client_h < 1) win->client_h = 1;
            
            win->next = g_z;
            g_z = win;
            
            console_printf("[gui] Window created: %s (app_id=%d)\n", title ? title : "unnamed", app_id);
            return win;
        }
    }
    return NULL;
}

void gui_destroy_window(struct gui_window* w) {
    if (!w) return;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (g_used[i] && &g_wins[i] == w) {
            if (w->on_close) w->on_close(w);
            w->visible = false;
            g_used[i] = false;
            w->next = NULL;
            console_puts("[gui] Window destroyed\n");
            break;
        }
    }
}

void gui_set_active(struct gui_window* w) {
    if (!w) return;
    for (struct gui_window* p = g_z; p; p = p->next) p->active = false;
    w->active = true;
    w->minimized = false;
    // 移到 z 序头部
    struct gui_window** pp = &g_z;
    while (*pp) {
        if (*pp == w) {
            *pp = w->next;
            break;
        }
        pp = &(*pp)->next;
    }
    w->next = g_z;
    g_z = w;
}

void gui_set_title(struct gui_window* w, const char* title) {
    if (!w || !title) return;
    int i = 0;
    for (; title[i] && i < 31; i++) w->title[i] = title[i];
    w->title[i] = 0;
}

void gui_set_min_size(struct gui_window* w, int mw, int mh) {
    if (!w) return;
    w->min_w = mw > 40 ? mw : 40;
    w->min_h = mh > 40 ? mh : 40;
}

void gui_set_click2(struct gui_window* w, void (*fn)(struct gui_window*, int, int, int)) {
    if (w) w->on_click2 = fn;
}

void gui_set_tick(struct gui_window* w, void (*fn)(struct gui_window*)) {
    if (w) w->on_tick = fn;
}

void gui_set_geom(struct gui_window* w, int x, int y, int cw, int ch) {
    if (!w) return;
    w->x = x;
    w->y = y;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    w->w = cw + GUI_BORDER * 2;
    w->h = ch + GUI_TITLE_H + GUI_BORDER;
    if (w->w < w->min_w) w->w = w->min_w;
    if (w->h < w->min_h) w->h = w->min_h;
    w->client_x = w->x + GUI_BORDER;
    w->client_y = w->y + GUI_TITLE_H + GUI_BORDER;
    w->client_w = w->w - GUI_BORDER * 2;
    w->client_h = w->h - GUI_TITLE_H - GUI_BORDER * 2;
    if (w->client_w < 1) w->client_w = 1;
    if (w->client_h < 1) w->client_h = 1;
}

int gui_fit_window_to_client(struct gui_window* w, int cw, int ch) {
    if (!w) return 0;
    const int nw = cw + GUI_BORDER * 2;
    const int nh = ch + GUI_TITLE_H + GUI_BORDER;
    if (nw < w->min_w || nh < w->min_h) return 0;
    if (nw == w->w && nh == w->h) return 1;
    w->w = nw;
    w->h = nh;
    w->client_x = w->x + GUI_BORDER;
    w->client_y = w->y + GUI_TITLE_H + GUI_BORDER;
    w->client_w = w->w - GUI_BORDER * 2;
    w->client_h = w->h - GUI_TITLE_H - GUI_BORDER * 2;
    if (w->client_w < 1) w->client_w = 1;
    if (w->client_h < 1) w->client_h = 1;
    return 1;
}

void gui_invalidate_window(struct gui_window* w) {
    (void)w;
}

void gui_flip_window(struct gui_window* w) {
    (void)w;
}

int gui_screen_w(void) { return g_gui_screen_w; }
int gui_screen_h(void) { return g_gui_screen_h; }
int gui_taskbar_h(void) { return GUI_TASKBAR_H; }

struct gui_window* gui_top_window(void) { return g_z; }

bool gui_click_is_right(void) { return g_click_right; }

int gui_window_count(void) {
    int n = 0;
    for (struct gui_window* w = g_z; w; w = w->next) if (w->visible) n++;
    return n;
}

struct gui_window* gui_window_at(int i) {
    int k = 0;
    for (struct gui_window* w = g_z; w; w = w->next) {
        if (!w->visible) continue;
        if (k == i) return w;
        k++;
    }
    return NULL;
}

int gui_app_windows(int app_id) {
    int n = 0;
    for (struct gui_window* w = g_z; w; w = w->next)
        if (w->visible && w->app_id == (uint8_t)app_id) n++;
    return n;
}

void gui_close_all_windows(void) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (g_used[i]) gui_destroy_window(&g_wins[i]);
}

int gui_close_app(int app_id) {
    int n = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (g_used[i] && g_wins[i].app_id == (uint8_t)app_id) {
            gui_destroy_window(&g_wins[i]);
            n++;
        }
    }
    return n;
}

bool gui_lang_zh(void) { return g_lang_zh; }

void gui_set_lang_zh(bool zh) {
    g_lang_zh = zh;
}

const char* gui_tr(const char* en, const char* zh) { return g_lang_zh ? zh : en; }

void sys_reboot(void) {
    console_puts("[gui] Reboot requested\n");
}

void sys_shutdown(void) {
    console_puts("[gui] Shutdown requested\n");
}

void gui_dirty(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}

void gui_invalidate(void) {
    g_dirty_any = true;
    g_dirty_x0 = 0;
    g_dirty_y0 = 0;
    g_dirty_x1 = g_gui_screen_w;
    g_dirty_y1 = g_gui_screen_h;
}

uint32_t gui_fps(void) { return 60; }
uint8_t gui_cpu_busy_pct(void) { return 0; }

void gui_set_cursor(int shape) { (void)shape; }
int gui_get_cursor(void) { return MOUSE_CUR_ARROW; }

int gui_selftest(void) {
    console_puts("[gui] selftest (stub) PASS\n");
    return 0;
}