/* SutureOS: real vertical-retrace (vblank) present primitive - implementation.
 *
 * The only hardware vblank signal a plain x86 PC exposes without a GPU driver
 * is the VGA-compatible Input Status Register 1 at port 0x3DA: bit 3 is 1
 * while the beam is inside vertical retrace, bit 0 is 1 during blanking.
 * Reading that port has a documented side effect (it resets the attribute
 * controller flip-flop), which is acceptable here because SutureOS never
 * programs the VGA attribute controller after Limine hands over the
 * framebuffer, but it is listed in docs/vsync.md as a real limitation.
 *
 * Everything below is measured, never assumed:
 *   - the init latch timestamps 60 consecutive rising edges with
 *     time_source_now_ns() (HPET, 100 MHz) and derives period / min / max / Hz;
 *   - the CRTC registers give a *nominal* refresh rate as a cross-check;
 *   - a source whose measured edge rate is not a display refresh rate
 *     (40..120 Hz) is rejected instead of being silently trusted, so callers
 *     fall back to their software pacing grid instead of pacing on nonsense;
 *   - vsync_edges_in_window() counts raw edges over wall-clock time so the
 *     acceptance script can prove the bit really toggles (not stuck at 0/1).
 *
 * Bounded by construction: vsync_present() has a ~40 ms wall-clock deadline and
 * a pause-spin cap, and it never sleeps on an interrupt that does not exist.
 */
#include "vsync.h"

#include <stitch/types.h>
#include <stitch/io.h>
#include <stitch/string.h>
#include <stitch/console.h>

#include "time_source.h"

/* ---- VGA-compatible register interface ---------------------------------- */
#define VGA_STATUS_PORT   0x3DAu   /* input status register 1 (colour) */
#define VGA_VRETRACE_BIT  0x08u    /* bit 3: vertical retrace in progress */
#define VGA_SEQ_INDEX     0x3C4u
#define VGA_SEQ_DATA      0x3C5u
#define VGA_CRTC_INDEX    0x3D4u
#define VGA_CRTC_DATA     0x3D5u
#define VGA_MISC_READ     0x3CCu   /* read-only misc output register */

#define VGA_SEQ_CLOCK_MODE 0x01u
#define VGA_SEQ_SCREEN_OFF 0x20u   /* bit 5 of clocking mode */

/* ---- measurement / policy constants ------------------------------------ */
#define VSYNC_EDGE_TARGET      60u          /* edges the init latch wants */
#define VSYNC_LATCH_BUDGET_NS  2000000000ull /* 2 s to collect them, else no source */
#define VSYNC_WAIT_LIMIT_NS    40000000ull   /* 40 ms bound for vsync_present() */
#define VSYNC_SPIN_CAP         1000000u      /* backstop only: dead-clock protection */
#define VSYNC_HZ_MIN_OK        4000          /* x100: 40.00 Hz */
#define VSYNC_HZ_MAX_OK        12000         /* x100: 120.00 Hz */
#define VSYNC_TEST_HZ_MIN      5000          /* selftest window: 50.00 Hz */
#define VSYNC_TEST_HZ_MAX      7500          /* selftest window: 75.00 Hz */
#define VSYNC_TEST_STREAK       30

/* ---- state ------------------------------------------------------------- */
static int g_inited;
static int g_available;
static const char *g_reason = "not probed";

static struct vsync_crtc_info g_crtc;
static int g_nominal_hz100;

static uint32_t g_latch_edges;
static uint32_t g_observed_hz100;
static uint64_t g_period_ns;
static uint64_t g_min_period_ns;
static uint64_t g_max_period_ns;

static uint32_t g_edges;        /* successful vsync_present() waits */
static uint32_t g_timeouts;     /* bounded waits that expired */
static uint32_t g_max_wait_us;
static uint32_t g_presents;     /* attempts (edges + timeouts) */
static uint64_t g_wait_ns_total;

/* stats of the most recent vsync_edges_in_window() call are returned through
 * the caller's out-parameters (see the header) - no hidden state is kept. */

/* ---- low level --------------------------------------------------------- */

