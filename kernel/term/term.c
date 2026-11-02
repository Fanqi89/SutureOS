/* SutureOS: framebuffer terminal adapter around the vendored flanterm.
 *
 * This file contains only SutureOS glue: it takes the Limine framebuffer
 * geometry, gives flanterm a private arena to allocate from and exposes the
 * small API declared in term.h.  flanterm itself is kept verbatim (apart from
 * the documented `SutureOS fix` lines in fb.c) in kernel/term/flanterm/
 * (BSD-2-Clause, Copyright (C) 2022-2025 mintsuki and contributors; vendored
 * from CoolPotOS src/term/flanterm).
 */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>

#include "term.h"
#include "cjk16.h"
#include "flanterm/flanterm.h"
/* SutureOS addition: the CJK blit needs flanterm's cursor position and cell
 * metrics, so this adapter (and only this adapter) sees the private header. */
#define FLANTERM_IN_FLANTERM 1
#include "flanterm/flanterm_private.h"
#include "flanterm/flanterm_backends/fb.h"

/* Published by kernel/main/limine.c from the Limine framebuffer request.
 * limine_fb_addr is used directly as a writable pointer: the Limine
 * framebuffer window is identity-mapped by the MMU setup, exactly as the
 * existing fbcon driver already assumes. */
extern uint64_t limine_fb_addr, limine_fb_width, limine_fb_height, limine_fb_pitch, limine_fb_bpp;

static struct flanterm_context *term_ctx = NULL;

/* Console palette: BLACK background, WHITE text, expressed as 0x00RRGGBB.
 * flanterm_fb_init() takes these by pointer (and copies the values), but they
 * stay statics so the addresses are valid for the whole context lifetime. */
static uint32_t term_default_fg = 0xFFFFFF;
static uint32_t term_default_bg = 0x000000;

/* Framebuffer pixel format: 32 bpp xRGB8888, i.e. 8 bit per channel with
 * red << 16, green << 8, blue << 0. */
#define TERM_RED_MASK_SIZE    8
#define TERM_RED_MASK_SHIFT   16
#define TERM_GREEN_MASK_SIZE  8
#define TERM_GREEN_MASK_SHIFT 8
#define TERM_BLUE_MASK_SIZE   8
#define TERM_BLUE_MASK_SHIFT  0

/* Glyph count of flanterm's built-in 8x16 VGA font (256 glyphs * 16 bytes).
 * With a NULL font flanterm forces 8x16 and uses 256 glyphs, so this must be
 * 256 or the built-in font copy at init time would read out of bounds. */
#define TERM_NUM_GLYPH 256

/* ============================ terminal arena ==============================
 * SutureOS fix (deviation from the original "allocate from the kernel heap"
 * design): the terminal state must NOT live in the kernel heap on this system.
 *
 * kmain's suture_mem_bringup() runs moe_pmm_init(kstart, mem_top) and then takes
 * the 8 MiB kernel heap from moe_pmm_alloc(2048).  moe_pmm_init() stores its
 * per-page flag array AT kstart ("pmm_flags = (uint8_t *)start") and
 * moe_pmm_alloc() hands out pages starting at kstart as well, so the kernel
 * heap and the PMM's flag array are literally the same memory.  Every later
 * moe_pmm_alloc()/moe_pmm_free() (AP stacks in smp_init(), task stacks, ...)
 * writes flag bytes straight into the live heap.
 *
 * Measured with a host harness that links the real pmm.c + heap.c + this file:
 * one flag-write range over the context turns struct flanterm_context's parser
 * state into garbage.  The ANSI parser then swallows every character, so the
 * terminal stops drawing completely while term_cols()/term_rows() still report
 * 113x48 and only the cells drawn before the corruption stay on screen (a few
 * cursor blocks) - exactly the broken layout seen in the QEMU screendump.  With
 * the state in the heap the same harness also dies with an access violation
 * once the heap header is clobbered.
 *
 * The terminal therefore owns this static arena (the same approach as
 * flanterm's own bump allocator).  It lives in .bss, below _bss_end, where
 * neither the PMM nor the heap can reach it - the console keeps rendering even
 * while the rest of the kernel is still losing memory to that conflict.
 *
 * Size: the state is context (~1 KiB) + font bits (4 KiB) + font bitmap
 * (~36 KiB) + grid + queue + map (~52 bytes per cell).  1 MiB covers the
 * 113x48 cells of a 1024x768 framebuffer (~330 KiB, logged at init) with a
 * wide margin; exhaustion fails the terminal cleanly instead of corrupting.
 * ------------------------------------------------------------------------ */
