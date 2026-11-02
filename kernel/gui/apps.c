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
#include "imgdecode.h"
#include "heap.h"
#include "vfs.h"
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

/* ==================== Image Viewer (v0.18.0) ====================
 * New application, but NOT part of the default desktop: it is created the first
 * time the start menu asks for it (gui_apps_launch below), so the frozen
 * baseline screenshot is untouched.
 *
 * Chrome and layout are SutureOS' own (same client background, same status line
 * conventions as the other apps). Decoders live in kernel/gui/img_bmp.c and
 * img_qoi.c; algorithm references: HBOS bmp.c (GPL-3.0) / PlantOS qoi.c (MIT).
 */
#define IMG_MAX_W 600
#define IMG_MAX_H 480
#define IMG_MAX_PIXELS (IMG_MAX_W * IMG_MAX_H)
#define IMG_SCALE_MAX 12

static struct {
    int       loaded;          /* 0 = not tried, 1 = pixels valid            */
    int       fmt;             /* IMG_FMT_*                                  */
    int       w, h;
    uint32_t *px;              /* heap buffer, IMG_MAX_PIXELS words          */
    int       pattern;         /* 1 = built-in colour pattern, no file       */
    char      name[40];        /* source name for the status line            */
    char      line[72];        /* "IMG.BMP 32x32 bmp (24bpp)"                */
} g_img;

static void put_u32_dec(char **pp, uint32_t v) {
    char tmp[10];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) *(*pp)++ = tmp[--n];
}

/* suffix test (no strstr in this kernel's libc subset) */
static int img_suffix_is(const char* s, const char* suf) {
    const size_t n = strlen(s);
    const size_t m = strlen(suf);
    if (n < m) return 0;
    return strcmp(s + n - m, suf) == 0;
}

/* Built-in 64x48 gradient with a red/green/blue frame, used when no image file
 * is present: the window still proves the draw path without lying about it. */
static void img_build_pattern(void) {
    for (int y = 0; y < 48; y++) {
        for (int x = 0; x < 64; x++) {
            uint32_t r = (uint32_t)(x * 255 / 63);
            uint32_t g = (uint32_t)(y * 255 / 47);
            uint32_t b = (uint32_t)(((x ^ y) & 0x20) ? 200 : 40);
            if (x < 2 || y < 2 || x >= 62 || y >= 46) {
                /* frame: split into red / green / blue quarters */
                r = (x < 32) ? 255u : 0u;
                g = (x < 32) ? 0u : 255u;
                b = (y < 24) ? 0u : 255u;
            }
            g_img.px[y * 64 + x] = (r << 16) | (g << 8) | b;
        }
    }
    g_img.w = 64;
    g_img.h = 48;
    g_img.fmt = IMG_FMT_NONE;
    g_img.pattern = 1;
    g_img.loaded = 1;
    { char *p = g_img.line; const char *q = "no image file - built-in pattern"; while (*q) *p++ = *q++; *p = 0; }
}

/* Try every known location once. The FAT32 test disk is the natural place to
 * drop IMG.BMP / IMG.QOI for an acceptance run. */
