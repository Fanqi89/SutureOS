/* SutureOS: Slint GUI prototype bridge (Stage 0).
 *
 * See slint_bridge.h for the scope and the linking strategy. The Rust side
 * lives in rust/suture-gui/src/lib.rs; the only ABI here is:
 *
 *   uint32_t suture_gui_version(void);                                  -> 0 if not linked
 *   uint32_t suture_gui_selftest_render(uint32_t*, int, int, int);      -> frame checksum
 *   uint32_t suture_gui_frames(void);                                   -> frames drawn
 *
 * 32bpp is required because the Rust `TargetPixel` is a 0x00RRGGBB u32 word,
 * which is exactly what the kernel GUI writes (kernel/gui/gui.c). A 24bpp
 * framebuffer is reported honestly instead of being scribbled on.
 */
#include "slint_bridge.h"
#include <stitch/console.h>

/* Provided by kernel/main/limine.c (multiboot1 framebuffer fields). */
extern uint64_t limine_fb_addr;
extern uint64_t limine_fb_width;
extern uint64_t limine_fb_height;
extern uint64_t limine_fb_pitch;
extern uint64_t limine_fb_bpp;

extern uint32_t suture_gui_version(void);
extern uint32_t suture_gui_selftest_render(uint32_t *fb, int32_t w, int32_t h,
                                           int32_t pitch_px);
extern uint32_t suture_gui_frames(void);
extern uint32_t suture_gui_shell_step(uint32_t *fb, int32_t w, int32_t h,
                                      int32_t pitch_px, int32_t mx, int32_t my,
                                      int32_t buttons, int32_t click_x,
                                      int32_t click_y, int32_t click_btn,
                                      uint32_t now_ms, int32_t key, int32_t ch,
                                      int32_t clock_hhmm);
extern uint32_t suture_gui_interactive_step(uint32_t *fb, int32_t w, int32_t h,
                                            int32_t pitch_px, int32_t mx,
                                            int32_t my, int32_t buttons,
                                            int32_t click_x, int32_t click_y,
                                            int32_t click_btn, uint32_t now_ms);

/* v0.60.0: swap-chain counters (present/coalesce accounting in shell.rs). */
extern uint32_t suture_gui_swap_report(void);

/* Weak fallbacks: overridden by the Rust staticlib when it is linked in. */
__attribute__((weak)) uint32_t suture_gui_version(void) { return 0; }
__attribute__((weak)) uint32_t suture_gui_selftest_render(uint32_t *fb, int32_t w,
                                                          int32_t h, int32_t pitch_px) {
    (void)fb;
    (void)w;
    (void)h;
    (void)pitch_px;
    return 0;
}
__attribute__((weak)) uint32_t suture_gui_frames(void) { return 0; }
__attribute__((weak)) uint32_t suture_gui_swap_report(void) { return 0; }
__attribute__((weak)) uint32_t suture_gui_interactive_step(uint32_t *fb, int32_t w,
                                                           int32_t h, int32_t pitch,
                                                           int32_t mx, int32_t my,
                                                           int32_t buttons, int32_t cx,
                                                           int32_t cy, int32_t cb,
                                                           uint32_t now) {
    (void)fb; (void)w; (void)h; (void)pitch; (void)mx; (void)my;
    (void)buttons; (void)cx; (void)cy; (void)cb; (void)now;
    return 0;
}

int slint_gui_linked(void) { return suture_gui_version() != 0; }

/* v0.60.0: `guishell swapstat` - log the present/swap counters once. Returns the
 * present count (0 when the Rust object is not linked, so the caller can say so
 * honestly instead of printing zeros that look like a broken counter). */
uint32_t slint_gui_swap_report(void) { return suture_gui_swap_report(); }

/* ---- glyph access (v0.38.0) -------------------------------------------------
 * font8x16 lives in kernel/drivers/font.c, the CJK blob in kernel/term/cjk16.c.
 * Both are the assets the terminal/C desktop already render with, so the new
 * renderer stays visually consistent with them and adds no font data to the
 * image. The Rust side copies the glyph bytes into its own buffer and does the
 * alpha blending (it needs sub-pixel coverage for its own text alpha). */
extern const uint8_t font8x16[95 * 16];
extern const uint8_t *cjk16_lookup(uint32_t codepoint);
extern int cjk16_init(void);
extern int cjk16_count(void);

int slint_bridge_ascii(uint32_t ch, uint8_t *out16) {
    if (!out16 || ch < 32 || ch > 126) return 0;
    const uint8_t *glyph = font8x16 + (ch - 32) * 16;
    for (int i = 0; i < 16; i++) out16[i] = glyph[i];
    return 1;
}