#define TERM_ARENA_SIZE (1024u * 1024u)

static uint8_t term_arena[TERM_ARENA_SIZE] __attribute__((aligned(16)));
static size_t term_arena_used = 0;
static size_t term_arena_high = 0;

/* flanterm allocation hooks (see the arena note above). */
static void *term_malloc(size_t n)
{
    size_t need = (n + 15u) & ~(size_t)15u;   /* keep flanterm's 16-byte alignment */

    if (need > TERM_ARENA_SIZE - term_arena_used) {
        return NULL;                          /* flanterm_fb_init() fails cleanly */
    }

    void *p = &term_arena[term_arena_used];
    term_arena_used += need;
    if (term_arena_used > term_arena_high) {
        term_arena_high = term_arena_used;
    }
    return p;
}

/* Bump allocator: nothing is ever handed back.  flanterm only calls this from
 * flanterm_deinit() (never used here) and from its own init failure path. */
static void term_free(void *p, size_t n)
{
    (void)p;
    (void)n;
}

/* One-shot render check: proves flanterm's pixels actually reach the
 * framebuffer (a foreign/absent mapping, a wrong pitch or a corrupted context
 * shows up here instead of as a mysteriously blank console).  flanterm's
 * built-in 'H' paints 25 white pixels inside the first 9x16 cell. */
static void term_selftest(void)
{
    volatile uint32_t *pixels = (volatile uint32_t *)(uintptr_t)limine_fb_addr;
    size_t stride = (size_t)(limine_fb_pitch / 4u);
    uint32_t drawn = 0;

    term_clear();
    term_putc('H');

    for (size_t y = 0; y < 16 && y < limine_fb_height; y++) {
        for (size_t x = 0; x < 12 && x < limine_fb_width; x++) {  /* glyph starts at x=3 */
            if (pixels[y * stride + x] == 0x00FFFFFFu) {
                drawn++;
            }
        }
    }

    console_printf("[term] selftest: %u px visible at cell(0,0) (expect 25)\n",
                   (uint64_t)drawn);
    term_clear();
}

