/* SutureOS: desktop applications for the ring-0 GUI.
 *
 * The window manager / compositor in gui.c was ported from VimtuOS gui64, but
 * no application was ever created, so the desktop came up empty. This file adds
 * three real apps on top of the public gui.h API (gui_create_window + the
 * gui_fill_rect/gui_draw_str helpers):
 *
 *   - Task Manager : live system counters (CPUs, PMM, heap, uptime, FPS, windows)
 *   - Calculator   : keyboard-driven, the only interactive app in the build
 *   - About        : what this kernel is, where its parts came from, licences
 *
 * Drawing contract (see gui.c's gui_draw_window): the compositor already clipped
 * the callback to the window's client rectangle, so apps draw with absolute
 * screen coordinates starting at w->client_x / w->client_y.
 */
#include "sutureos/version.h"
#include "gui.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>

extern volatile uint64_t g_tick_count;      /* PIT ticks, ~100 Hz */
extern uint32_t smp_online_cpu_count(void);
extern uint64_t moe_pmm_free_count(void);
extern uintptr_t lemis_heap_free_space(void);

#define APP_PIT_HZ 100u                     /* g_tick_count increments per second */

/* ---------- tiny text helpers (no snprintf in this kernel) ---------- */

static char* put_u32(char* p, uint32_t v) {
    char tmp[10];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) *p++ = tmp[--n];
    return p;
}

static char* put_str(char* p, const char* s) {
    while (*s) *p++ = *s++;
    return p;
}

/* Resizing can shrink a window down to the 120x80 minimum, so every derived
 * width used below passes through this clamp: a negative width would be dropped
 * by the drawing primitives and an unsigned wrap would paint the wrong extent.
 * Default geometry is untouched - the clamp only engages below the authored
 * window sizes. */
static int clamp_min(int v, int lo) { return v < lo ? lo : v; }

/* "label: value" line at (x,y) */
static void kv_u32(int x, int y, const char* label, uint32_t v, uint32_t rgb) {
    char buf[64];
    char* p = put_str(buf, label);
    p = put_u32(p, v);
    *p = 0;
    gui_draw_str(x, y, buf, rgb);
}

/* ==================== Task Manager ==================== */

static void tmgr_draw(struct gui_window* w) {
    const int x = w->client_x + 10;
    int y = w->client_y + 8;
    const uint32_t fg = 0x1A1A1Au;

    gui_draw_str(x, y, "SutureOS Task Manager", 0x143C8Cu);
    y += 22;

    kv_u32(x, y, "CPUs online : ", smp_online_cpu_count(), fg);
    y += 18;

    {
        const uint64_t pages = moe_pmm_free_count();
        char buf[64];
        char* p = put_str(buf, "PMM free    : ");
        p = put_u32(p, (uint32_t)pages);
        p = put_str(p, " pages / ");
        p = put_u32(p, (uint32_t)(pages / 256ull));   /* 256 pages = 1 MiB */
        p = put_str(p, " MiB");
        *p = 0;
        gui_draw_str(x, y, buf, fg);
    }
    y += 18;

    {
        const uint32_t kib = (uint32_t)(lemis_heap_free_space() / 1024u);
        char buf[64];
        char* p = put_str(buf, "Heap free   : ");
        p = put_u32(p, kib);
        p = put_str(p, " KiB");
        *p = 0;
        gui_draw_str(x, y, buf, fg);
    }
    y += 18;

    kv_u32(x, y, "Uptime      : ", (uint32_t)(g_tick_count / APP_PIT_HZ), fg);
    gui_draw_str(x + 13 * 8, y, " s", fg);
    y += 18;

    kv_u32(x, y, "FPS         : ", gui_fps(), fg);
    y += 18;

    kv_u32(x, y, "Windows     : ", (uint32_t)gui_window_count(), fg);
    y += 22;

    /* busy bar: the frame-time share measured by the main loop */
    gui_draw_str(x, y, "CPU busy", fg);
    y += 18;
    {
        const uint32_t pct = gui_cpu_busy_pct();
        const int bar_w = clamp_min(w->client_w - 20, 8);
        gui_fill_rect(x, y, bar_w, 10, 0xD0D4DAu);
        gui_fill_rect(x, y, (int)((uint32_t)bar_w * pct / 100u), 10, 0x2E8B57u);
        gui_frame_rect(x, y, bar_w, 10, 0x808890u);
        kv_u32(x + bar_w + 4, y - 3, "", pct, fg);
    }
}

/* Repaint at ~10 Hz: enough for live counters without redrawing every frame. */
static void tmgr_tick(struct gui_window* w) {
    static uint64_t last = 0;
    if (g_tick_count - last < (APP_PIT_HZ / 10u)) return;
    last = g_tick_count;
    gui_invalidate_window(w);
}

/* ==================== Calculator ==================== */

struct calc_state {
    int32_t acc;        /* accumulator */
    int32_t cur;        /* value being typed */
    int     entering;   /* 1 while digits build `cur` */
    char    op;         /* pending operator, 0 = none */
    char    line[40];   /* display string */
    char    status[24]; /* "3 + 4" style breadcrumb */
};

static struct calc_state g_calc;

