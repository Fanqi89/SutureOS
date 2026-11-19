/* SutureOS: Slint GUI prototype bridge (Stage 0, v0.37 prototype).
 *
 * `rust/suture-gui` is a no_std staticlib (crate-type = staticlib, target
 * x86_64-unknown-none) that contains Slint 1.18 + its software renderer. It is
 * linked into the kernel by scripts/build.py exactly like `rust/suture-core`:
 * every object of the archive except compiler_builtins is extracted and linked
 * as a plain object (whole-archive would drag in compiler_builtins' memcpy /
 * memset and collide with kernel/lib/string.c).
 *
 * This file is the C side of the boundary:
 *   - declares the Rust entry points,
 *   - carries WEAK fallbacks, so a machine without the Rust toolchain still
 *     links and `guislint` reports "not linked" instead of failing to build,
 *   - owns the framebuffer discovery + the serial-log line that proves a frame
 *     was really rendered.
 *
 * Deliberate scope: this draws ONE static prototype frame over the current
 * screen and waits for a key. It does not touch the existing C desktop
 * (kernel/gui/gui.c), the start menu, the taskbar, or the boot presentation.
 */
#ifndef SUTUREOS_GUI_SLINT_BRIDGE_H
#define SUTUREOS_GUI_SLINT_BRIDGE_H

#include <stitch/types.h>

/* Render one prototype frame into the linear framebuffer.
 * Returns 0 on success (a frame was drawn), negative on failure:
 *   -1 rust object not linked, -2 no framebuffer, -3 unsupported bpp,
 *   -4 the renderer produced no pixels. */
int slint_gui_render_frame(void);

/* 1 when the Rust GUI object is linked into this image. */
int slint_gui_linked(void);

/* ---- glyph access for the Rust renderer (v0.38.0 text support) ----
 * The kernel already owns two bitmap fonts; the Rust rasteriser must draw with
 * the SAME assets rather than ship a second copy (and the CJK blob is 238 KiB):
 *
 *   slint_bridge_ascii(ch, out)   font8x16 (ASCII 32..126), 16 bytes, 8x16,
 *                                one byte per row, MSB = leftmost pixel.
 *   slint_bridge_cjk(cp, out)     cjk16 (GB2312 subset), 32 bytes, 16x16,
 *                                two bytes per row, MSB = leftmost pixel.
 *
 * Both return 1 on success, 0 when the codepoint is not covered. */
int slint_bridge_ascii(uint32_t ch, uint8_t *out16);
int slint_bridge_cjk(uint32_t codepoint, uint8_t *out32);

/* 1 when the CJK blob was initialised and has glyphs. */
int slint_bridge_cjk_ready(void);

#endif