void term_init_from_limine(void)
{
    /* Already initialised: do not allocate a second context (two contexts would
     * fight over one screen). */
    if (term_ctx != NULL) {
        return;
    }

    if (limine_fb_addr == 0 || limine_fb_width == 0 || limine_fb_height == 0 ||
        limine_fb_bpp != 32) {
        console_printf("[term] no usable framebuffer (addr=0x%x, bpp=%u)\n",
                       (uint64_t)limine_fb_addr, (uint64_t)limine_fb_bpp);
        return;
    }

    size_t arena_mark = term_arena_used;

    term_ctx = flanterm_fb_init(
        /* allocator hooks: private static arena, never the kernel heap */
        term_malloc,
        term_free,
        /* framebuffer geometry from Limine */
        (uint32_t *)(uintptr_t)limine_fb_addr,
        (size_t)limine_fb_width,
        (size_t)limine_fb_height,
        (size_t)limine_fb_pitch,
        /* xRGB8888 masks */
        TERM_RED_MASK_SIZE, TERM_RED_MASK_SHIFT,
        TERM_GREEN_MASK_SIZE, TERM_GREEN_MASK_SHIFT,
        TERM_BLUE_MASK_SIZE, TERM_BLUE_MASK_SHIFT,
        /* canvas / ansi palettes: flanterm defaults */
        NULL,
        NULL,
        NULL,
        /* default colours: black background, white text */
        &term_default_bg,
        &term_default_fg,
        NULL,
        NULL,
        /* NULL font -> built-in 8x16 font; width/height/spacing/scale/margin
         * are ignored or overridden by flanterm in that case */
        NULL,
        0, 0, 0,
        0, 0,
        0,
        TERM_NUM_GLYPH
    );

    /* CJK bitmap font (16x16, GB2312 subset). Without it the terminal still
     * works: wide characters fall through to flanterm's replacement box. */
    if (cjk16_init() == 0) {
        console_printf("[term] CJK font: %d glyphs, 16x16\n",
                       (int64_t)cjk16_count());
    } else {
        console_puts("[term] CJK font missing - wide characters render as boxes\n");
    }

    if (term_ctx == NULL) {
        term_arena_used = arena_mark;   /* give the arena back: init stays retry-able */
        console_printf("[term] flanterm init FAILED\n");
        return;
    }

    flanterm_set_autoflush(term_ctx, true);
    term_clear();
    term_selftest();

    console_printf("[term] flanterm arena %u KiB of %u KiB used\n",
                   (uint64_t)(term_arena_high / 1024u),
                   (uint64_t)(TERM_ARENA_SIZE / 1024u));
    console_printf("[term] flanterm ready %ux%u on %ux%u@%ubpp\n",
                   (uint64_t)term_cols(), (uint64_t)term_rows(),
                   limine_fb_width, limine_fb_height, limine_fb_bpp);
}

int term_available(void)
{
    return term_ctx != NULL;
}

/* NOTE (deliberate deviation from the "term_putc == flanterm_putchar" mapping):
 * flanterm_putchar() only queues the character into the backend's double
 * buffer; flanterm honours `autoflush` exclusively at the end of
 * flanterm_write().  A bare flanterm_putchar() would therefore never reach the
 * framebuffer until some other call flushed, which is fatal for the console
 * path (console_putc -> term_putc per character: the screen would stay blank or
 * always lag by one character).  term_putc() keeps the documented behaviour
 * "one character in, one character on screen" by flushing right after it; the
 * flush only redraws the queued cell plus the cursor, so it stays cheap. */
/* ==================== CJK (16x16) support ====================
 * flanterm's built-in font is 8x16 cp437: a CJK codepoint decodes as UTF-8 and
 * then falls back to the "no glyph" box. SutureOS therefore intercepts the text
 * stream one level above flanterm:
 *   - ASCII bytes go to flanterm unchanged,
 *   - a wide codepoint is written as TWO spaces (so wrapping, scrolling and the
 *     cell grid stay flanterm's business), then the 16x16 bitmap is blitted over
 *     the two cells in the framebuffer.
 * Known limit: the bitmap is painted on top of flanterm's cells, so a later full
 * repaint of those cells (scroll) clears it. That is documented in README. */
static uint8_t  cjk_seq[4];
static uint32_t cjk_seq_len;
static uint8_t  cjk_seq_need;

static uint32_t cjk_decode(uint32_t n) {
    uint32_t cp;
    switch (n) {
    case 2:  cp = (uint32_t)(cjk_seq[0] & 0x1Fu) << 6;  break;
    case 3:  cp = (uint32_t)(cjk_seq[0] & 0x0Fu) << 12; break;
    default: cp = (uint32_t)(cjk_seq[0] & 0x07u) << 18; break;
    }
    for (uint32_t i = 1; i < n; i++)
        cp |= (uint32_t)(cjk_seq[i] & 0x3Fu) << (6u * (n - 1u - i));
    return cp;
}

/* CJK overlay list: flanterm's grid only knows 8x16 cells, so every glyph we
 * paint on top of two cells is remembered here. When the terminal scrolls (a
 * newline on the last row) the list is shifted up and repainted, which is what
 * keeps Chinese text readable after `ls` produces more lines than the screen
 * holds. Cleared by term_clear()/term_clear_full(). */
