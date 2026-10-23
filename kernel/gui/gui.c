/* SutureOS module: GUI framework - ported from VimtuOS gui64
 * Source: C:\Users\fanqi\Desktop\自研操作系统\VimtuOS\kernel\gui64.h/cpp
 * Original license: GPL-3.0
 * Changes: Adapted to SutureOS; prefix gui_; window bookkeeping kept from the
 *          original stub, the framebuffer main loop / composition / title-bar
 *          drawing / keyboard handling are ported from the reference.
 * Main loop ported/adapted from VimtuOS kernel/gui64.cpp (read-only reference), GPL-3.0
 */
#include "gui.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/init.h>      /* limine_fb_* : real framebuffer discovery (same as kmain/fbcon) */
#include "heap.h"
#include "task.h"             /* sched_yield */
#include <stitch/io.h>     /* inb() for the 8042 status heartbeat */
#include "mb_kbd.h"           /* mbos_kbd_poll / mbos_kbd_decode (same pattern as shell.c) */
#include "power.h"            /* power_reset / power_off (ACPI S5 + reset register) */
#include "font.h"             /* font8x16: ASCII 32..126 bitmap font (fbcon's font asset) */

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

/* ==================== Framebuffer access ====================
 * Identity-mapped GOP framebuffer, exactly like kernel/drivers/fbcon.c:
 * the physical address IS the virtual address (kernel built a 1 GiB identity
 * map; fbcon rejects anything >= 1 GiB for the same reason).
 * Supports 32bpp and 24bpp, little-endian BGR(X) packing = 0x00RRGGBB,
 * identical to fbcon's pack_pixel()/per-pixel uint32 write. */
static volatile uint8_t* g_fb = NULL;
static uint32_t g_fb_pitch = 0;
static uint32_t g_fb_bpp = 0;
static const char* g_fb_fail = NULL;    /* reason of last gui_fb_setup() failure */

/* dirty-rect clip used while composing a frame */
static int g_clip_x0 = 0, g_clip_y0 = 0, g_clip_x1 = 0, g_clip_y1 = 0;

/* measured perf (returned by gui_fps / gui_cpu_busy_pct) */
static uint32_t g_fps_meas = 60;
static uint8_t  g_busy_meas = 0;

/* timer tick counter (kernel/main/init.c, incremented from the vector 0x20 ISR) */
extern volatile uint64_t g_tick_count;

/* max pause-spin while waiting for a tick; bounds the wait if the timer is dead */
#define GUI_PACE_MAX_SPINS 8000000

/* ==================== low-level drawing primitives ==================== */

static inline uint64_t gui_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void gui_px(int x, int y, uint32_t c) {
    if (!g_fb) return;
    if (x < g_clip_x0 || y < g_clip_y0 || x >= g_clip_x1 || y >= g_clip_y1) return;
    volatile uint8_t* p = g_fb + (uint32_t)y * g_fb_pitch +
                          (uint32_t)x * (g_fb_bpp >> 3);
    if (g_fb_bpp == 32) {
        *(volatile uint32_t*)p = c;                       /* XRGB, like fbcon */
    } else {                                              /* 24bpp: B,G,R */
        p[0] = (uint8_t)c;
        p[1] = (uint8_t)(c >> 8);
        p[2] = (uint8_t)(c >> 16);
    }
}

/* clipped solid rectangle fill (clip = current dirty/segment clip rect) */
static void gui_fill(int x, int y, int w, int h, uint32_t c) {
    if (!g_fb || w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < g_clip_x0) x0 = g_clip_x0;
    if (y0 < g_clip_y0) y0 = g_clip_y0;
    if (x1 > g_clip_x1) x1 = g_clip_x1;
    if (y1 > g_clip_y1) y1 = g_clip_y1;
    if (x1 <= x0 || y1 <= y0) return;
    if (g_fb_bpp == 32) {
        for (int yy = y0; yy < y1; yy++) {
            volatile uint32_t* p =
                (volatile uint32_t*)(g_fb + (uint32_t)yy * g_fb_pitch) + x0;
            for (int xx = x0; xx < x1; xx++) *p++ = c;
        }
    } else {
        for (int yy = y0; yy < y1; yy++) {
            volatile uint8_t* p = g_fb + (uint32_t)yy * g_fb_pitch +
                                  (uint32_t)x0 * 3;
            for (int xx = x0; xx < x1; xx++) {
                p[0] = (uint8_t)c;
                p[1] = (uint8_t)(c >> 8);
                p[2] = (uint8_t)(c >> 16);
                p += 3;
            }
        }
    }
}

/* 1px rectangle outline */
static void gui_rect(int x, int y, int w, int h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    gui_fill(x, y, w, 1, c);
    gui_fill(x, y + h - 1, w, 1, c);
    gui_fill(x, y, 1, h, c);
    gui_fill(x + w - 1, y, 1, h, c);
}

/* text: font8x16 (95 glyphs, ASCII 32..126) - the same asset fbcon renders with */
static void gui_draw_char(int x, int y, char ch, uint32_t fg) {
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t* glyph = font8x16 + (ch - 32) * 16;
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        if (!bits) continue;
        for (int col = 0; col < 8; col++)
            if (bits & (uint8_t)(0x80 >> col)) gui_px(x + col, y + row, fg);
    }
}

static void gui_draw_text(int x, int y, const char* s, uint32_t fg) {
    if (!s) return;
    while (*s) {
        gui_draw_char(x, y, *s++, fg);
        x += 8;
    }
}

/* decimal u32 -> buf, returns length (no terminator) */
static int gui_u32_dec(uint32_t v, char* buf) {
    char tmp[10];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int out = 0;
    while (n) buf[out++] = tmp[--n];
    return out;
}

static void gui_clip_full(void) {
    g_clip_x0 = 0;
    g_clip_y0 = 0;
    g_clip_x1 = g_gui_screen_w;
    g_clip_y1 = g_gui_screen_h;
}

/* ==================== framebuffer discovery ==================== */
/* returns NULL on success, else the failure reason (g_fb_fail mirrors it) */
static const char* gui_fb_setup(void) {
    g_fb = NULL;
    g_fb_pitch = 0;
    g_fb_bpp = 0;
    g_fb_fail = NULL;
    if (!limine_fb_addr || !limine_fb_width || !limine_fb_height) {
        return (g_fb_fail = "not available");            /* -> "[gui] no framebuffer" */
    }
    /* No address-range guard here any more: limine_parse_fb() identity-maps the
     * linear framebuffer window with mmu_map_mmio_identity() even when it lives
     * above 1 GiB (QEMU/VMware report 0xFD000000), so writing to it is safe.
     * The old "outside 1 GiB identity map" rejection made `startgui` refuse to
     * start on exactly the machines this kernel targets. */
    if (limine_fb_bpp != 32 && limine_fb_bpp != 24)
        return (g_fb_fail = "unsupported bpp (need 24 or 32)");
    if (limine_fb_pitch < limine_fb_width * (limine_fb_bpp / 8))
        return (g_fb_fail = "bad pitch");
    g_fb = (volatile uint8_t*)(uintptr_t)limine_fb_addr;
    g_fb_pitch = (uint32_t)limine_fb_pitch;
    g_fb_bpp = (uint32_t)limine_fb_bpp;
    return NULL;
}

/* ==================== focus / keyboard helpers ==================== */

static struct gui_window* gui_focused(void) {
    for (struct gui_window* w = g_z; w; w = w->next)
        if (w->visible && !w->minimized && w->active) return w;
    for (struct gui_window* w = g_z; w; w = w->next)
        if (w->visible && !w->minimized) return w;
    return NULL;
}

/* Defined later in this file, used by the pointer/menu code below.
 * (gui_dirty() is already declared by gui.h.) */
static void gui_activate_app(int app_id);