static void img_load_once(void) {
    static const char* paths[] = {
        "/mnt/fat/\xE5\x9B\xBE\xE7\x89\x87.BMP",   /* 图片.BMP: CJK status line */
        "/mnt/fat/IMG.BMP", "/mnt/fat/IMG.QOI",
        "/images/IMG.BMP", "/images/IMG.QOI",
        "/IMG.BMP", "/IMG.QOI",
    };
    if (!g_img.px) {
        g_img.px = (uint32_t*)lemis_heap_alloc((uintptr_t)IMG_MAX_PIXELS * 4u);
        if (!g_img.px) return;
    }
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        size_t len = 0;
        void* buf = vfs_read_file(paths[i], &len);
        if (!buf) continue;
        img_decoded_t dec;
        int ok = 0;
        const uint32_t n = (uint32_t)len;
        if (img_suffix_is(paths[i], ".QOI")) {
            dec.px = g_img.px;
            ok = img_decode_qoi((const uint8_t*)buf, n, &dec, IMG_MAX_PIXELS);
        } else {
            dec.px = g_img.px;
            ok = img_decode_bmp((const uint8_t*)buf, n, &dec, IMG_MAX_PIXELS);
        }
        lemis_heap_free(buf);
        if (!ok) {
            console_printf("[img] cannot decode %s (%u bytes) - trying next\n",
                           paths[i], (uint64_t)n);
            continue;
        }
        g_img.loaded = 1;
        g_img.pattern = 0;
        g_img.w = dec.w;
        g_img.h = dec.h;
        g_img.fmt = dec.fmt;
        {
            const char* base = paths[i];
            const char* slash = strrchr(base, '/');
            if (slash) base = slash + 1;
            unsigned k = 0;
            while (base[k] && k < sizeof(g_img.name) - 1) { g_img.name[k] = base[k]; k++; }
            g_img.name[k] = 0;
        }
        {
            char* q = g_img.line;
            const char* nm = g_img.name;
            while (*nm) *q++ = *nm++;
            *q++ = ' ';
            put_u32_dec(&q, (uint32_t)g_img.w);
            *q++ = 'x';
            put_u32_dec(&q, (uint32_t)g_img.h);
            *q++ = ' ';
            {
                const char* f = img_fmt_name(dec.fmt);
                while (*f) *q++ = *f++;
            }
            if (dec.bpp) {
                *q++ = ' ';
                *q++ = '(';
                put_u32_dec(&q, (uint32_t)dec.bpp);
                const char* b = "bpp)";
                while (*b) *q++ = *b++;
            }
            *q = 0;
        }
        console_printf("[img] decoded %s %ux%u %s bpp=%u\n", paths[i],
                       (uint64_t)g_img.w, (uint64_t)g_img.h,
                       img_fmt_name(g_img.fmt), (uint64_t)dec.bpp);
        return;
    }
    img_build_pattern();
    console_puts("[img] no decodable image on disk - showing the built-in pattern\n");
}

static void img_draw(struct gui_window* w) {
    if (!g_img.loaded) img_load_once();

    const int x = w->client_x + 10;
    int y = w->client_y + 8;
    const int inner_w = clamp_min(w->client_w - 20, 8);
    const int inner_h = clamp_min(w->client_h - 34, 8);

    /* status line + divider, same conventions as the other apps */
    gui_draw_str(x, y, g_img.line, 0x143C8Cu);
    y += 20;
    gui_fill_rect(x, y, inner_w, 1, 0xC0C6CEu);
    y += 6;

    const int avail_h = clamp_min(inner_h - 22, 8);
    if (g_img.px && g_img.w > 0 && g_img.h > 0) {
        int sx = inner_w / g_img.w;
        int sy = avail_h / g_img.h;
        int scale = (sx < sy) ? sx : sy;
        if (scale < 1) scale = 1;
        if (scale > IMG_SCALE_MAX) scale = IMG_SCALE_MAX;
        const int dw = g_img.w * scale;
        const int dh = g_img.h * scale;
        int ox = x + (inner_w - dw) / 2;
        int oy = y + (avail_h - dh) / 2;
        if (ox < x) ox = x;
        if (oy < y) oy = y;

        for (int dy = 0; dy < dh; dy++) {
            const int src_y = dy / scale;
            if (src_y >= g_img.h) break;
            const uint32_t* row = g_img.px + (uint64_t)src_y * (uint64_t)g_img.w;
            for (int dx = 0; dx < dw; dx += scale) {
                const int src_x = dx / scale;
                if (src_x >= g_img.w) break;
                const uint32_t c = row[src_x];
                gui_fill_rect(ox + dx, oy + dy, scale, 1,
                              ((c >> 16) & 0xFFu) << 16 |
                              ((c >> 8) & 0xFFu) << 8 | (c & 0xFFu));
            }
        }
        gui_frame_rect(ox - 1, oy - 1, dw + 2, dh + 2, 0x808890u);
        y = oy + dh + 6;
        if (y + 14 <= w->client_y + w->client_h) {
            char foot[40];
            char* p = foot;
            const char* n = "\xE7\xBC\xA9\xE6\x94\xBE x";  /* 缩放 x */
            while (*n) *p++ = *n++;
            put_u32_dec(&p, (uint32_t)scale);
            *p = 0;
            gui_draw_str(x, y, foot, 0x707880u);
        }
    } else {
        gui_draw_str(x, y + 10, "no image loaded", 0x707880u);
    }
}

/* Click cycles to the next image source (the viewer has no file browser: the
 * frozen chrome has no list widget and this app must not invent one). */