#define CJK_OVERLAY_MAX 512
struct cjk_overlay_entry {
    uint16_t x;
    uint16_t y;
    uint32_t cp;
};
static struct cjk_overlay_entry g_cjk_overlay[CJK_OVERLAY_MAX];
static uint32_t g_cjk_overlay_count;

static void cjk_overlay_repaint(void) {
    if (!term_ctx || g_cjk_overlay_count == 0) return;
    uint32_t *fb = NULL;
    size_t pitch = 0, w = 0, h = 0, cw = 8, ch = 16, ox = 0, oy = 0;
    flanterm_fb_metrics(term_ctx, &fb, &pitch, &w, &h, &cw, &ch, &ox, &oy);
    if (!fb || !cw || !ch) return;
    for (uint32_t i = 0; i < g_cjk_overlay_count; i++) {
        const struct cjk_overlay_entry *e = &g_cjk_overlay[i];
        cjk16_draw(fb, (uint32_t)pitch, (uint32_t)w, (uint32_t)h,
                   (int)ox + (int)e->x * (int)cw,
                   (int)oy + (int)e->y * (int)ch, e->cp,
                   term_default_fg, term_default_bg);
    }
}

static void cjk_overlay_scroll(uint32_t rows) {
    uint32_t keep = 0;
    for (uint32_t i = 0; i < g_cjk_overlay_count; i++) {
        if (g_cjk_overlay[i].y >= rows) {
            g_cjk_overlay[keep] = g_cjk_overlay[i];
            g_cjk_overlay[keep].y = (uint16_t)(g_cjk_overlay[i].y - rows);
            keep++;
        }
    }
    g_cjk_overlay_count = keep;
    flanterm_flush(term_ctx);      /* grid first (see term_blit_cjk) */
    cjk_overlay_repaint();
}

static void cjk_overlay_clear(void) {
    g_cjk_overlay_count = 0;
}

static void cjk_overlay_add(uint32_t x, uint32_t y, uint32_t cp) {
    if (g_cjk_overlay_count >= CJK_OVERLAY_MAX) {
        /* drop the oldest quarter, keep the newest text */
        const uint32_t drop = CJK_OVERLAY_MAX / 4;
        for (uint32_t i = drop; i < g_cjk_overlay_count; i++)
            g_cjk_overlay[i - drop] = g_cjk_overlay[i];
        g_cjk_overlay_count -= drop;
    }
    g_cjk_overlay[g_cjk_overlay_count].x = (uint16_t)x;
    g_cjk_overlay[g_cjk_overlay_count].y = (uint16_t)y;
    g_cjk_overlay[g_cjk_overlay_count].cp = cp;
    g_cjk_overlay_count++;
}

/* Blit one wide glyph over the two cells that were just blanked. */
static void term_blit_cjk(uint32_t cp, uint32_t before_cols, uint32_t before_rows) {
    (void)before_cols; (void)before_rows;
    if (!cjk16_is_wide(cp) || !cjk16_lookup(cp)) {
        /* not in the subset: let flanterm draw its replacement box */
        flanterm_write(term_ctx, "  ", 2);
        return;
    }
    flanterm_write(term_ctx, "  ", 2);

    size_t cx = 0, cy = 0;
    if (term_ctx->get_cursor_pos) term_ctx->get_cursor_pos(term_ctx, &cx, &cy);
    int gx, gy;
    if (cx >= 2) { gx = (int)cx - 2; gy = (int)cy; }
    else         { gx = (int)term_cols() - 2; gy = (int)cy - 1; }
    if (gx < 0 || gy < 0) return;

    uint32_t *fb = NULL;
    size_t pitch = 0, w = 0, h = 0, cw = 8, ch = 16, ox = 0, oy = 0;
    flanterm_fb_metrics(term_ctx, &fb, &pitch, &w, &h, &cw, &ch, &ox, &oy);
    if (!fb || !cw || !ch) return;

    /* Flush flanterm's cell grid FIRST: the backend is double buffered, so a
     * flush after the blit would repaint the blanked cells over the glyph. */
    flanterm_flush(term_ctx);
    cjk16_draw(fb, (uint32_t)pitch, (uint32_t)w, (uint32_t)h,
               (int)ox + gx * (int)cw, (int)oy + gy * (int)ch, cp,
               term_default_fg, term_default_bg);
    cjk_overlay_add((uint32_t)gx, (uint32_t)gy, cp);
    /* Cursor: flanterm repaints it on its next flush, which now happens before
     * the next blit, so the cursor stays visible without erasing the glyph. */
}