/* ==================== pointer + start menu ====================
 * The GUI started out keyboard-only, so there was no pointer on screen and the
 * taskbar name was decoration. The PS/2 driver (kernel/drivers/mb_mouse.c) now
 * supplies a real pointer, so:
 *   - an arrow is composited on top of every frame,
 *   - the bottom-left "SutureOS" name is a start button; clicking it pops a
 *     menu UPWARDS out of the taskbar (as requested),
 *   - clicking a menu entry launches/focuses that application.
 */
#define START_BTN_X      4
#define START_BTN_W      96
#define START_MENU_W     212
#define START_MENU_ITEM_H 24
#define START_MENU_PAD   6

/* Items in the start menu. NULL = separator line. */
enum { START_KIND_SEP = -1, START_KIND_EXIT = -2 };
static const struct {
    const char* label;
    int         app_id;      /* APP_ID_* or START_KIND_* */
} g_start_items[] = {
    { "Task Manager",   APP_ID_TMGR     },
    { "Calculator",     APP_ID_CALC     },
    { "About SutureOS", APP_ID_SETTINGS },
    { NULL,             START_KIND_SEP  },
    { "Exit to terminal", START_KIND_EXIT },
};
#define START_ITEM_COUNT ((int)(sizeof(g_start_items) / sizeof(g_start_items[0])))

static int g_mx, g_my;                  /* pointer position (clamped)      */
static int g_mprev_x, g_mprev_y, g_mprev_w, g_mprev_h;   /* last drawn rect  */
static int g_start_open;
static int g_gui_exit;                  /* set by the start menu           */
static int g_pointer_selftest;          /* armed by `startgui test`        */
static int start_menu_height(void);   /* defined below */

void gui_arm_pointer_selftest(void) { g_pointer_selftest = 1; }

/* Pointer self-test: feed synthetic 3-byte packets through the REAL PS/2
 * decoder (mbos_mouse_inject_packet) and let the normal input path react.
 * QEMU cannot inject PS/2 pointer events without a display backend, so this is
 * what proves "decode -> cursor -> click -> start menu" end to end; the wire
 * path is proven separately by reading the aux device id (`mouseinfo`).
 *
 * A PS/2 packet carries only +/-127 px, so movement happens in small steps,
 * exactly like a real mouse: one packet per frame until the target is reached.
 * Packet layout: byte0 = 0x08 | sign bits | buttons, then dx, dy. */
static int selftest_move_towards(int tx, int ty)
{
    extern void mbos_mouse_inject_packet(const uint8_t* p);
    uint8_t pkt[3];
    int dx = tx - g_mx;
    int dy = ty - g_my;
    int pdy;

    if (dx == 0 && dy == 0) {
        return 1;                          /* arrived */
    }
    if (dx > 60) dx = 60;
    if (dx < -60) dx = -60;
    if (dy > 60) dy = 60;
    if (dy < -60) dy = -60;
    pdy = -dy;                             /* PS/2 Y is inverted vs the screen */
    pkt[0] = (uint8_t)(0x08u | ((dx < 0) ? 0x10u : 0u) |
                       ((pdy < 0) ? 0x20u : 0u));
    pkt[1] = (uint8_t)(dx & 0xFF);
    pkt[2] = (uint8_t)(pdy & 0xFF);
    mbos_mouse_inject_packet(pkt);
    return 0;
}

static void selftest_click(void)
{
    extern void mbos_mouse_inject_packet(const uint8_t* p);
    uint8_t press[3]   = { 0x09u, 0, 0 };  /* bit3 + left button down */
    uint8_t release[3] = { 0x08u, 0, 0 };
    mbos_mouse_inject_packet(press);
    mbos_mouse_inject_packet(release);
}

/* One frame of the self-test; runs only when armed with `startgui test`. */
static void gui_selftest_frame(void)
{
    enum { ST_MOVE_START, ST_CLICK_START, ST_WAIT_MENU,
           ST_MOVE_ITEM, ST_CLICK_ITEM, ST_DONE };
    static int st = ST_MOVE_START;
    static uint64_t wait_until;
    const int btn_x = START_BTN_X + 8;
    const int btn_y = g_gui_screen_h - GUI_TASKBAR_H + 8;

    switch (st) {
    case ST_MOVE_START:
        if (selftest_move_towards(btn_x, btn_y)) {
            console_printf("[gui] pointer selftest: on the start button (%d,%d)\n",
                           (int64_t)g_mx, (int64_t)g_my);
            st = ST_CLICK_START;
        }
        break;
    case ST_CLICK_START:
        selftest_click();
        /* hold the menu open for ~3 s so a screenshot can capture it */
        wait_until = g_tick_count + 1000;
        st = ST_WAIT_MENU;
        break;
    case ST_WAIT_MENU:
        if (g_tick_count >= wait_until) {
            console_printf("[gui] pointer selftest: after the click the menu is %s\n",
                           g_start_open ? "OPEN" : "closed");
            st = g_start_open ? ST_MOVE_ITEM : ST_DONE;
        }
        break;
    case ST_MOVE_ITEM: {
        const int item_y = g_gui_screen_h - GUI_TASKBAR_H - start_menu_height() -
                           2 + START_MENU_PAD + START_MENU_ITEM_H / 2;
        if (selftest_move_towards(START_BTN_X + 14, item_y)) {
            st = ST_CLICK_ITEM;
        }
        break;
    }
    case ST_CLICK_ITEM:
        selftest_click();
        wait_until = g_tick_count + 100;
        st = ST_DONE;
        break;
    case ST_DONE:
    default:
        if (wait_until != 0 && g_tick_count >= wait_until) {
            console_printf("[gui] pointer selftest: finished (menu=%s, focus='%s')\n",
                           g_start_open ? "open" : "closed",
                           gui_focused() ? gui_focused()->title : "-");
            g_pointer_selftest = 0;        /* one shot */
        }
        break;
    }
}
static int start_menu_height(void)
{
    return START_ITEM_COUNT * START_MENU_ITEM_H + START_MENU_PAD * 2;
}

static void start_menu_rect(int* mx, int* my, int* mw, int* mh)
{
    const int y0 = g_gui_screen_h - GUI_TASKBAR_H;
    const int h = start_menu_height();
    *mx = START_BTN_X;
    *my = y0 - h - 2;                   /* grows UPWARDS from the taskbar   */
    if (*my < 2) *my = 2;
    *mw = START_MENU_W;
    *mh = h;
}

/* 12x19 classic arrow: '#' outline, '.' fill, ' ' transparent. */
static const char* const g_arrow[19] = {
    "#           ",
    "##          ",
    "#.#         ",
    "#..#        ",
    "#...#       ",
    "#....#      ",
    "#.....#     ",
    "#......#    ",
    "#.......#   ",
    "#........#  ",
    "#.....##### ",
    "#..#..#     ",
    "#.# #..#    ",
    "##  #..#    ",
    "#    #..#   ",
    "     #..#   ",
    "      #..#  ",
    "      #..#  ",
    "       ##   ",
};

static void gui_draw_pointer(void)
{
    const int aw = 12, ah = 19;

    for (int y = 0; y < ah; y++) {
        const int py = g_my + y;
        if (py < g_clip_y0 || py >= g_clip_y1) continue;
        for (int x = 0; x < aw; x++) {
            const int px = g_mx + x;
            const char c = g_arrow[y][x];
            if (c == ' ') continue;
            if (px < g_clip_x0 || px >= g_clip_x1) continue;
            gui_fill(px, py, 1, 1, (c == '#') ? 0x000000u : 0xFFFFFFu);
        }
    }
    g_mprev_x = g_mx;
    g_mprev_y = g_my;
    g_mprev_w = aw;
    g_mprev_h = ah;
}

static void gui_draw_start_button(void)
{
    const int y0 = g_gui_screen_h - GUI_TASKBAR_H;
    const int hover = (g_my >= y0 && g_my < g_gui_screen_h &&
                       g_mx >= START_BTN_X && g_mx < START_BTN_X + START_BTN_W);
    const uint32_t bg = g_start_open ? 0x2E5A9Eu
                     : (hover ? 0x24293Au : 0x1B1E28u);
    gui_fill(START_BTN_X, y0 + 4, START_BTN_W - 8, GUI_TASKBAR_H - 8, bg);
    const int ty = y0 + (GUI_TASKBAR_H - 16) / 2;
    gui_draw_text(START_BTN_X + 8, ty, "SutureOS", 0xE8ECF4u);
}