int slint_bridge_cjk(uint32_t codepoint, uint8_t *out32) {
    if (!out32) return 0;
    if (cjk16_count() == 0) (void)cjk16_init();
    const uint8_t *glyph = cjk16_lookup(codepoint);
    if (!glyph) return 0;
    for (int i = 0; i < 32; i++) out32[i] = glyph[i];
    return 1;
}

int slint_bridge_cjk_ready(void) {
    if (cjk16_count() == 0) (void)cjk16_init();
    return cjk16_count() > 0 ? 1 : 0;
}

int slint_gui_render_frame(void) {
    if (!slint_gui_linked()) {
        console_printf("[gui-slint] not linked (install the Rust toolchain + "
                       "x86_64-unknown-none target)\n");
        return -1;
    }
    if (!limine_fb_addr || !limine_fb_width || !limine_fb_height) {
        console_printf("[gui-slint] no framebuffer\n");
        return -2;
    }
    const uint64_t bpp = limine_fb_bpp;
    if (bpp != 32) {
        console_printf("[gui-slint] need a 32bpp framebuffer, got %llu bpp\n",
                       (uint64_t)bpp);
        return -3;
    }
    const uint32_t w = (uint32_t)limine_fb_width;
    const uint32_t h = (uint32_t)limine_fb_height;
    const uint32_t pitch_px = (uint32_t)(limine_fb_pitch / 4u);
    if (pitch_px < w) {
        console_printf("[gui-slint] bad pitch (%llu bytes for %llu px)\n",
                       (uint64_t)limine_fb_pitch, (uint64_t)w);
        return -3;
    }

    const uint32_t sum = suture_gui_selftest_render((uint32_t *)(uintptr_t)limine_fb_addr,
                                                   (int32_t)w, (int32_t)h, (int32_t)pitch_px);
    /* v0.40.0: hand the just-rendered static image to the interactive loop as
     * its scene cache, so the loop starts immediately instead of re-rendering
     * the same scene (the analytic AA/shadow pass is seconds under TCG). */
    {
        extern uint32_t suture_gui_cache_seed(const uint32_t *fb, int32_t w,
                                              int32_t h, int32_t pitch_px);
        (void)suture_gui_cache_seed((const uint32_t *)(uintptr_t)limine_fb_addr,
                                    (int32_t)w, (int32_t)h, (int32_t)pitch_px);
    }
    console_printf("[gui-slint] frame rendered %llux%llu checksum=%llx frames=%llu %s\n",
                   (uint64_t)w, (uint64_t)h, (uint64_t)sum,
                   (uint64_t)suture_gui_frames(), sum ? "OK" : "EMPTY");
    if (!sum) return -4;
    return 0;
}

/* ===========================================================================
 * Interactive prototype (v0.40.0)
 * ---------------------------------------------------------------------------
 * The shell command `guislint` renders the static frame first (that keeps the
 * deterministic v0.39 pixel acceptance intact) and then calls this loop. It is
 * the smallest honest "live GUI" step: PS/2 mouse + keyboard are polled by the
 * kernel, one Rust frame is drawn per iteration, ESC leaves.
 *
 * Constraints that must stay: no sched_yield() (double-scheduler bug), bounded
 * pause spin for pacing, bounded lifetime as a safety net for automated runs.
 * =========================================================================== */
