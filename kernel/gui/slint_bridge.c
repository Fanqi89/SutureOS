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