/* Feed one byte of the text stream. */
static void term_feed(uint8_t b) {
    if (cjk_seq_need == 0) {
        if (b < 0x80u) {
            const int is_nl = (b == '\n');
            size_t cx = 0, cy = 0;
            if (is_nl && term_ctx->get_cursor_pos)
                term_ctx->get_cursor_pos(term_ctx, &cx, &cy);
            flanterm_putchar(term_ctx, b);
            if (is_nl && term_ctx->rows && cy + 1 >= (size_t)term_ctx->rows)
                cjk_overlay_scroll(1);
            return;
        }
        if ((b & 0xE0u) == 0xC0u)      { cjk_seq_need = 2; }
        else if ((b & 0xF0u) == 0xE0u) { cjk_seq_need = 3; }
        else if ((b & 0xF8u) == 0xF0u) { cjk_seq_need = 4; }
        else { flanterm_putchar(term_ctx, '?'); return; }
        cjk_seq[0] = b;
        cjk_seq_len = 1;
        return;
    }
    if ((b & 0xC0u) != 0x80u) {          /* broken sequence: drop it */
        cjk_seq_need = 0;
        cjk_seq_len = 0;
        flanterm_putchar(term_ctx, '?');
        return;
    }
    cjk_seq[cjk_seq_len++] = b;
    if (cjk_seq_len < cjk_seq_need) return;
    const uint32_t cp = cjk_decode(cjk_seq_need);
    cjk_seq_need = 0;
    cjk_seq_len = 0;
    term_blit_cjk(cp, 0, 0);
}

void term_putc(char c)
{
    if (term_ctx == NULL) {
        return;
    }
    term_feed((uint8_t)c);
    flanterm_flush(term_ctx);
}

void term_write(const char *s, uint32_t n)
{
    if (term_ctx == NULL || s == NULL) {
        return;
    }
    if (cjk16_count() == 0) {            /* no font: flanterm's own path */
        flanterm_write(term_ctx, s, (size_t)n);
        return;
    }
    for (uint32_t i = 0; i < n; i++) term_feed((uint8_t)s[i]);
    flanterm_flush(term_ctx);
}

void term_puts(const char *s)
{
    if (term_ctx == NULL || s == NULL) {
        return;
    }
    term_write(s, (uint32_t)strlen(s));
}

/* Clear the screen and home the cursor using the standard ANSI sequences
 * flanterm understands (ED 2 = erase whole display, CUP 1;1 = home). */
/* Wipe the framebuffer pixels with the terminal background colour.
 *
 * SutureOS fix (boot presentation): before flanterm takes over, the early boot
 * log is drawn by the fbcon fallback straight into the framebuffer.  flanterm's
 * own grid starts out empty, so a "clear" that only touches flanterm's cells
 * cannot erase those pixels - the tail of the boot log stayed visible on the
 * top row of the screen even after the banner was printed.  Painting the whole
 * framebuffer with the background colour first makes the screen really empty. */
static void term_wipe_framebuffer(void)
{
    volatile uint32_t *pixels = (volatile uint32_t *)(uintptr_t)limine_fb_addr;
    size_t stride = (size_t)(limine_fb_pitch / 4u);
    uint32_t bg = term_default_bg;

    if (limine_fb_addr == 0 || limine_fb_pitch == 0) {
        return;
    }
    for (uint32_t y = 0; y < limine_fb_height; y++) {
        volatile uint32_t *line = pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < limine_fb_width; x++) {
            line[x] = bg;
        }
    }
}