int slint_gui_run_interactive(void)
{
    extern int mbos_mouse_present(void);
    extern void mbos_mouse_poll(void);
    extern int mbos_mouse_x(void), mbos_mouse_y(void), mbos_mouse_buttons(void);
    extern int mbos_mouse_take_click(int *, int *, int *);
    extern void mbos_mouse_move_to(int x, int y);
    extern int mbos_kbd_poll(void);
    extern volatile uint64_t g_tick_count;

    if (!slint_gui_linked()) return -1;
    if (!limine_fb_addr || !limine_fb_width || !limine_fb_height) return -2;
    if (limine_fb_bpp != 32) return -3;
    const uint32_t w = (uint32_t)limine_fb_width;
    const uint32_t h = (uint32_t)limine_fb_height;
    const uint32_t pitch_px = (uint32_t)(limine_fb_pitch / 4u);
    if (pitch_px < w) return -3;
    uint32_t *fb = (uint32_t *)(uintptr_t)limine_fb_addr;

    const int mouse = mbos_mouse_present();
    if (mouse) mbos_mouse_move_to((int)w / 2, (int)h / 2);
    console_printf("[gui-slint] interactive: loop (mouse=%d), ESC exits\n", mouse);

    const uint64_t start = g_tick_count;
    int frames = 0;
    int quit = 0;
    while (!quit) {
        /* frame-based safety net: automated runs (and a lost ESC) cannot hang
         * the box. The tick rate differs per build, so count frames instead. */
        if (frames > 1800) break;

        if (mouse) mbos_mouse_poll();
        int mx = mouse ? mbos_mouse_x() : (int)w / 2;
        int my = mouse ? mbos_mouse_y() : (int)h / 2;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx >= (int)w) mx = (int)w - 1;
        if (my >= (int)h) my = (int)h - 1;
        const int buttons = mouse ? mbos_mouse_buttons() : 0;

        int cx = -1, cy = -1, cb = 0;
        if (mouse) (void)mbos_mouse_take_click(&cx, &cy, &cb);

        /* keyboard: ESC (make code 0x01) leaves; drain everything else */
        int sc;
        int key = -1;                      /* last pressed key this frame */
        while ((sc = mbos_kbd_poll()) >= 0) {
            if ((uint32_t)sc == 0x01u) {
                quit = 1;
            } else if ((sc & 0x80) == 0) {  /* make code only */
                key = sc;
            }
        }

        const uint32_t now_ms = (uint32_t)((g_tick_count - start) * 10u);
        (void)suture_gui_interactive_step(fb, (int32_t)w, (int32_t)h,
                                          (int32_t)pitch_px, mx, my, buttons,
                                          cx, cy, cb, now_ms);
        frames++;
        if ((frames % 120) == 0) {
            console_printf("[gui-slint] frame %d ms=%u pointer=%d,%d\n",
                           frames, now_ms, mx, my);
        }

        /* ~30 fps: bounded pause spin waiting for 3 ticks (100 Hz tick) */
        const uint64_t t0 = g_tick_count;
        while ((g_tick_count - t0) < 3u) {
            for (volatile int spin = 0; spin < 4000; spin++) { }
        }
    }
    console_printf("[gui-slint] interactive: exit after %d frames%s\n", frames,
                   quit ? "" : " (timeout)");
    return 0;
}

/* ===========================================================================
 * Desktop shell prototype (v0.41.0)
 * ---------------------------------------------------------------------------
 * Same polling loop as the interactive prototype, but the Rust side owns the
 * window state (two windows, taskbar, start menu): click to focus, drag by the
 * title bar, minimise/restore via the taskbar, maximise via the title-bar
 * button (or a double click on the taskbar button), start menu on the start
 * button. ESC leaves.
 * =========================================================================== */
/* v0.48.0: local wall clock for the taskbar (RTC is UTC, fixed +08:00 offset
 * until SutureOS grows a timezone database). */
static int clock_local_from_utc(int utc_hhmm)
{
    int total = (utc_hhmm / 100) * 60 + (utc_hhmm % 100) + 8 * 60;
    total %= 24 * 60;
    return (total / 60) * 100 + (total % 60);
}

/* v0.57.1: called by the Rust shell when the logo shutdown animation ends. */
void suture_power_off(void)
{
    extern void power_off(void);
    console_printf("[gui-shell] power off\n");
    power_off();
}