static int vretrace_level(void)
{
    return (inb(VGA_STATUS_PORT) & VGA_VRETRACE_BIT) ? 1 : 0;
}

static uint8_t crtc_read(uint8_t index)
{
    outb(VGA_CRTC_INDEX, index);
    return inb(VGA_CRTC_DATA);
}

/* "60.02" - the kernel's vsnprintf has no %f and no zero padding, so the
 * fixed-point refresh rate is formatted by hand. Shared with `vsynctest`. */
void vsync_format_hz(char *out, uint32_t hz100)
{
    char digits[12];
    int n = 0;
    uint32_t ip = hz100 / 100u;

    if (!ip) {
        digits[n++] = '0';
    } else {
        while (ip) {
            digits[n++] = (char)('0' + (ip % 10u));
            ip /= 10u;
        }
    }
    int k = 0;
    while (n) out[k++] = digits[--n];
    out[k++] = '.';
    out[k++] = (char)('0' + ((hz100 / 10u) % 10u));
    out[k++] = (char)('0' + (hz100 % 10u));
    out[k] = '\0';
}

/* Refresh rate x100 from an edge spacing. Kept as one function so the exact
 * arithmetic that runs on real hardware is exercised by vsync_math_selftest()
 * even on a machine whose port 0x3DA is useless (e.g. QEMU). */
static uint32_t hz100_from_period(uint64_t period_ns)
{
    if (!period_ns) return 0;
    /* Round to the nearest 0.01 Hz instead of truncating: an exact 16.666667 ms
     * frame must report 60.00 Hz and not 59.99 (the pure division truncates to
     * 5999 there, and the selftest below pins this down). */
    const uint64_t hz100 = (100ull * 1000000000ull + period_ns / 2ull) / period_ns;
    return (hz100 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)hz100;
}

/* Unlock the sequencer (clear screen-off), read the CRTC timing registers and
 * derive the *nominal* refresh rate from the VGA dot clock. This mirrors the
 * standard VGA formula:
 *     hz = (dot_clock / dots_per_char) / ((h_total + 5) * (v_total + 2))
 * with the 8/9-dot (or 8/9-clock) doubling bit from the clocking mode and the
 * 9-bit vertical total split across CRTC 0x07. It is a cross-check only: the
 * measured edge rate is what the API reports. Returns 0 when the values are
 * unusable (e.g. all zero after a non-VGA mode set). */
static int crtc_measure(struct vsync_crtc_info *info)
{
    static const uint32_t clk_hz[4] = { 25175000u, 28322000u, 25175000u, 25175000u };

    /* Unlock the sequencer exactly as the hardware docs describe. The index
     * registers are documented as readable; only write them back when the
     * value looks like a real index, so a bogus read cannot corrupt state. */
    const uint8_t seq_saved = inb(VGA_SEQ_INDEX);
    outb(VGA_SEQ_INDEX, VGA_SEQ_CLOCK_MODE);
    const uint8_t clocking = inb(VGA_SEQ_DATA);
    outb(VGA_SEQ_DATA, (uint8_t)(clocking & (uint8_t)~VGA_SEQ_SCREEN_OFF));

    const uint8_t crtc_saved = inb(VGA_CRTC_INDEX);
    const uint8_t h_total = crtc_read(0x00);
    const uint8_t h_display_end = crtc_read(0x01);
    const uint8_t v_total = crtc_read(0x06);
    const uint8_t overflow = crtc_read(0x07);
    const uint8_t v_sync_start = crtc_read(0x10);
    const uint8_t v_sync_end = crtc_read(0x11);
    const uint8_t v_display_end = crtc_read(0x12);
    if (crtc_saved <= 0x18u) outb(VGA_CRTC_INDEX, crtc_saved);

    /* Put the sequencer back exactly as it was (clocking mode, then the index),
     * so a probe can never leave the screen disabled/enabled differently. */
    outb(VGA_SEQ_INDEX, VGA_SEQ_CLOCK_MODE);
    outb(VGA_SEQ_DATA, clocking);
    if (seq_saved <= 0x07u) outb(VGA_SEQ_INDEX, seq_saved);

    const uint8_t msr = inb(VGA_MISC_READ);

    info->h_total = h_total;
    info->h_display_end = h_display_end;
    info->v_total = v_total;
    info->overflow = overflow;
    info->v_sync_start = v_sync_start;
    info->v_sync_end = v_sync_end;
    info->v_display_end = v_display_end;
    info->clocking = clocking;
    info->misc = msr;

    const uint32_t clock_sel = (uint32_t)((msr >> 2) & 3u);
    const uint32_t dots = (msr & 1u) ? 8u : 9u;   /* 8 or 9 dots per character */
    uint32_t htotal_chars = (uint32_t)h_total + 5u;
    uint32_t vtotal_lines = (uint32_t)v_total |
                            ((((uint32_t)overflow & 1u) |
                              (((uint32_t)overflow >> 4) & 2u)) << 8);
    vtotal_lines += 2u;
    if ((clocking >> 3) & 1u) htotal_chars <<= 1u;   /* clocking-mode doubling */

    info->nominal_hz100 = 0;
    if (!htotal_chars || !vtotal_lines) return 0;

    const uint64_t chars_per_sec = (uint64_t)clk_hz[clock_sel] / dots;
    const uint64_t denom = (uint64_t)htotal_chars * (uint64_t)vtotal_lines;
    if (!denom) return 0;
    const uint32_t hz100 = (uint32_t)((chars_per_sec * 100ull) / denom);
    info->nominal_hz100 = hz100;
    return (int)hz100;
}