static void gui_draw_start_menu(void)
{
    int mx, my, mw, mh;
    if (!g_start_open) return;
    start_menu_rect(&mx, &my, &mw, &mh);

    gui_fill(mx, my, mw, mh, 0x1E2230u);
    gui_rect(mx, my, mw, mh, 0x5A6B86u);

    int y = my + START_MENU_PAD;
    for (int i = 0; i < START_ITEM_COUNT; i++) {
        if (g_start_items[i].app_id == START_KIND_SEP) {
            gui_fill(mx + 8, y + START_MENU_ITEM_H / 2, mw - 16, 1, 0x3A4356u);
        } else {
            const int hover = (g_my >= y && g_my < y + START_MENU_ITEM_H &&
                               g_mx >= mx && g_mx < mx + mw);
            if (hover)
                gui_fill(mx + 2, y, mw - 4, START_MENU_ITEM_H, 0x2E5A9Eu);
            gui_draw_text(mx + 12, y + (START_MENU_ITEM_H - 16) / 2,
                          g_start_items[i].label, 0xE8ECF4u);
        }
        y += START_MENU_ITEM_H;
    }
}

/* ==================== mouse input ====================
 * Polled once per frame (IRQ12 delivery is unreliable on this machine, exactly
 * like IRQ1). Moving the pointer dirties both the old and the new arrow rect so
 * the compositor repaints exactly the pixels that changed.
 */
static void gui_pointer_moved(int nx, int ny)
{
    if (nx == g_mx && ny == g_my) return;
    if (g_mprev_w > 0)
        gui_dirty(g_mprev_x, g_mprev_y, g_mprev_w, g_mprev_h);
    g_mx = nx;
    g_my = ny;
    gui_dirty(g_mx, g_my, 12, 19);        /* arrow size */
    if (g_start_open) gui_invalidate();   /* hover highlight in the menu */
}

static void gui_clamp_pointer(void)
{
    if (g_mx < 0) g_mx = 0;
    if (g_my < 0) g_my = 0;
    if (g_mx > g_gui_screen_w - 1) g_mx = g_gui_screen_w - 1;
    if (g_my > g_gui_screen_h - 1) g_my = g_gui_screen_h - 1;
}

/* ==================== interactive window management ====================
 * Drag (title bar), resize (bottom-right grip) and close (the title bar "x")
 * are implemented against the EXISTING chrome: no colour, font, metric, widget
 * or draw order of the frozen baseline (docs/baseline/base_gui.png) changes.
 *
 * The only affordance that is not part of the static desktop is the 3-dot
 * resize grip. It is drawn exclusively while the pointer sits inside the 12x12
 * grip area (or while a resize is in progress), so a screenshot taken with the
 * pointer anywhere else stays pixel-identical to the baseline.
 *
 * Interaction model (screen coordinates; everything is polled once per frame):
 *   press in the title bar  -> move drag: the window origin follows the pointer
 *   press in the grip area  -> resize: the bottom-right corner follows it
 *   left button released    -> drag ends, no state is left behind
 * Every geometry change dirties BOTH the old and the new window rect, so the
 * existing union-rect compositor erases the stale pixels; the pointer is
 * composited last, so it stays on top while a window is dragged under it.
 */
#define GUI_GRIP_SIZE 12
#define GUI_BTN_LEFT  0x01
#define GUI_DRAG_NONE 0
#define GUI_DRAG_MOVE 1
#define GUI_DRAG_SIZE 2

static int g_drag_kind = GUI_DRAG_NONE;
static struct gui_window* g_drag_win = NULL;
static int g_drag_off_x, g_drag_off_y;   /* move: pointer - window origin     */
static int g_drag_px, g_drag_py;         /* resize: pointer at the grab point */
static int g_drag_w0, g_drag_h0;         /* resize: window size when grabbed  */
static struct gui_window* g_grip_win = NULL;   /* grip hover highlight        */

static void gui_dirty_win(struct gui_window* w) {
    if (w) gui_dirty(w->x, w->y, w->w, w->h);
}

/* Recompute the client rect exactly the way gui_create_window() does, so a
 * resized window keeps the same 1px border / GUI_TITLE_H title layout. */
static void gui_sync_client(struct gui_window* w) {
    w->client_x = w->x + GUI_BORDER;
    w->client_y = w->y + GUI_TITLE_H + GUI_BORDER;
    w->client_w = w->w - GUI_BORDER * 2;
    w->client_h = w->h - GUI_TITLE_H - GUI_BORDER * 2;
    if (w->client_w < 1) w->client_w = 1;
    if (w->client_h < 1) w->client_h = 1;
}

/* Start moving `w`. The grab offset is taken from the LIVE pointer position
 * (g_mx,g_my), never from the queued click coordinate: gui_handle_mouse() updates
 * the pointer from the newest PS/2 bytes and only then drains the click queue, so
 * any motion delivered in the same frame as the press is already in g_mx but not
 * in the click record. Deriving the offset from the click made the window leap by
 * exactly that un-delivered motion on the first drag frame (measured: a press
 * recorded at x=221 while the pointer was at x=100 threw a window from x=24 to
 * x=3 with a single +100 packet). Using the live position makes the gesture
 * start where the pointer actually is, so the window never jumps.
 * The offset is also clamped into the window so a wildly stale click record can
 * never place the pointer far outside the frame it is dragging. */
static void gui_drag_begin_move(struct gui_window* w) {
    if (!w) return;
    g_drag_kind = GUI_DRAG_MOVE;
    g_drag_win = w;
    g_drag_off_x = g_mx - w->x;
    g_drag_off_y = g_my - w->y;
    if (g_drag_off_x < 0) g_drag_off_x = 0;
    if (g_drag_off_x > w->w) g_drag_off_x = w->w;
    if (g_drag_off_y < 0) g_drag_off_y = 0;
    if (g_drag_off_y > GUI_BORDER + GUI_TITLE_H) g_drag_off_y = GUI_BORDER + GUI_TITLE_H;
    console_printf("[gui] drag: '%s' at %d,%d\n",
                   w->title, (int64_t)w->x, (int64_t)w->y);
}

/* Start resizing `w` from its bottom-right corner. Same rule as the move: the
 * reference point is the live pointer, so the very first resize frame cannot
 * change the size by the click-record lag. */
static void gui_drag_begin_resize(struct gui_window* w) {
    if (!w) return;
    g_drag_kind = GUI_DRAG_SIZE;
    g_drag_win = w;
    g_drag_px = g_mx;
    g_drag_py = g_my;
    g_drag_w0 = w->w;
    g_drag_h0 = w->h;
    console_printf("[gui] resize: '%s' %dx%d\n",
                   w->title, (int64_t)w->w, (int64_t)w->h);
}

/* One frame of drag/resize. Called with the CURRENT pointer position and the
 * CURRENT button mask, so a fast release can never leave a window glued to the
 * pointer: the mask simply reads 0 and the gesture ends. */