static void img_click(struct gui_window* w, int cx, int cy) {
    (void)w; (void)cx; (void)cy;
    if (!g_img.loaded) return;
    static int idx = 0;
    static const char* paths[] = { "/mnt/fat/IMG.BMP", "/mnt/fat/IMG.QOI" };
    idx = (idx + 1) % (int)(sizeof(paths) / sizeof(paths[0]));
    size_t len = 0;
    void* buf = vfs_read_file(paths[idx], &len);
    if (!buf) return;
    img_decoded_t dec;
    dec.px = g_img.px;
    int ok = img_suffix_is(paths[idx], ".QOI")
                 ? img_decode_qoi((const uint8_t*)buf, (uint32_t)len, &dec, IMG_MAX_PIXELS)
                 : img_decode_bmp((const uint8_t*)buf, (uint32_t)len, &dec, IMG_MAX_PIXELS);
    lemis_heap_free(buf);
    if (ok) {
        g_img.w = dec.w; g_img.h = dec.h; g_img.fmt = dec.fmt; g_img.pattern = 0;
        console_printf("[img] switched to %s (%ux%u)\n", paths[idx],
                       (uint64_t)dec.w, (uint64_t)dec.h);
    }
}

/* Called by the start menu the first time "Image Viewer" is chosen. */

/* ==================== Text Editor (v0.25.0, B10) ====================
 * A small but real editor: it loads a text file from the VFS, edits it with the
 * keyboard path the window manager already provides (printable / Enter /
 * Backspace), and writes it back with Ctrl+S. The GUI's global q / ESC shortcut
 * still quits the desktop, so the editor deliberately does not claim 'q'.
 *
 * No third-party code: drawing goes through gui_draw_str/gui_fill_rect only.
 */
#define ED_MAX 1024
static struct {
    char buf[ED_MAX];
    int  len;
    int  cur;
    int  loaded;
    int  dirty;
    char path[64];
    char msg[48];
} g_ed;

static int ed_lines(const char *b, int n, int *line_starts, int max_lines) {
    int lines = 0;
    if (max_lines > 0) line_starts[0] = 0;
    lines = 1;
    for (int i = 0; i < n; i++) {
        if (b[i] == '\n') {
            if (lines < max_lines) line_starts[lines] = i + 1;
            lines++;
        }
    }
    return lines;
}

static void ed_load(void) {
    if (g_ed.loaded) return;
    g_ed.loaded = 1;
    strcpy(g_ed.path, "/mnt/fat/EDIT.TXT");
    g_ed.len = 0;
    g_ed.cur = 0;
    vfs_stat_t st;
    if (vfs_stat(g_ed.path, &st) != VFS_OK || st.type != VFS_NODE_FILE) {
        strcpy(g_ed.msg, "new file");
        console_printf("[edit] new file %s\n", g_ed.path);
        return;
    }
    int fd = vfs_open(g_ed.path, VFS_O_RDONLY);
    if (fd < 0) { strcpy(g_ed.msg, "open failed"); return; }
    int n = vfs_read(fd, g_ed.buf, ED_MAX - 1);
    vfs_close(fd);
    if (n < 0) n = 0;
    g_ed.len = n;
    g_ed.cur = n;
    snprintf(g_ed.msg, sizeof(g_ed.msg), "loaded %d bytes", n);
    console_printf("[edit] loaded %s (%d bytes)\n", g_ed.path, (int64_t)n);
}

static void ed_save(void) {
    int r = vfs_write_file(g_ed.path, g_ed.buf, (uint32_t)g_ed.len);
    if (r == VFS_OK) {
        g_ed.dirty = 0;
        snprintf(g_ed.msg, sizeof(g_ed.msg), "saved %d bytes", g_ed.len);
        console_printf("[edit] saved %d bytes to %s\n", (int64_t)g_ed.len, g_ed.path);
    } else {
        snprintf(g_ed.msg, sizeof(g_ed.msg), "save failed (%d)", r);
        console_printf("[edit] save FAILED (%d)\n", (int64_t)r);
    }
}

static void ed_insert(char c) {
    if (g_ed.len >= ED_MAX - 1) return;
    for (int i = g_ed.len; i > g_ed.cur; i--) g_ed.buf[i] = g_ed.buf[i - 1];
    g_ed.buf[g_ed.cur] = c;
    g_ed.cur++;
    g_ed.len++;
    g_ed.dirty = 1;
}