void term_clear(void)
{
    cjk_overlay_clear();
    if (term_ctx == NULL) {
        return;
    }
    /* Flush the double buffer FIRST.
     *
     * flanterm queues every cell and paints it on the next flush; a line that
     * was still queued when the screen was cleared (the boot log is written
     * while the console is busy, and a concurrent writer can leave a partial
     * line behind) would be painted at its OLD position afterwards - the boot
     * log then reappears as scattered fragments under the banner (measured on
     * VMware: "ahci port 0: V" and other leftovers). Flushing first empties the
     * queue, so nothing stale can be painted after the clear. */
    flanterm_flush(term_ctx);

    /* Deterministic clear: walk every row of the grid and erase it with
     * ESC[2K (flanterm implements it: "erase the whole line"), then park the
     * cursor in the top-left corner.
     *
     * Why not ESC[2J: flanterm does NOT implement the "erase whole screen"
     * variant, so it was silently ignored - the boot log stayed on the screen.
     * And ESC[1J/ESC[0J only work relative to the cursor, which left the rows
     * above it untouched on some framebuffer geometries (VMware). Row by row is
     * geometry independent. */
    {
        char seq[16];
        uint32_t rows = term_rows();

        for (uint32_t r = 0; r < rows; r++) {
            /* ESC [ <row> ; 1 H  ESC [ 2 K   (no snprintf in this TU) */
            size_t n = 0;
            seq[n++] = 0x1B;
            seq[n++] = '[';
            if (r + 1u >= 10u) {
                seq[n++] = (char)('0' + ((r + 1u) / 10u) % 10u);
            }
            seq[n++] = (char)('0' + (r + 1u) % 10u);
            seq[n++] = ';';
            seq[n++] = '1';
            seq[n++] = 'H';
            seq[n++] = 0x1B;
            seq[n++] = '[';
            seq[n++] = '2';
            seq[n++] = 'K';
            flanterm_write(term_ctx, seq, n);
        }
        flanterm_write(term_ctx, "\x1b[H", 3);
    }
    flanterm_flush(term_ctx);
    term_wipe_framebuffer();
    flanterm_full_refresh(term_ctx);
    flanterm_flush(term_ctx);
}

/* Clear the screen AND repaint every cell.
 *
 * A plain flush repaints only the cells flanterm considers dirty, so text that
 * was on the screen before the clear could survive on the framebuffer
 * (measured: the tail of the boot log stayed visible on the top row after the
 * banner was drawn). flanterm_full_refresh() repaints the whole grid from the
 * (now empty) cell buffer, which is what a real "clear the screen" needs. */
void term_clear_full(void)
{
    cjk_overlay_clear();
    term_clear();
}

/* Flush queued cells to the framebuffer.  arch/x86_64/serial.c calls this once
 * per newline, so it must stay cheap: it only paints the cells that changed
 * since the last flush (a plain term_putc() already flushes per character, so
 * this is then a no-op).  Use flanterm_full_refresh() directly if the whole
 * screen ever has to be repainted from the grid (e.g. after another renderer
 * painted over the framebuffer). */
void term_refresh(void)
{
    if (term_ctx == NULL) {
        return;
    }
    flanterm_flush(term_ctx);
}

uint32_t term_cols(void)
{
    size_t cols = 0, rows = 0;

    if (term_ctx == NULL) {
        return 0;
    }
    flanterm_get_dimensions(term_ctx, &cols, &rows);
    return (uint32_t)cols;
}

uint32_t term_rows(void)
{
    size_t cols = 0, rows = 0;

    if (term_ctx == NULL) {
        return 0;
    }
    flanterm_get_dimensions(term_ctx, &cols, &rows);
    return (uint32_t)rows;
}

/* The default colours are configuration values, not mutable terminal state
 * (flanterm only exposes them via its backend-private struct), so they are
 * reported straight from the values handed to flanterm_fb_init().  For the
 * xRGB8888 masks used here flanterm's colour conversion is the identity, so
 * these are exactly the colours the backend stored. */
uint32_t term_fg(void)
{
    return term_default_fg;
}

uint32_t term_bg(void)
{
    return term_default_bg;
}