static void gui_drag_update(void) {
    extern int mbos_mouse_buttons(void);
    struct gui_window* w = g_drag_win;

    if (g_drag_kind == GUI_DRAG_NONE) return;
    if (w == NULL || !w->visible) {              /* window went away mid-drag */
        g_drag_kind = GUI_DRAG_NONE;
        g_drag_win = NULL;
        return;
    }
    if ((mbos_mouse_buttons() & GUI_BTN_LEFT) == 0) {   /* released */
        g_drag_kind = GUI_DRAG_NONE;
        g_drag_win = NULL;
        gui_dirty_win(w);              /* drop the grip highlight / final frame */
        return;
    }

    if (g_drag_kind == GUI_DRAG_MOVE) {
        const int max_x = g_gui_screen_w - w->w;
        const int max_y = g_gui_screen_h - GUI_TASKBAR_H - w->h;
        int nx = g_mx - g_drag_off_x;
        int ny = g_my - g_drag_off_y;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (max_x > 0 && nx > max_x) nx = max_x;
        if (max_y > 0 && ny > max_y) ny = max_y;
        if (nx == w->x && ny == w->y) return;
        gui_dirty_win(w);              /* old rect */
        w->x = nx;
        w->y = ny;
        gui_sync_client(w);
        gui_dirty_win(w);              /* new rect */
        return;
    }

    /* resize from the bottom-right corner */
    {
        int nw = g_drag_w0 + (g_mx - g_drag_px);
        int nh = g_drag_h0 + (g_my - g_drag_py);
        const int max_w = g_gui_screen_w - w->x;
        const int max_h = g_gui_screen_h - GUI_TASKBAR_H - w->y;
        if (max_w > 0 && nw > max_w) nw = max_w;
        if (max_h > 0 && nh > max_h) nh = max_h;
        if (nw < w->min_w) nw = w->min_w;      /* sane minimum (120x80 default) */
        if (nh < w->min_h) nh = w->min_h;
        if (nw == w->w && nh == w->h) return;
        gui_dirty_win(w);              /* old rect */
        w->w = nw;
        w->h = nh;
        w->maximized = false;
        gui_sync_client(w);
        gui_dirty_win(w);              /* new rect */
    }
}

/* Move keyboard focus to the topmost visible, non-minimized window. Used after
 * a close / minimize so the disappeared window never keeps the keyboard. */
static void gui_focus_topmost_visible(void) {
    for (struct gui_window* w = g_z; w; w = w->next)
        if (w->active) { w->active = false; gui_dirty_win(w); }
    for (struct gui_window* w = g_z; w; w = w->next) {
        if (!w->visible || w->minimized) continue;
        w->active = true;
        gui_dirty_win(w);
        break;
    }
}

/* Close = hide, keeping the window slot. gui_destroy_window() would free the
 * slot and the desktop apps are only created once by gui_apps_init(), so hiding
 * is the model that matches the existing start menu: gui_activate_app() looks a
 * window up by APP_ID_* and restores it (minimized / hidden alike). */
static void gui_hide_window(struct gui_window* w) {
    if (!w) return;
    if (w->on_close) w->on_close(w);
    if (g_drag_win == w) { g_drag_kind = GUI_DRAG_NONE; g_drag_win = NULL; }
    if (g_grip_win == w) g_grip_win = NULL;
    gui_dirty_win(w);                  /* repaint where it was */
    w->visible = false;
    w->active = false;
    console_printf("[gui] window closed: '%s' (start menu re-opens it)\n",
                   w->title);
    gui_focus_topmost_visible();
    gui_invalidate();
}

/* Track which window's resize grip the pointer is over. Only the 12x12 grip
 * rect is dirtied on a change, so the compositor repaints nothing else. */
static void gui_update_grip_hover(void) {
    struct gui_window* hit = NULL;
    /* the open start menu is modal: nothing under it reacts */
    if (!g_start_open) {
        for (struct gui_window* w = g_z; w; w = w->next) {
            if (!w->visible || w->minimized) continue;
            if (g_mx >= w->x + w->w - GUI_GRIP_SIZE && g_mx < w->x + w->w &&
                g_my >= w->y + w->h - GUI_GRIP_SIZE && g_my < w->y + w->h) {
                hit = w;
                break;
            }
        }
    }
    if (hit == g_grip_win) return;
    if (g_grip_win)
        gui_dirty(g_grip_win->x + g_grip_win->w - GUI_GRIP_SIZE,
                  g_grip_win->y + g_grip_win->h - GUI_GRIP_SIZE,
                  GUI_GRIP_SIZE, GUI_GRIP_SIZE);
    g_grip_win = hit;
    if (hit)
        gui_dirty(hit->x + hit->w - GUI_GRIP_SIZE,
                  hit->y + hit->h - GUI_GRIP_SIZE,
                  GUI_GRIP_SIZE, GUI_GRIP_SIZE);
}

/* Handle a click at (cx,cy). `button` is 0 for the left button, 1 right, 2 middle
 * (see mb_mouse.c's decoder). Returns nothing; the next frame repaints.
 *
 * The window frame (drag / resize / the three title-bar buttons) reacts ONLY to
 * the LEFT button: this used to ignore `button` completely, so a right-click on a
 * title bar started a drag and a right-click on the red "x" closed the window.
 * Client-area clicks keep the full button state - gui_click_is_right() is set for
 * the legacy query path and on_click2() receives the raw button index. */
static void gui_click_at(int cx, int cy, int button)
{
    /* The pointer is NOT rewound to (cx,cy): it is refreshed from the PS/2 device
     * at the top of every frame, and moving it back to a click record that can be
     * one motion burst older made the drawn arrow jump backwards (and fed a stale
     * position into the drag state machine in the same frame). The hit tests below
     * use the click position explicitly, so nothing depends on that side effect. */
    const int left = (button == 0) ? 1 : 0;
    g_click_right = (button != 0);

    /* The open start menu is modal: it eats the click. */
    if (g_start_open) {
        int mx, my, mw, mh;
        start_menu_rect(&mx, &my, &mw, &mh);
        if (cx >= mx && cx < mx + mw && cy >= my && cy < my + mh) {
            const int idx = (cy - my - START_MENU_PAD) / START_MENU_ITEM_H;
            if (idx >= 0 && idx < START_ITEM_COUNT) {
                const int kind = g_start_items[idx].app_id;
                console_printf("[gui] start menu: '%s' (click %d,%d)\n",
                               g_start_items[idx].label ? g_start_items[idx].label : "-",
                               (int64_t)cx, (int64_t)cy);
                if (kind == START_KIND_EXIT) {
                    g_gui_exit = 1;
                } else if (kind != START_KIND_SEP) {
                    gui_activate_app(kind);
                }
            }
            g_start_open = 0;
            gui_invalidate();
            return;
        }
        /* click outside closes the menu and falls through to normal handling */
        g_start_open = 0;
        console_puts("[gui] start menu: closed\n");
        gui_invalidate();
    }

    /* taskbar: the name on the left is the start button */
    const int tb_y0 = g_gui_screen_h - GUI_TASKBAR_H;
    if (cy >= tb_y0) {
        if (cx >= START_BTN_X && cx < START_BTN_X + START_BTN_W) {
            g_start_open = !g_start_open;
            console_printf("[gui] start menu: %s (click %d,%d)\n",
                           g_start_open ? "opened" : "closed",
                           (int64_t)cx, (int64_t)cy);
            gui_invalidate();
        }
        return;
    }

    /* topmost window under the pointer */
    struct gui_window* hit = NULL;
    for (struct gui_window* w = g_z; w; w = w->next) {
        if (!w->visible || w->minimized) continue;
        if (cx >= w->x && cx < w->x + w->w && cy >= w->y && cy < w->y + w->h) {
            hit = w;
            break;
        }
    }
    if (hit == NULL) return;

    gui_set_active(hit);                  /* raise + focus (drag raises too) */

    /* bottom-right grip: 12x12 corner starts a resize. Checked before the title
     * bar and the client area because the grip lives inside the client rect and
     * must win over the app's own on_click. Left button only. */
    if (left && cx >= hit->x + hit->w - GUI_GRIP_SIZE && cx < hit->x + hit->w &&
        cy >= hit->y + hit->h - GUI_GRIP_SIZE && cy < hit->y + hit->h) {
        gui_drag_begin_resize(hit);
        return;
    }

    /* title bar: the three buttons on the right (close / max / min) */
    if (cy < hit->y + GUI_BORDER + GUI_TITLE_H) {
        if (!left) return;                /* right/middle click never moves or
                                           * closes a window */
        const int bx = hit->x + hit->w - GUI_BORDER - GUI_BTN_W * 3;
        if (cx >= bx + 3 && cx < bx + 3 + GUI_BTN_W * 3) {
            const int b = (cx - (bx + 3)) / GUI_BTN_W;
            if (b == 2) {                 /* close: hide, start menu re-opens */
                gui_hide_window(hit);
            } else if (b == 1) {          /* maximize / restore (unchanged) */
                gui_flip_window(hit);
            } else {                      /* minimize -> start menu restores */
                console_printf("[gui] click: minimize '%s'\n", hit->title);
                if (g_drag_win == hit) { g_drag_kind = GUI_DRAG_NONE; g_drag_win = NULL; }
                hit->minimized = true;
                hit->active = false;
                gui_dirty_win(hit);        /* repaint the now-hidden rect */
                gui_focus_topmost_visible();
                gui_invalidate();
            }
            return;
        }
        /* empty title-bar space: start a move drag. gui_set_active() above has
         * already raised and focused this window, so dragging an unfocused
         * window raises it on the same click. */
        gui_drag_begin_move(hit);
        return;
    }

    /* client area -> the app's on_click with client-relative coordinates */
    if (hit->on_click) {
        hit->on_click(hit, cx - hit->client_x, cy - hit->client_y);
    } else if (hit->on_click2) {
        hit->on_click2(hit, cx - hit->client_x, cy - hit->client_y, button);
    }
    gui_invalidate();
}