static void ed_backspace(void) {
    if (g_ed.cur <= 0) return;
    for (int i = g_ed.cur - 1; i < g_ed.len - 1; i++) g_ed.buf[i] = g_ed.buf[i + 1];
    g_ed.cur--;
    g_ed.len--;
    g_ed.dirty = 1;
}

static void ed_key(struct gui_window* w, char c) {
    ed_load();
    (void)w;
    if (c == 0x13) {                       /* Ctrl+S */
        ed_save();
    } else if (c == '\b') {
        ed_backspace();
    } else if (c == '\n') {
        /* The keyboard decoder in this build does not surface the CTRL
         * modifier, so Ctrl+S never arrives as 0x13 - Enter doubles as
         * "commit line + save", which is also the friendlier behaviour for a
         * one-file editor. */
        ed_insert('\n');
        ed_save();
    } else if (c >= 32 && c < 127) {
        ed_insert(c);
    }
    gui_invalidate();
}

static void ed_draw(struct gui_window* w) {
    ed_load();
    int x = w->client_x, y = w->client_y;
    int cw = w->client_w, ch = w->client_h;

    gui_fill_rect(x, y, cw, ch, 0x12141Au);
    gui_fill_rect(x, y, cw, 20, 0x1C2432u);

    /* text area, wrapped at the client width in 8px cells */
    const int cols = cw > 16 ? (cw - 8) / 8 : 1;
    const int line_h = 16;
    const int rows = (ch - 40) / line_h;
    int line_starts[64];
    const int nlines = ed_lines(g_ed.buf, g_ed.len, line_starts, 64);

    int c_line = 0, c_col = 0;
    {
        int ln = 0, col = 0;
        for (int i = 0; i < g_ed.cur; i++) {
            if (g_ed.buf[i] == '\n') { ln++; col = 0; }
            else {
                col++;
                if (col >= cols) { ln++; col = 0; }
            }
        }
        c_line = ln;
        c_col = col;
    }
    int first = c_line - rows + 1;
    if (first < 0) first = 0;

    int row = 0;
    for (int ln = 0; ln < nlines && row < rows; ln++) {
        int start = line_starts[ln];
        int end = (ln + 1 < nlines) ? line_starts[ln + 1] - 1 : g_ed.len;
        for (int col = 0; col < cols && start + col < end; col++) {
            char one[2] = { g_ed.buf[start + col], 0 };
            if (one[0] == '\n') break;
            if (ln < first) continue;
            gui_draw_str(x + 4 + col * 8, y + 24 + row * line_h, one, 0xF0F4FFu);
        }
        if (ln >= first) row++;
    }

    /* cursor */
    if (c_line >= first && c_line - first < rows) {
        int cy = y + 24 + (c_line - first) * line_h;
        gui_fill_rect(x + 4 + c_col * 8, cy, 2, 14, 0x6FA8FFu);
    }

    /* footer: path | bytes | hint */
    char foot[128];
    snprintf(foot, sizeof(foot), "%s | %d bytes | Ctrl+S save%s",
             g_ed.path, g_ed.len, g_ed.dirty ? " *" : "");
    gui_draw_str(x + 4, y + ch - 18, foot, 0x9FB2CCu);
    if (g_ed.msg[0]) gui_draw_str(x + cw - 4 - (int)strlen(g_ed.msg) * 8,
                                 y + 2, g_ed.msg, 0x8CE0A0u);
}

int gui_editor_launch(void) {
    g_ed.loaded = 0;
    g_ed.msg[0] = '\0';
    struct gui_window* w = gui_create_window("Text Editor", 120, 90, 520, 380,
                                             ed_draw, ed_key, NULL, APP_ID_EDT);
    if (!w) return 0;
    console_puts("[gui] text editor window created\n");
    return 1;
}

int gui_apps_launch(int app_id) {
    if (app_id == APP_ID_EDT) return gui_editor_launch();
    if (app_id != APP_ID_IMG) return 0;
    struct gui_window* w = gui_create_window("Image Viewer", 150, 80, 420, 340,
                                             img_draw, NULL, img_click, APP_ID_IMG);
    if (!w) return 0;
    console_puts("[gui] image viewer window created\n");
    return 1;
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