static void calc_update_line(struct calc_state* c) {
    char* p = c->line;
    if (c->op) p = put_str(p, c->status);
    p = put_u32(p, (uint32_t)(c->entering ? c->cur : c->acc));
    *p = 0;
}

static void calc_apply(struct calc_state* c) {
    switch (c->op) {
    case '+': c->acc += c->cur; break;
    case '-': c->acc -= c->cur; break;
    case '*': c->acc *= c->cur; break;
    case '/': c->acc = c->cur ? (c->acc / c->cur) : 0; break;
    default:  c->acc = c->cur; break;
    }
    c->op = 0;
    c->entering = 0;
}

static void calc_key(struct gui_window* w, char ch) {
    struct calc_state* c = &g_calc;
    if (ch >= '0' && ch <= '9') {
        if (!c->entering) { c->entering = 1; c->cur = 0; }
        c->cur = c->cur * 10 + (ch - '0');
    } else if (ch == '+' || ch == '-' || ch == '*' || ch == '/') {
        if (c->entering) { calc_apply(c); c->entering = 0; }
        c->op = ch;
        {
            char* p = put_u32(c->status, (uint32_t)c->acc);
            *p++ = ' ';
            *p++ = ch;
            *p++ = ' ';
            *p = 0;
        }
    } else if (ch == '=' || ch == '\n' || ch == '\r') {
        if (c->entering) { calc_apply(c); }
    } else if (ch == 'c' || ch == 'C') {
        c->acc = 0; c->cur = 0; c->entering = 0; c->op = 0; c->status[0] = 0;
    }
    calc_update_line(c);
    gui_invalidate_window(w);
}

static void calc_draw(struct gui_window* w) {
    const int x = w->client_x + 10;
    int y = w->client_y + 10;
    const int inner_w = clamp_min(w->client_w - 20, 8);   /* resize-safe */

    /* display */
    gui_fill_rect(x, y, inner_w, 34, 0x101418u);
    gui_frame_rect(x, y, inner_w, 34, 0x39424Eu);
    {
        const int len = (int)strlen(g_calc.line);
        int tx = x + inner_w - 8 - len * 8;
        if (tx < x) tx = x;                  /* keep the number inside the box */
        gui_draw_str(tx, y + 9, g_calc.line, 0x8CE89Cu);
    }
    y += 44;

    /* key legend (keyboard driven: there is no mouse driver in this build) */
    gui_draw_str(x, y, "0-9  + - * /  =  c", 0x30363Eu);
    y += 22;
    if (g_calc.op) gui_draw_str(x, y, g_calc.status, 0x505860u);
    else           gui_draw_str(x, y, "keys: digits, operators, = , c", 0x707880u);
}

/* ==================== About ==================== */

static void about_draw(struct gui_window* w) {
    const int x = w->client_x + 10;
    int y = w->client_y + 8;
    /* the version comes from include/sutureos/version.h so a release bump can
     * never leave a stale string baked into the ISO */
    static char about_ver[64];
    if (about_ver[0] == 0) {
        int n = snprintf(about_ver, sizeof(about_ver), "SutureOS v%s",
                         SUTUREOS_VERSION);
        if (n <= 0) about_ver[0] = 0;
    }
    static const char* lines[] = {
        "a stitched-together x86_64 kernel",
        "",
        "14 upstream hobby OS projects:",
        "CoolPotOS Cinux HanOS HeloOS",
        "Lemis MandelbrotOS MoeOS MOOS",
        "NAOS Phobos PlantOS SpiritFoxOS",
        "VimtuOS XJ380",
        "",
        "Terminal: flanterm (BSD-2-Clause)",
        "License: GPL-3.0 (upstreams respected)",
    };
    gui_draw_str(x, y, about_ver, 0x1B3A6Bu);
    y += 16;
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        gui_draw_str(x, y + (int)i * 16, lines[i], i == 0 ? 0x143C8Cu : 0x1A1A1Au);
    }
}

/* ==================== bring-up ==================== */

void gui_apps_init(void) {
    static int created = 0;
    if (created) return;
    created = 1;

    const int sh = gui_screen_h();
    const int avail = sh - gui_taskbar_h();

    /* Task Manager: left, tall enough for the counter block */
    struct gui_window* tm = gui_create_window("Task Manager", 24, 24, 330, 260,
                                             tmgr_draw, NULL, NULL, APP_ID_TMGR);
    if (tm) gui_set_tick(tm, tmgr_tick);

    /* Calculator: middle, the interactive one */
    gui_create_window("Calculator", 372, 40, 250, 190,
                      calc_draw, calc_key, NULL, APP_ID_CALC);

    /* About: right */
    gui_create_window("About SutureOS", 646, 90, 350, 240,
                      about_draw, NULL, NULL, APP_ID_SETTINGS);

    g_calc.acc = 0; g_calc.cur = 0; g_calc.entering = 0; g_calc.op = 0;
    g_calc.status[0] = 0;
    calc_update_line(&g_calc);

    console_printf("[gui] apps created (available height %d px)\n", (int64_t)avail);
    console_puts("[gui] keys: Tab=focus  arrows=move focused window  q/ESC=quit\n");
}
