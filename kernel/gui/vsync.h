/* SutureOS: real vertical-retrace (vblank) present primitive.
 *
 * Why this file exists: the Rust desktop shell and the compositor both pace
 * presents on a *software* 60 Hz grid built from the HPET (see
 * kernel/gui/slint_bridge.c: suture_gui_time_ns()), because SutureOS has no GPU
 * or display interrupt. The one hardware vblank signal an x86 PC still exposes
 * without a driver model is the VGA-compatible Input Status Register 1 at port
 * 0x3DA: bit 3 reads 1 while the beam is inside vertical retrace. This module
 * turns that bit into a bounded `vsync_present()` edge wait, measures the real
 * edge rate with the HPET nanosecond clock, and reports honest statistics.
 *
 * The implementation deliberately never blocks: every wait has both a
 * wall-clock bound (~40 ms) and a pause-spin count bound, so a source that
 * stops toggling degrades to `0` (timeout) or `-1` (unavailable) instead of
 * hanging the GUI. See docs/vsync.md for the measured QEMU numbers and the
 * limitations (this is register polling, not a scanline IRQ).
 *
 * API contract (the slint_bridge.c call site depends on these signatures):
 *   vsync_init()      0 = a usable vblank source exists, -1 = none
 *   vsync_available() 1/0
 *   vsync_present()   1 = waited for the next rising edge, 0 = timed out
 *                     (bounded, ~40 ms), -1 = no source
 *   vsync_hz100()     measured refresh rate x100 (60.00 Hz -> 6000), 0 = unknown
 *   vsync_stats()     cumulative edges / timeouts / worst wait in microseconds
 *   vsync_selftest()  prints "[vsync] selftest: ... PASS|FAIL", 0 = PASS
 */
#ifndef SUTUREOS_VSYNC_H
#define SUTUREOS_VSYNC_H

#include <stitch/types.h>

/* Lazily probe the VGA status port and latch 60 rising edges with HPET
 * timestamps. Returns 0 when a display-rate vblank source is available, -1
 * otherwise (no edges, too few edges, or an edge rate that is not a display
 * refresh rate). Safe to call repeatedly; only the first call measures. */
int vsync_init(void);

/* 1 when vsync_present() can be used, 0 otherwise. */
int vsync_available(void);

/* Wait for the next rising edge of the vertical-retrace bit.
 *   1  = the edge arrived, wait time was recorded in the statistics
 *   0  = bounded wait expired (counted as a timeout)
 *  -1  = no usable source (present() must not be called in a loop then)
 * Bounded by ~40 ms of wall clock and by a pause-spin cap, never blocks. */
int vsync_present(void);

/* Measured refresh rate x100 (6000 == 60.00 Hz). 0 when unknown, which is
 * also what an unusable source reports on purpose: callers must not pace on a
 * rate that failed validation. */
int vsync_hz100(void);

/* Cumulative statistics. Any pointer may be NULL. */
void vsync_stats(uint32_t *edges, uint32_t *timeouts, uint32_t *max_wait_us);

/* Print "[vsync] selftest: hz=<x.yy> edges=N timeouts=M max_wait_us=K PASS|FAIL"
 * and return 0 on PASS. FAIL means: no source, measured hz outside 50..75 Hz,
 * or more than 20% of the presents in the selftest window timed out. */
int vsync_selftest(void);

/* ---- diagnostics for the `vsynctest` shell command ----------------------
 * These are deliberately *not* part of the present-path API: they expose what
 * was measured even when the source was rejected, so a human (and the host
 * acceptance script) can see the honest numbers instead of a silent zero. */

/* Raw VGA CRTC timing registers captured by the cross-check, plus the nominal
 * refresh rate derived from them (x100, 0 = not usable). */
struct vsync_crtc_info {
    uint8_t h_total;        /* CRTC 0x00 */
    uint8_t h_display_end;  /* CRTC 0x01 */
    uint8_t v_total;        /* CRTC 0x06 */
    uint8_t overflow;       /* CRTC 0x07 */
    uint8_t v_sync_start;   /* CRTC 0x10 */
    uint8_t v_sync_end;     /* CRTC 0x11 */
    uint8_t v_display_end;  /* CRTC 0x12 */
    uint8_t clocking;       /* sequencer clocking mode */
    uint8_t misc;           /* misc output register (dot-clock select in 2..3) */
    uint32_t nominal_hz100; /* derived nominal rate x100, 0 = unavailable */
};

/* Nominal refresh rate from the VGA CRTC registers x100, 0 when the values are
 * not usable (the misc output register gives the dot clock). This is the
 * cross-check, not the measurement. */
int vsync_nominal_hz100(void);

/* The CRTC registers behind vsync_nominal_hz100(). Never NULL. */
const struct vsync_crtc_info *vsync_crtc_info(void);

/* Measured edge rate x100 even when the source was rejected as unusable;
 * 0 when nothing could be measured. */
uint32_t vsync_observed_hz100(void);

/* Measured edge spacing in nanoseconds (first-to-last over the latched edges)
 * plus the shortest / longest single spacing, 0 when unknown. */
uint64_t vsync_period_ns(void);
uint64_t vsync_min_period_ns(void);
uint64_t vsync_max_period_ns(void);

/* Average and worst wait of the present() calls seen so far, in microseconds. */
uint32_t vsync_avg_wait_us(void);
uint32_t vsync_max_wait_us(void);

/* Count raw rising edges on port 0x3DA over a wall-clock window. `samples` and
 * `high` (both optional) receive how many times the bit was read and how many
 * of those reads saw it set, which is how "the signal really toggles" is
 * proven instead of assumed. Returns the edge count. */
uint32_t vsync_edges_in_window(uint32_t ms, uint32_t *samples, uint32_t *high);

/* Number of rising edges the init latch managed to collect (60 = complete). */
uint32_t vsync_latch_edges(void);

/* Format a x100 rate as "60.02" (the kernel printf has no %f and no zero
 * padding). `out` needs at least 8 bytes. Shared with the shell command. */
void vsync_format_hz(char *out, uint32_t hz100);

/* Short human-readable status: why the source is or is not available. */
const char *vsync_status_text(void);

/* Check the hardware-independent half of the logic - period -> Hz arithmetic,
 * the fixed-point formatter and the accept/reject rate policy - against known
 * values, so the code path real hardware takes is proven even where port 0x3DA
 * turns out to be unusable. Prints "[vsync] math: ... PASS|FAIL", 0 = PASS. */
int vsync_math_selftest(void);

#endif /* SUTUREOS_VSYNC_H */
