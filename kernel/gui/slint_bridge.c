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

int slint_gui_linked(void) { return suture_gui_version() != 0; }

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
    console_printf("[gui-slint] frame rendered %llux%llu checksum=%llx frames=%llu %s\n",
                   (uint64_t)w, (uint64_t)h, (uint64_t)sum,
                   (uint64_t)suture_gui_frames(), sum ? "OK" : "EMPTY");
    if (!sum) return -4;
    return 0;
}
