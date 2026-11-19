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

#endif
