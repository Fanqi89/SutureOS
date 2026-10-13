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
#include "flanterm/flanterm.h"
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
void term_putc(char c)
{
    if (term_ctx == NULL) {
        return;
    }
    flanterm_putchar(term_ctx, (uint8_t)c);
    flanterm_flush(term_ctx);
}

void term_write(const char *s, uint32_t n)
{
    if (term_ctx == NULL || s == NULL) {
        return;
    }
    flanterm_write(term_ctx, s, (size_t)n);
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
void term_clear(void)
{
    if (term_ctx == NULL) {
        return;
    }
    flanterm_write(term_ctx, "\x1b[2J\x1b[H", 7);
    flanterm_flush(term_ctx);
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