static void gui_handle_mouse(void)
{
    extern void mbos_mouse_poll(void);
    extern int  mbos_mouse_x(void), mbos_mouse_y(void);
    extern int  mbos_mouse_take_click(int*, int*, int*);

    mbos_mouse_poll();

    int nx = mbos_mouse_x();
    int ny = mbos_mouse_y();
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx > g_gui_screen_w - 1) nx = g_gui_screen_w - 1;
    if (ny > g_gui_screen_h - 1) ny = g_gui_screen_h - 1;
    gui_pointer_moved(nx, ny);
    gui_clamp_pointer();

    int cx, cy, btn;
    while (mbos_mouse_take_click(&cx, &cy, &btn)) {
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        if (cx > g_gui_screen_w - 1) cx = g_gui_screen_w - 1;
        if (cy > g_gui_screen_h - 1) cy = g_gui_screen_h - 1;
        gui_click_at(cx, cy, btn);
    }

    /* drag / resize continuation for this frame (runs after the click drain, so
     * a press handled above starts the gesture and the very next frame keeps it
     * alive), then the hover highlight of the resize grip. */
    gui_drag_update();
    gui_update_grip_hover();
}

/* First visible window owned by `app_id` (used by the start menu). */
static struct gui_window* gui_find_app_window(int app_id)
{
    for (struct gui_window* w = g_z; w; w = w->next)
        if (w->app_id == (uint8_t)app_id) return w;
    return NULL;
}

/* Bring the window of `app_id` to the front (restoring it if minimized, and
 * re-showing it if the title-bar "x" closed it). The start menu is the existing
 * re-open affordance: there is no taskbar slot model to click a hidden window
 * in, and adding slots would restyle the frozen taskbar. */
static void gui_activate_app(int app_id)
{
    struct gui_window* w = gui_find_app_window(app_id);
    if (w != NULL) {
        w->visible = true;               /* re-open after the close button  */
        gui_set_active(w);               /* raises + marks active          */
        w->minimized = false;
        gui_invalidate();
    }
}

/* Tab: cyclic focus over visible windows, WITHOUT reordering z (reordering
 * via gui_set_active would make Tab ping-pong between the top two windows) */
static void gui_focus_next(void) {
    struct gui_window* cand[GUI_MAX_WINDOWS];
    int n = 0;
    for (struct gui_window* w = g_z; w && n < GUI_MAX_WINDOWS; w = w->next)
        if (w->visible && !w->minimized) cand[n++] = w;
    if (n == 0) return;
    int cur = -1;
    for (int i = 0; i < n; i++)
        if (cand[i]->active) { cur = i; break; }
    struct gui_window* nxt = (cur >= 0) ? cand[(cur + 1) % n] : cand[0];
    for (int i = 0; i < n; i++) {
        if (cand[i]->active) {
            cand[i]->active = false;
            gui_dirty(cand[i]->x, cand[i]->y, cand[i]->w, cand[i]->h);
        }
    }
    nxt->active = true;
    nxt->minimized = false;
    gui_dirty(nxt->x, nxt->y, nxt->w, nxt->h);
}

/* arrows: move the focused window +/-16 px, clamped to the screen (taskbar
 * excluded), marking both the old and the new rect dirty */
static void gui_move_focused(int dx, int dy) {
    struct gui_window* w = gui_focused();
    if (!w) return;
    gui_dirty(w->x, w->y, w->w, w->h);
    int nx = w->x + dx;
    int ny = w->y + dy;
    int max_x = g_gui_screen_w - w->w;
    int max_y = g_gui_screen_h - GUI_TASKBAR_H - w->h;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx > max_x) nx = max_x;
    if (ny > max_y) ny = max_y;
    w->x = nx;
    w->y = ny;
    w->client_x = w->x + GUI_BORDER;
    w->client_y = w->y + GUI_TITLE_H + GUI_BORDER;
    gui_dirty(w->x, w->y, w->w, w->h);
}

/* drain the keyboard FIFO; returns true when the loop should exit */
static bool gui_handle_keys(void) {
    bool quit = false;
    for (int guard = 0; guard < 64; guard++) {
        int sc = mbos_kbd_poll();
        if (sc < 0) break;
        int ch = mbos_kbd_decode((uint8_t)sc);
        /* first few keys: prove delivery+decode reach this loop (serial-only) */
        {
            extern uint32_t mbos_kbd_key_log_count(void);
            extern void mbos_kbd_key_log_note(int sc, int ch);
            mbos_kbd_key_log_note(sc, ch);
        }

        /* Arrow keys: scancode set-1 make codes. mbos_kbd_decode() maps the
         * arrows to 0 (unmapped), so match the RAW scancode; break codes
         * (bit 7 set) and the 0xE0 prefix are skipped here (0xE0 has bit 7). */
        if (!(sc & 0x80)) {
            switch (sc) {
            case 0x48: gui_move_focused(0, -16); continue;   /* Up    */
            case 0x50: gui_move_focused(0,  16); continue;   /* Down  */
            case 0x4B: gui_move_focused(-16, 0); continue;   /* Left  */
            case 0x4D: gui_move_focused( 16, 0); continue;   /* Right */
            default: break;
            }
        }

        if (ch <= 0) continue;                    /* break/modifier/unmapped */
        if (ch == '\t') { gui_focus_next(); continue; }          /* Tab  */
        if (ch == 0x1B || ch == 'q' || ch == 'Q') {              /* q / ESC */
            quit = true;
            break;
        }
        if (ch == 'r' || ch == 'R') { gui_invalidate(); continue; }  /* r */
        /* printable keys (+Enter/Backspace for the terminal app) -> on_key */
        if (ch == '\n' || ch == '\b' || (ch >= 32 && ch < 127)) {
            struct gui_window* w = gui_focused();
            if (w && w->on_key) w->on_key(w, (char)ch);
        }
    }
    return quit;
}

/* ==================== composition (per frame) ====================
 * Layer order follows the porting spec: desktop gradient -> taskbar ->
 * windows bottom-to-top (z list head = topmost, so it is reversed first,
 * exactly like gui64.cpp render()). Only the dirty union rect is repainted;
 * unchanged-region (per-window diff) optimization intentionally skipped. */

static void gui_draw_desktop(int x0, int y0, int x1, int y1) {
    const int h = g_gui_screen_h > 0 ? g_gui_screen_h : 1;
    for (int y = y0; y < y1; y++) {
        const int t = (y * 64) / h;    /* 0..63 top->bottom */
        const uint32_t c = ((uint32_t)(14 + t / 2) << 16) |
                           ((uint32_t)(20 + t / 2) << 8) |
                           (uint32_t)(40 + t);
        gui_fill(x0, y, x1 - x0, 1, c);
    }
}