/* ---- init -------------------------------------------------------------- */

int vsync_init(void)
{
    if (g_inited) return g_available ? 0 : -1;
    g_inited = 1;

    g_nominal_hz100 = crtc_measure(&g_crtc);

    int prev = vretrace_level();
    uint64_t first_ns = 0, last_ns = 0, prev_edge_ns = 0;
    uint64_t min_ns = 0, max_ns = 0;
    uint32_t edges = 0;
    uint32_t spins = 0;
    const uint64_t t_start = time_source_now_ns();

    while (edges < VSYNC_EDGE_TARGET) {
        const int cur = vretrace_level();
        if (!prev && cur) {                       /* 0 -> 1: start of retrace */
            const uint64_t now = time_source_now_ns();
            if (edges == 0) {
                first_ns = now;
            } else {
                const uint64_t d = now - prev_edge_ns;
                if (edges == 1u || d < min_ns) min_ns = d;
                if (d > max_ns) max_ns = d;
            }
            prev_edge_ns = now;
            last_ns = now;
            edges++;
        }
        prev = cur;
        __asm__ volatile("pause");
        if (((++spins & 0x1FFu) == 0u) &&
            (time_source_now_ns() - t_start) >= VSYNC_LATCH_BUDGET_NS) {
            break;                                /* 2 s gone: not a usable source */
        }
    }

    g_latch_edges = edges;
    if (edges >= 2u) {
        g_period_ns = (last_ns - first_ns) / (uint64_t)(edges - 1u);
        g_min_period_ns = min_ns;
        g_max_period_ns = max_ns;
        g_observed_hz100 = hz100_from_period(g_period_ns);
    }

    if (edges < VSYNC_EDGE_TARGET) {
        g_reason = (edges == 0u)
            ? "no rising edge on 0x3DA bit 3 within 2 s"
            : "fewer than 60 rising edges within 2 s";
        g_available = 0;
    } else if (g_observed_hz100 < (uint32_t)VSYNC_HZ_MIN_OK ||
               g_observed_hz100 > (uint32_t)VSYNC_HZ_MAX_OK) {
        /* The bit moves, but not once per displayed frame: never pace on it. */
        g_reason = "edge rate is not a display refresh rate";
        g_available = 0;
    } else {
        g_reason = "VGA 0x3DA bit 3 polling";
        g_available = 1;
    }

    char obs[12], nom[12];
    vsync_format_hz(obs, g_observed_hz100);
    vsync_format_hz(nom, (uint32_t)(g_nominal_hz100 > 0 ? g_nominal_hz100 : 0));
    console_printf("[vsync] probe: latch_edges=%u observed_hz=%s nominal_hz=%s period_ns=%llu min_ns=%llu max_ns=%llu -> %s\n",
                   (unsigned)g_latch_edges,
                   obs, nom,
                   (unsigned long long)g_period_ns,
                   (unsigned long long)g_min_period_ns,
                   (unsigned long long)g_max_period_ns,
                   g_available ? "available" : "unavailable");
    return g_available ? 0 : -1;
}