int slint_gui_run_shell(void)
{
    extern int mbos_kbd_decode(unsigned char scancode);
    extern int mbos_mouse_present(void);
    extern void mbos_mouse_poll(void);
    extern int mbos_mouse_x(void), mbos_mouse_y(void), mbos_mouse_buttons(void);
    extern int mbos_mouse_take_click(int *, int *, int *);
    extern void mbos_mouse_move_to(int x, int y);
    extern int mbos_kbd_poll(void);
    extern volatile uint64_t g_tick_count;

    if (!slint_gui_linked()) return -1;
    if (!limine_fb_addr || !limine_fb_width || !limine_fb_height) return -2;
    if (limine_fb_bpp != 32) return -3;
    const uint32_t w = (uint32_t)limine_fb_width;
    const uint32_t h = (uint32_t)limine_fb_height;
    const uint32_t pitch_px = (uint32_t)(limine_fb_pitch / 4u);
    if (pitch_px < w) return -3;
    uint32_t *fb = (uint32_t *)(uintptr_t)limine_fb_addr;

    /* real wall clock for the taskbar (was hardcoded 12:00 until v0.44.0) */
    extern uint64_t rtc_now_epoch(void);
    extern void rtc_format_epoch(uint64_t epoch, char *out, unsigned long outlen);
    char clockbuf[32];
    clockbuf[0] = 0;
    const uint64_t boot_epoch = rtc_now_epoch();
    rtc_format_epoch(boot_epoch, clockbuf, sizeof clockbuf);
    int clock_hhmm = 0;
    int clock_utc_hhmm = 0;
    uint64_t clock_refresh_tick = 0;
    {
        /* "YYYY-MM-DD HH:MM:SS" -> HH*100+MM (UTC) */
        const char *sp = clockbuf;
        while (*sp && *sp != ' ') sp++;
        if (*sp == ' ') sp++;
        int hh = 0, mm = 0;
        if (sp[0] >= '0' && sp[0] <= '9' && sp[1] >= '0' && sp[1] <= '9' &&
            sp[3] >= '0' && sp[3] <= '9' && sp[4] >= '0' && sp[4] <= '9') {
            hh = (sp[0] - '0') * 10 + (sp[1] - '0');
            mm = (sp[3] - '0') * 10 + (sp[4] - '0');
        }
        clock_utc_hhmm = hh * 100 + mm;
        clock_hhmm = clock_local_from_utc(clock_utc_hhmm);
        console_printf("[gui-shell] clock utc=%02d:%02d local=%02d:%02d (+08:00)\n",
                       clock_utc_hhmm / 100, clock_utc_hhmm % 100,
                       clock_hhmm / 100, clock_hhmm % 100);
    }

    const int mouse = mbos_mouse_present();
    if (mouse) mbos_mouse_move_to((int)w / 2, (int)h - 120);
    console_printf("[gui-shell] shell: loop (mouse=%d), ESC exits\n", mouse);

    const uint64_t start = g_tick_count;
    int frames = 0;
    int quit = 0;

    /* v0.59.0: keystroke queue. One frame can drain many bytes from the 8042,
     * but the Rust step accepts ONE key/character per frame; keeping only the
     * last one silently dropped fast keystrokes (measured: "hello" -> "hllo"). */
    struct kbd_event { int sc; int ch; };
    static struct kbd_event kq[32];
    static int kq_head, kq_tail;
    while (!quit) {
        /* safety nets: frame count is now cheap (cursor fast path ~13 fps),
         * so keep a generous count plus an absolute time cap */
        if (frames > 20000) break;
        if (((g_tick_count - start) * 10u) > 900000u) break;
        if (mouse) mbos_mouse_poll();
        int mx = mouse ? mbos_mouse_x() : (int)w / 2;
        int my = mouse ? mbos_mouse_y() : (int)h / 2;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx >= (int)w) mx = (int)w - 1;
        if (my >= (int)h) my = (int)h - 1;
        const int buttons = mouse ? mbos_mouse_buttons() : 0;

        int cx = -1, cy = -1, cb = 0;
        if (mouse) (void)mbos_mouse_take_click(&cx, &cy, &cb);

        int sc;
        while ((sc = mbos_kbd_poll()) >= 0) {
            /* v0.58.0: decode every byte (make *and* break) so the shell gets a
             * character for its per-window text line; mbos_kbd_decode() owns the
             * shift/caps state. v0.59.0: queue the events instead of dropping all
             * but the last one. */
            const int dec = mbos_kbd_decode((unsigned char)sc);
            if ((uint32_t)sc == 0x01u) {
                quit = 1;
                continue;
            }
            if ((sc & 0x80) == 0 || dec) {
                const int next = (kq_tail + 1) % 32;
                if (next != kq_head) {
                    kq[kq_tail].sc = (sc & 0x80) ? -1 : sc;
                    kq[kq_tail].ch = dec;
                    kq_tail = next;
                }
            }
        }
        int key = -1;                      /* key/char served this frame */
        int chr = -1;
        if (kq_head != kq_tail) {
            key = kq[kq_head].sc;
            chr = kq[kq_head].ch;
            kq_head = (kq_head + 1) % 32;
        }

        /* refresh the wall clock once a minute (6000 ticks at 100 Hz) */
        if ((g_tick_count - clock_refresh_tick) >= 6000u) {
            clock_refresh_tick = g_tick_count;
            const uint64_t abs = boot_epoch + (g_tick_count - start) / 100u;
            clock_utc_hhmm = (int)((abs / 3600u) % 24u) * 100 + (int)((abs / 60u) % 60u);
            clock_hhmm = clock_local_from_utc(clock_utc_hhmm);
            console_printf("[gui-shell] clock update local=%02d:%02d\n",
                           clock_hhmm / 100, clock_hhmm % 100);
        }

        const uint32_t now_ms = (uint32_t)((g_tick_count - start) * 10u);
        (void)suture_gui_shell_step(fb, (int32_t)w, (int32_t)h, (int32_t)pitch_px,
                                    mx, my, buttons, cx, cy, cb, now_ms, key, chr,
                                    clock_hhmm);
        frames++;
        if ((frames % 30) == 0) {
            console_printf("[gui-shell] frame %d ms=%u pointer=%d,%d\n",
                           frames, now_ms, mx, my);
        }

        const uint64_t t0 = g_tick_count;
        while ((g_tick_count - t0) < 3u) {
            for (volatile int spin = 0; spin < 4000; spin++) { }
        }
    }
    console_printf("[gui-shell] shell: exit after %d frames%s\n", frames,
                   quit ? "" : " (timeout)");
    return 0;
}