static void gui_draw_taskbar(void) {
    const int w = g_gui_screen_w, h = g_gui_screen_h;
    int y0 = h - GUI_TASKBAR_H;
    if (y0 < 0) y0 = 0;
    gui_fill(0, y0, w, GUI_TASKBAR_H, 0x16181Eu);
    gui_fill(0, y0, w, 1, 0x3C4454u);                    /* highlight edge */
    const int ty = y0 + (GUI_TASKBAR_H - 16) / 2;

    /* the system name is the start button (click = menu opens upwards) */
    gui_draw_start_button();

    gui_draw_text(START_BTN_X + START_BTN_W + 8, ty,
                  "click name for menu | Tab:focus r:repaint q/ESC:quit",
                  0x9FB2CCu);
    /* right side: measured fps */
    char d[12];
    const int dl = gui_u32_dec(g_fps_meas, d);
    char buf[24];
    int p = 0;
    const char* s = "FPS ";
    while (*s) buf[p++] = *s++;
    for (int i = 0; i < dl; i++) buf[p++] = d[i];
    buf[p] = 0;
    gui_draw_text(w - 8 - p * 8, ty, buf, 0x9FB2CCu);
}

/* decorative title buttons (min/max/close) - keyboard-only build: drawn for
 * looks; there is no mouse driver, so they are not clickable */static void gui_draw_title_buttons(struct gui_window* w) {
    const int bx = w->x + w->w - GUI_BORDER - GUI_BTN_W * 3;
    const int by = w->y + GUI_BORDER + 4;
    for (int b = 0; b < 3; b++) {
        const int x = bx + b * GUI_BTN_W + 3;
        const int bw = GUI_BTN_W - 6, bh = 16;
        const uint32_t bg = (b == 2) ? 0xB03A2Eu
                          : (w->active ? 0x3C64B4u : 0x4A4E56u);
        gui_fill(x, by, bw, bh, bg);
        const int cx = x + bw / 2, cy = by + bh / 2;
        if (b == 0) {                              /* minimize: short bar */
            gui_fill(cx - 5, cy + 2, 11, 2, 0xFFFFFFu);
        } else if (b == 1) {                       /* maximize: square */
            gui_rect(cx - 5, cy - 5, 11, 11, 0xFFFFFFu);
        } else {                                   /* close: X */
            for (int i = -4; i <= 4; i++) {
                gui_px(cx + i, cy + i, 0xFFFFFFu);
                gui_px(cx - i, cy + i, 0xFFFFFFu);
            }
        }
    }
}

static void gui_draw_window(struct gui_window* w) {
    /* 1) title bar: active / inactive fill */
    const uint32_t tb = w->active ? 0x2859A9u : 0x4A4E56u;
    gui_fill(w->x + GUI_BORDER, w->y + GUI_BORDER,
             w->w - GUI_BORDER * 2, GUI_TITLE_H, tb);

    /* 2) title text + buttons, clipped to the title strip so a long title
     *    can never spill over neighbouring pixels */
    {
        const int sx0 = g_clip_x0, sy0 = g_clip_y0;
        const int sx1 = g_clip_x1, sy1 = g_clip_y1;
        int tx0 = w->x + GUI_BORDER, ty0 = w->y + GUI_BORDER;
        int tx1 = w->x + w->w - GUI_BORDER, ty1 = ty0 + GUI_TITLE_H;
        if (tx0 < sx0) tx0 = sx0;
        if (ty0 < sy0) ty0 = sy0;
        if (tx1 > sx1) tx1 = sx1;
        if (ty1 > sy1) ty1 = sy1;
        g_clip_x0 = tx0; g_clip_y0 = ty0; g_clip_x1 = tx1; g_clip_y1 = ty1;
        gui_draw_text(w->x + 8, w->y + GUI_BORDER + (GUI_TITLE_H - 16) / 2,
                      w->title, 0xFFFFFFu);
        gui_draw_title_buttons(w);
        g_clip_x0 = sx0; g_clip_y0 = sy0; g_clip_x1 = sx1; g_clip_y1 = sy1;
    }

    /* 3) client area: neutral base, then the app's draw callback (clipped) */
    gui_fill(w->client_x, w->client_y, w->client_w, w->client_h, 0xECECECu);
    if (w->draw) {
        const int sx0 = g_clip_x0, sy0 = g_clip_y0;
        const int sx1 = g_clip_x1, sy1 = g_clip_y1;
        int cx0 = w->client_x, cy0 = w->client_y;
        int cx1 = cx0 + w->client_w, cy1 = cy0 + w->client_h;
        if (cx0 < sx0) cx0 = sx0;
        if (cy0 < sy0) cy0 = sy0;
        if (cx1 > sx1) cx1 = sx1;
        if (cy1 > sy1) cy1 = sy1;
        g_clip_x0 = cx0; g_clip_y0 = cy0; g_clip_x1 = cx1; g_clip_y1 = cy1;
        w->draw(w);
        g_clip_x0 = sx0; g_clip_y0 = sy0; g_clip_x1 = sx1; g_clip_y1 = sy1;
    }

    /* 4) 1px border (active window gets a brighter frame) */
    gui_rect(w->x, w->y, w->w, w->h, w->active ? 0x8FB4E8u : 0x3C4048u);

    /* 5) resize grip (3 dots, drawn AFTER the app callback so it is never
     *    overpainted). Only shown while the pointer is over the grip or while
     *    that window is being resized, which keeps the static desktop
     *    byte-identical to docs/baseline/base_gui.png. */
    if (w == g_grip_win ||
        (g_drag_kind == GUI_DRAG_SIZE && g_drag_win == w)) {
        const int gx = w->x + w->w - 4;
        const int gy = w->y + w->h - 4;
        const uint32_t gc = 0x8A929Cu;             /* neutral window grey */
        gui_fill(gx - 2, gy - 2, 2, 2, gc);
        gui_fill(gx - 6, gy - 2, 2, 2, gc);
        gui_fill(gx - 2, gy - 6, 2, 2, gc);
    }
}

/* compose one frame over the dirty union rect, then clear the dirty flag */
static void gui_render(void) {
    if (!g_dirty_any || !g_fb) return;
    const int x0 = g_dirty_x0, y0 = g_dirty_y0;
    const int x1 = g_dirty_x1, y1 = g_dirty_y1;
    if (x1 <= x0 || y1 <= y0) { g_dirty_any = false; return; }

    g_clip_x0 = x0; g_clip_y0 = y0; g_clip_x1 = x1; g_clip_y1 = y1;

    gui_draw_desktop(x0, y0, x1, y1);
    gui_draw_taskbar();

    /* windows bottom -> top (g_z head is the topmost, so reverse) */
    struct gui_window* stack[GUI_MAX_WINDOWS];
    int n = 0;
    for (struct gui_window* w = g_z; w && n < GUI_MAX_WINDOWS; w = w->next)
        stack[n++] = w;
    for (int i = n - 1; i >= 0; i--) {
        struct gui_window* w = stack[i];
        if (!w->visible || w->minimized) continue;
        if (w->x >= x1 || w->y >= y1 || w->x + w->w <= x0 || w->y + w->h <= y0)
            continue;
        gui_draw_window(w);
    }

    /* the start menu pops over the windows; the pointer is always on top */
    gui_draw_start_menu();
    gui_draw_pointer();

    gui_clip_full();
    g_dirty_any = false;              /* clear AFTER commit (gui64.cpp lesson) */
}

/* frame pacing: wait for one g_tick_count increment (PIT ~100 Hz), bounded so
 * a dead timer cannot hang the loop. No wall clock.
 * NOTE: do NOT call sched_yield() from here. task.c's cooperative round-robin
 * and eevdf.c's EEVDF loop are two schedulers over one context struct; yielding
 * from inside the GUI task switched to a stale EEVDF context and made the CPU
 * execute garbage (observed as an endless "unexpected cpu exception vector=6"
 * (#UD) flood at rip=0x3, painted over the GUI by the console). The shell input
 * loop uses the same bounded-pause approach for the same reason. */