int vsync_available(void)
{
    if (!g_inited) (void)vsync_init();
    return g_available;
}

int vsync_present(void)
{
    if (!g_inited && vsync_init() != 0) return -1;
    if (!g_available) return -1;

    const uint64_t t0 = time_source_now_ns();
    int prev = vretrace_level();
    uint32_t spins = 0;

    for (;;) {
        const int cur = vretrace_level();
        if (!prev && cur) {
            const uint64_t now = time_source_now_ns();
            const uint64_t wait = now - t0;
            const uint32_t us = (uint32_t)(wait / 1000ull);
            g_edges++;
            g_presents++;
            g_wait_ns_total += wait;
            if (us > g_max_wait_us) g_max_wait_us = us;
            return 1;
        }
        prev = cur;
        __asm__ volatile("pause");
        if ((++spins & 0xFFu) == 0u) {
            if ((time_source_now_ns() - t0) >= VSYNC_WAIT_LIMIT_NS) break;
            if (spins >= VSYNC_SPIN_CAP) break;   /* dead wall clock backstop */
        }
    }

    g_timeouts++;
    g_presents++;
    if (g_max_wait_us < (uint32_t)(VSYNC_WAIT_LIMIT_NS / 1000ull)) {
        g_max_wait_us = (uint32_t)(VSYNC_WAIT_LIMIT_NS / 1000ull);
    }
    return 0;
}

int vsync_hz100(void)
{
    if (!g_inited) (void)vsync_init();
    /* Deliberately 0 for a rejected source: callers must not pace on a rate
     * that failed validation, and 0 already means "unknown" in the contract. */
    return g_available ? (int)g_observed_hz100 : 0;
}

void vsync_stats(uint32_t *edges, uint32_t *timeouts, uint32_t *max_wait_us)
{
    if (edges) *edges = g_edges;
    if (timeouts) *timeouts = g_timeouts;
    if (max_wait_us) *max_wait_us = g_max_wait_us;
}

int vsync_selftest(void)
{
    if (!g_inited) (void)vsync_init();

    const uint32_t e0 = g_edges;
    const uint32_t t0 = g_timeouts;
    if (g_available) {
        for (int i = 0; i < VSYNC_TEST_STREAK; i++) (void)vsync_present();
    }
    const uint32_t de = g_edges - e0;
    const uint32_t dt = g_timeouts - t0;
    const uint32_t attempts = de + dt;

    const int hz_ok = g_available &&
                      g_observed_hz100 >= (uint32_t)VSYNC_TEST_HZ_MIN &&
                      g_observed_hz100 <= (uint32_t)VSYNC_TEST_HZ_MAX;
    const int timeout_ok = (attempts == 0u)
        ? g_available
        : ((uint64_t)dt * 100ull <= (uint64_t)attempts * 20ull);
    const int pass = g_available && hz_ok && timeout_ok;

    char hz[12];
    vsync_format_hz(hz, g_available ? g_observed_hz100 : 0u);
    console_printf("[vsync] selftest: hz=%s edges=%u timeouts=%u max_wait_us=%u %s\n",
                   hz,
                   (unsigned)g_edges,
                   (unsigned)g_timeouts,
                   (unsigned)g_max_wait_us,
                   pass ? "PASS" : "FAIL");
    return pass ? 0 : -1;
}

/* ---- diagnostics ------------------------------------------------------- */

/* Prove the parts of the logic that do not depend on the hardware being
 * usable: the period -> Hz arithmetic, the fixed-point formatter and the
 * accept/reject rate policy. This is the code path real hardware would take,
 * so it is worth checking even when 0x3DA turns out to be a per-read toggle. */
