/* SutureOS module: GUI framework - 移植自 VimtuOS gui64
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

/* window that keyboard actions target: active one, else topmost visible */
static struct gui_window* gui_focused(void) {
    for (struct gui_window* w = g_z; w; w = w->next)
        if (w->visible && !w->minimized && w->active) return w;
    for (struct gui_window* w = g_z; w; w = w->next)
        if (w->visible && !w->minimized) return w;
    return NULL;
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
    gui_draw_text(8, ty, "SutureOS", 0xE8ECF4u);
    gui_draw_text(8 + 9 * 8, ty,
                  " | arrows:move Tab:focus r:repaint q/ESC:quit",
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
 * looks; there is no mouse driver, so they are not clickable */
static void gui_draw_title_buttons(struct gui_window* w) {
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
    console_puts("[gui] main loop start\n");
    console_puts("[gui] keys: arrows=move Tab=focus r=repaint q/ESC=exit\n");

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

    uint64_t sec_tick = g_tick_count;
    uint64_t sec_tsc = gui_rdtsc();
    uint64_t busy_tsc = 0;
    uint32_t frames = 0;

    for (;;) {
        const uint64_t t0 = gui_rdtsc();

        if (gui_handle_keys()) break;             /* q / ESC -> exit loop */

        /* application tick callbacks (timer app etc.) */
        for (struct gui_window* w = g_z; w; w = w->next)
            if (w->visible && !w->minimized && w->on_tick) w->on_tick(w);

        gui_render();

        busy_tsc += gui_rdtsc() - t0;
        frames++;

        sched_yield();                            /* keep shell/system running */
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
            
            // 简单的客户区计算
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

void gui_set_cursor(int shape) { (void)shape; }
int gui_get_cursor(void) { return MOUSE_CUR_ARROW; }

int gui_selftest(void) {
    console_puts("[gui] selftest (stub) PASS\n");
    return 0;
}