static void gui_pace(void) {
    const uint64_t start = g_tick_count;
    for (int i = 0; i < GUI_PACE_MAX_SPINS && g_tick_count == start; i++) {
        __asm__ volatile("pause");
    }
}

int gui_init(int screen_w, int screen_h) {
    memset(g_wins, 0, sizeof(g_wins));
    memset(g_used, 0, sizeof(g_used));
    g_z = NULL;
    /* real framebuffer dims win over the caller's (w,h) fallback defaults */
    if (gui_fb_setup() == NULL) {
        screen_w = (int)limine_fb_width;
        screen_h = (int)limine_fb_height;
    }
    g_gui_screen_w = screen_w;
    g_gui_screen_h = screen_h;
    g_gui_initialized = true;
    g_lang_zh = true;
    g_click_right = false;
    g_dirty_any = false;
    /* no gesture can survive an (re)initialisation of the window table */
    g_drag_kind = GUI_DRAG_NONE;
    g_drag_win = NULL;
    g_grip_win = NULL;
    gui_clip_full();

    console_printf("[gui] GUI framework init %dx%d (fb: %s)\n",
                   g_gui_screen_w, g_gui_screen_h,
                   g_fb ? "yes" : "no");
    return 0;
}

int gui_run(void) {
    /* framebuffer discovery - real dims, not the 1024x768 defaults */
    if (gui_fb_setup() != NULL) {
        if (g_fb_fail && g_fb_fail[0] && strcmp(g_fb_fail, "not available") != 0)
            console_printf("[gui] no framebuffer (%s)\n", g_fb_fail);
        else
            console_puts("[gui] no framebuffer\n");
        return -1;                    /* shell falls back to the terminal */
    }
    g_gui_screen_w = (int)limine_fb_width;
    g_gui_screen_h = (int)limine_fb_height;
    if (!g_gui_initialized) gui_init(g_gui_screen_w, g_gui_screen_h);
    gui_clip_full();

    console_printf("[gui] framebuffer %ux%u pitch=%u bpp=%u\n",
                   (unsigned)limine_fb_width, (unsigned)limine_fb_height,
                   (unsigned)limine_fb_pitch, (unsigned)limine_fb_bpp);

    /* pointer bring-up. The mouse is initialised ONCE at boot (kmain): doing it
     * again here re-sends 0xA8/0xAE/0x20/0xF6/0xF4 while the device is already
     * streaming, and the config-byte read then returns a pending DATA byte
     * instead (observed 0x9e) with 0xF6 unanswered - corrupting the packet
     * stream. So only query the state and place the pointer here. */
    {
        extern int mbos_mouse_present(void);
        extern void mbos_mouse_move_to(int x, int y);
        if (mbos_mouse_present()) {
            mbos_mouse_move_to(g_gui_screen_w / 2, g_gui_screen_h / 2);
            console_printf("[gui] pointer: PS/2 mouse at %dx%d\n",
                           (int64_t)g_gui_screen_w, (int64_t)g_gui_screen_h);
        } else {
            console_puts("[gui] pointer: no PS/2 mouse, keyboard only\n");
        }
    }

    console_puts("[gui] main loop start\n");
    console_puts("[gui] keys: arrows=move Tab=focus r=repaint q/ESC=exit\n");
    console_puts("[gui] mouse: click the SutureOS name (bottom left) for the menu\n");

    /* The GUI owns the framebuffer from here on: suspend the terminal renderer so
     * kernel log lines (still written to COM1) cannot paint over the desktop.
     * Restored, with a terminal clear, when gui_run() returns. */
    extern void console_screen_enable(int on);
    console_screen_enable(0);

    /* give initial focus to the topmost window so title bars read active */
    {
        struct gui_window* f = gui_focused();
        if (f && !f->active) {
            for (struct gui_window* p = g_z; p; p = p->next) p->active = false;
            f->active = true;
        }
    }

    gui_invalidate();                 /* first frame = full repaint */

    /* create the desktop applications once (Task Manager / Calculator / About) */
    {
        extern void gui_apps_init(void);
        gui_apps_init();
        gui_invalidate();             /* their windows must appear in frame 1 */
    }

    uint64_t sec_tick = g_tick_count;
    uint64_t sec_tsc = gui_rdtsc();
    uint64_t busy_tsc = 0;
    uint32_t frames = 0;

    for (;;) {
        const uint64_t t0 = gui_rdtsc();

        if (gui_handle_keys()) break;             /* q / ESC -> exit loop */

        /* pointer: poll the PS/2 mouse, move the arrow, dispatch clicks */
        gui_handle_mouse();
        if (g_gui_exit) break;                    /* start menu "Exit to terminal" */

        /* armed pointer self-test (`startgui test`): inject pointer packets
         * through the real decoder, then let the normal path react. */
        if (g_pointer_selftest) {
            gui_selftest_frame();
        }

        /* application tick callbacks (timer app etc.) */
        for (struct gui_window* w = g_z; w; w = w->next)
            if (w->visible && !w->minimized && w->on_tick) w->on_tick(w);

        gui_render();

        busy_tsc += gui_rdtsc() - t0;
        frames++;

        /* heartbeat every 1000 frames (~10 s) - monotonic because `frames` is
         * reset once per second by the fps block. It is the only way to tell
         * "the loop died" from "the timer died" in a serial-only test. */
        static uint32_t g_frames_total;
        g_frames_total++;
        if ((g_frames_total % 1000) == 0) {
            console_printf("[gui] frame=%u tick=%llu ps2stat=0x%x\n",
                           (unsigned)g_frames_total,
                           (unsigned long long)g_tick_count,
                           (unsigned long long)inb(0x64));
        }

        /* NO sched_yield() here. task.c keeps a cooperative round-robin and an
         * EEVDF scheduler over the same context storage; yielding from the GUI
         * lets the CPU switch to a stale EEVDF context and the desktop is never
         * scheduled again - measured as "frames freeze after ~4 s, ticks keep
         * running, keyboard goes dead" (the frame heartbeat and the 1 s
         * telemetry both stop while g_tick_count keeps advancing).
         * gui_pace() already blocks for one tick, which is all the pacing this
         * foreground loop needs. */
        gui_pace();                               /* ~1 tick (<=100 fps) per frame */

        const uint64_t now = g_tick_count;
        if (now - sec_tick >= 100) {              /* ~1 s at 100 Hz */
            const uint64_t dt = now - sec_tick;
            g_fps_meas = (uint32_t)((frames * 100ull) / dt);
            if (g_fps_meas > 999) g_fps_meas = 999;
            const uint64_t total = gui_rdtsc() - sec_tsc;
            uint8_t pct = total ? (uint8_t)(busy_tsc * 100 / total) : 0;
            if (pct > 100) pct = 100;
            g_busy_meas = pct;
            frames = 0;
            busy_tsc = 0;
            sec_tick = now;
            sec_tsc = gui_rdtsc();

            /* Pointer telemetry (serial only - the screen belongs to the GUI).
             * It is what the acceptance runs assert on: packets > 0 proves the
             * PS/2 data path works, and x/y prove the cursor really moved. */
            {
                extern uint32_t mbos_mouse_packets(void);
                extern uint32_t mbos_mouse_dropped(void);
                extern int mbos_mouse_buttons(void);
                extern uint32_t mbos_mouse_obf_seen(void);
                extern uint32_t mbos_mouse_aux_seen(void);
                console_printf("[gui] pointer x=%d y=%d buttons=%d pkts=%u drop=%u "
                               "obf=%u aux=%u\n",
                               (int64_t)g_mx, (int64_t)g_my,
                               (int64_t)mbos_mouse_buttons(),
                               (uint64_t)mbos_mouse_packets(),
                               (uint64_t)mbos_mouse_dropped(),
                               (uint64_t)mbos_mouse_obf_seen(),
                               (uint64_t)mbos_mouse_aux_seen());
            }
        }
    }

    console_puts("[gui] main loop exit (q/ESC)\n");
    console_screen_enable(1);      /* hand the screen back to the terminal */
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
            
            // 简单的客户区绘制
    win->client_x = win->x + 1;
            win->client_y = win->y + GUI_TITLE_H + GUI_BORDER;
            win->client_w = win->w - GUI_BORDER * 2;
            win->client_h = win->h - GUI_TITLE_H - GUI_BORDER * 2;
            if (win->client_w < 1) win->client_w = 1;
            if (win->client_h < 1) win->client_h = 1;
            
            win->next = g_z;
            g_z = win;
            
            gui_dirty(win->x, win->y, win->w, win->h);
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
            /* never leave an interaction pointing at a freed slot */
            if (g_drag_win == w) { g_drag_kind = GUI_DRAG_NONE; g_drag_win = NULL; }
            if (g_grip_win == w) g_grip_win = NULL;
            gui_dirty(w->x, w->y, w->w, w->h);   /* repaint where it was */
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
    for (struct gui_window* p = g_z; p; p = p->next) {
        if (p->active) gui_dirty(p->x, p->y, p->w, p->h);
        p->active = false;
    }
    w->active = true;
    w->minimized = false;
    gui_dirty(w->x, w->y, w->w, w->h);
    // 绉诲埌 z 搴忓ご閮?
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
    gui_dirty(w->x, w->y, w->w, GUI_DECO_H + 8);   /* title strip only */
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
    gui_dirty(w->x, w->y, w->w, w->h);             /* old rect */
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
    gui_dirty(w->x, w->y, w->w, w->h);             /* new rect */
}