int vsync_math_selftest(void)
{
    static const struct { uint64_t period_ns; uint32_t want_hz100; } cases[] = {
        { 16666667ull, 6000u },   /* 60.00 Hz  (the usual 60 Hz frame) */
        { 16659733ull, 6002u },   /* 60.02 Hz  (NTSC-ish)              */
        { 20000000ull, 5000u },   /* 50.00 Hz  (low end of the policy) */
        {  8333333ull, 12000u },  /* 120.00 Hz (high end of the policy) */
        { 13888889ull, 7200u },   /* 72.00 Hz */
    };
    int ok = 1;
    char buf[12];

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (hz100_from_period(cases[i].period_ns) != cases[i].want_hz100) ok = 0;
        if (hz100_from_period(cases[i].period_ns) < (uint32_t)VSYNC_HZ_MIN_OK ||
            hz100_from_period(cases[i].period_ns) > (uint32_t)VSYNC_HZ_MAX_OK) ok = 0;
    }
    if (hz100_from_period(0) != 0u) ok = 0;

    /* 1e9/3.6 us is the measured per-read toggle rate on QEMU: it must be
     * rejected, otherwise present() would "sync" to the port read itself. */
    if (hz100_from_period(3603ull) >= (uint32_t)VSYNC_HZ_MIN_OK &&
        hz100_from_period(3603ull) <= (uint32_t)VSYNC_HZ_MAX_OK) ok = 0;

    vsync_format_hz(buf, 6002u);
    if (strcmp(buf, "60.02") != 0) ok = 0;
    vsync_format_hz(buf, 0u);
    if (strcmp(buf, "0.00") != 0) ok = 0;
    vsync_format_hz(buf, 12345u);
    if (strcmp(buf, "123.45") != 0) ok = 0;
    vsync_format_hz(buf, 7884u);
    if (strcmp(buf, "78.84") != 0) ok = 0;

    console_printf("[vsync] math: period->hz conversions, formatter and rate policy %s\n",
                   ok ? "PASS" : "FAIL");
    return ok ? 0 : -1;
}

int vsync_nominal_hz100(void)
{
    if (!g_inited) (void)vsync_init();
    return g_nominal_hz100;
}

const struct vsync_crtc_info *vsync_crtc_info(void)
{
    if (!g_inited) (void)vsync_init();
    return &g_crtc;
}

uint32_t vsync_observed_hz100(void)
{
    if (!g_inited) (void)vsync_init();
    return g_observed_hz100;
}

uint64_t vsync_period_ns(void)
{
    if (!g_inited) (void)vsync_init();
    return g_period_ns;
}

uint64_t vsync_min_period_ns(void)
{
    if (!g_inited) (void)vsync_init();
    return g_min_period_ns;
}

uint64_t vsync_max_period_ns(void)
{
    if (!g_inited) (void)vsync_init();
    return g_max_period_ns;
}

uint32_t vsync_avg_wait_us(void)
{
    if (!g_presents) return 0;
    return (uint32_t)((g_wait_ns_total / (uint64_t)g_presents) / 1000ull);
}

uint32_t vsync_max_wait_us(void)
{
    return g_max_wait_us;
}

uint32_t vsync_edges_in_window(uint32_t ms, uint32_t *samples, uint32_t *high)
{
    if (!g_inited) (void)vsync_init();
    if (!ms) ms = 1;

    const uint64_t t0 = time_source_now_ns();
    const uint64_t limit = (uint64_t)ms * 1000000ull;
    int prev = vretrace_level();
    uint32_t edges = 0, n = 0, hi = 0, spins = 0;

    for (;;) {
        const int cur = vretrace_level();
        if (!prev && cur) edges++;
        if (cur) hi++;
        n++;
        prev = cur;
        __asm__ volatile("pause");
        if (((++spins & 0x1FFu) == 0u) && (time_source_now_ns() - t0) >= limit) break;
    }

    if (samples) *samples = n;
    if (high) *high = hi;
    return edges;
}

uint32_t vsync_latch_edges(void)
{
    if (!g_inited) (void)vsync_init();
    return g_latch_edges;
}

const char *vsync_status_text(void)
{
    if (!g_inited) (void)vsync_init();
    return g_reason;
}