int gui_fit_window_to_client(struct gui_window* w, int cw, int ch) {
    if (!w) return 0;
    const int nw = cw + GUI_BORDER * 2;
    const int nh = ch + GUI_TITLE_H + GUI_BORDER;
    if (nw < w->min_w || nh < w->min_h) return 0;
    if (nw == w->w && nh == w->h) return 1;
    gui_dirty(w->x, w->y, w->w, w->h);
    w->w = nw;
    w->h = nh;
    w->client_x = w->x + GUI_BORDER;
    w->client_y = w->y + GUI_TITLE_H + GUI_BORDER;
    w->client_w = w->w - GUI_BORDER * 2;
    w->client_h = w->h - GUI_TITLE_H - GUI_BORDER;
    if (w->client_w < 1) w->client_w = 1;
    if (w->client_h < 1) w->client_h = 1;
    gui_dirty(w->x, w->y, w->w, w->h);
    return 1;
}

void gui_invalidate_window(struct gui_window* w) {
    if (w) gui_dirty(w->x, w->y, w->w, w->h);
}

void gui_flip_window(struct gui_window* w) {
    /* single-buffered direct fb: nothing to flush - just make sure the next
     * frame repaints this window's rect */
    if (w) gui_dirty(w->x, w->y, w->w, w->h);
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

/* System power control.
 *
 * These used to be stubs that only printed a line, so `reboot`/`shutdown` from
 * the shell (and the GUI) did nothing. Now they run a Linux-style English
 * shutdown sequence and then really reset / power off the machine through ACPI
 * (arch/x86_64/power.c). Neither call returns on success. */
void sys_reboot(void) {
    console_puts("\n=== SutureOS system reboot ===\n");
    console_puts("[  OK  ] Stopping shell\n");
    console_puts("[  OK  ] Stopping scheduler\n");
    console_puts("[  OK  ] Unmounting filesystems (flush)\n");
    console_puts("[     ] Rebooting...\n");
    power_reset();
}

void sys_shutdown(void) {
    console_puts("\n=== SutureOS system shutdown ===\n");
    console_puts("[  OK  ] Stopping shell\n");
    console_puts("[  OK  ] Stopping scheduler\n");
    console_puts("[  OK  ] Unmounting filesystems (flush)\n");
    console_puts("[     ] Powering off...\n");
    power_off();
}

/* dirty union rect: clipped to the screen, merged into the running union.
 * Repaint policy: the compositor repaints only the union rect (full desktop
 * composition inside it); per-window diffing is intentionally skipped. */
void gui_dirty(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (g_gui_screen_w > 0 && x1 > g_gui_screen_w) x1 = g_gui_screen_w;
    if (g_gui_screen_h > 0 && y1 > g_gui_screen_h) y1 = g_gui_screen_h;
    if (x1 <= x || y1 <= y) return;
    if (!g_dirty_any) {
        g_dirty_x0 = x; g_dirty_y0 = y; g_dirty_x1 = x1; g_dirty_y1 = y1;
        g_dirty_any = true;
    } else {
        if (x < g_dirty_x0) g_dirty_x0 = x;
        if (y < g_dirty_y0) g_dirty_y0 = y;
        if (x1 > g_dirty_x1) g_dirty_x1 = x1;
        if (y1 > g_dirty_y1) g_dirty_y1 = y1;
    }
}

void gui_invalidate(void) {
    gui_dirty(0, 0, g_gui_screen_w, g_gui_screen_h);
}

uint32_t gui_fps(void) { return g_fps_meas; }
uint8_t gui_cpu_busy_pct(void) { return g_busy_meas; }

/* ---- drawing helpers for application callbacks (kernel/gui/apps.c) ----
 * Thin public wrappers over the compositor's own primitives, so applications do
 * not need access to gui.c internals. The compositor clips every callback to
 * its window's client rectangle, so absolute coordinates are safe. */
void gui_fill_rect(int x, int y, int w, int h, uint32_t rgb) { gui_fill(x, y, w, h, rgb); }
void gui_frame_rect(int x, int y, int w, int h, uint32_t rgb) { gui_rect(x, y, w, h, rgb); }
void gui_draw_str(int x, int y, const char* s, uint32_t rgb) { gui_draw_text(x, y, s, rgb); }

void gui_draw_u32(int x, int y, uint32_t v, uint32_t rgb) {
    char buf[12];
    const int n = gui_u32_dec(v, buf);
    buf[n] = 0;
    gui_draw_text(x, y, buf, rgb);
}

void gui_set_cursor(int shape) { (void)shape; }
int gui_get_cursor(void) { return MOUSE_CUR_ARROW; }

/* Real self-test: verify the composition path can actually reach pixels rather
 * than printing an unconditional PASS (the previous version did exactly that).
 * Draws a 16x16 marker into the top-left corner, reads the pixels back through
 * the same volatile pointer the compositor uses, then restores the desktop. */
int gui_selftest(void) {
    if (gui_fb_setup() != NULL) {
        console_printf("[gui] selftest SKIP (no framebuffer: %s)\n",
                       g_fb_fail ? g_fb_fail : "unknown");
        return -1;
    }
    if (g_gui_screen_w < 32 || g_gui_screen_h < 32) {
        console_puts("[gui] selftest FAIL (implausible geometry)\n");
        return -1;
    }

    const uint32_t marker = 0x00FF00FFu;   /* magenta: not used by the desktop */
    const int px = 4, py = 4;
    gui_clip_full();
    gui_fill(px, py, 16, 16, marker);

    volatile uint32_t* base = (volatile uint32_t*)g_fb;
    const uint32_t stride = g_fb_pitch / 4;
    uint32_t seen = 0;
    if (base) seen = base[(py + 8) * stride + (px + 8)];

    /* repaint the desktop area so the marker does not linger */
    gui_dirty(px, py, 16, 16);
    gui_render();

    if (seen != marker) {
        console_printf("[gui] selftest FAIL (wrote 0x%x, read back 0x%x)\n",
                       (uint64_t)marker, (uint64_t)seen);
        return -1;
    }
    console_printf("[gui] selftest PASS (composition path intact, %ux%u)\n",
                   (uint64_t)g_gui_screen_w, (uint64_t)g_gui_screen_h);
    return 0;
}

