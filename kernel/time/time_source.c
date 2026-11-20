/* SutureOS: generic time-source registry (domain 11: cross-architecture blueprint)
 *
 * Why this file exists: every non-x86 64-bit porting source in the material
 * library (LoongOS / token-os on AArch64, MikuOS on RISC-V) builds time on the
 * same arch-independent shape - one monotonic counter with a firmware-provided
 * frequency, plus a scheduler tick, and everything else asks the kernel for
 * nanoseconds. SutureOS had three private counters (HPET ticks, LAPIC timer
 * jiffies, 8254) and no common statement of "what time is it". This file
 * re-writes that mechanism on x86_64: a small registry of counters whose
 * frequencies are known, a best-source selector and one nanosecond clock.
 *
 * Upstreams read for the mechanism only (no code copied, GPL-3.0-compatible):
 *   - LoongOS  (MIT, AArch64 bare metal: generic timer + tick abstraction)
 *   - token-os (MIT, aarch64 bare-metal kernel)
 *   - MikuOS   (RISC-V 64 kernel, repository reference)
 *
 * The x86_64 side of the abstraction is ours: HPET (firmware period) is the
 * high-resolution source, the 100 Hz scheduler tick is the monotonic fallback,
 * and the 8254 PIT remains what the LAPIC timer is calibrated against.
 */
#include "time_source.h"

#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/selftest.h>

#include "hpet.h"
#include "mb_pit.h"

extern volatile uint64_t g_tick_count;   /* incremented by the timer IRQ (100 Hz) */

#define TIME_SOURCE_MAX 4

static const time_source_t *g_sources[TIME_SOURCE_MAX];
static int g_source_count;

/* ---- x86_64 source readers ------------------------------------------------ */

static uint64_t ts_hpet_ticks(void)
{
    return hanos_hpet_read_counter();
}

static uint64_t ts_jiffies_ticks(void)
{
    return g_tick_count;
}

/* ---- registry ------------------------------------------------------------- */

void time_source_register(const time_source_t *src)
{
    if (!src || !src->name || !src->read || src->freq_hz == 0) return;
    if (g_source_count >= TIME_SOURCE_MAX) return;
    /* replace an entry with the same name (re-init after a re-probe) */
    for (int i = 0; i < g_source_count; i++) {
        if (strcmp(g_sources[i]->name, src->name) == 0) {
            g_sources[i] = src;
            return;
        }
    }
    g_sources[g_source_count++] = src;
}

int time_source_count(void)
{
    return g_source_count;
}

const time_source_t *time_source_at(int index)
{
    if (index < 0 || index >= g_source_count) return NULL;
    return g_sources[index];
}

const time_source_t *time_source_best(void)
{
    const time_source_t *best = NULL;
    for (int i = 0; i < g_source_count; i++) {
        if (!best || g_sources[i]->prio > best->prio) best = g_sources[i];
    }
    return best;
}

/* Convert a raw counter reading to nanoseconds without overflowing: split the
 * ticks into whole seconds and the remainder, so ticks*1e9 never has to fit in
 * 64 bits (a 100 MHz HPET wraps around after ~58 years, but the product would
 * overflow in ~5.8 s of ticks). */
uint64_t time_source_ticks_to_ns(uint64_t ticks, uint64_t freq_hz)
{
    if (freq_hz == 0) return 0;
    const uint64_t secs = ticks / freq_hz;
    const uint64_t rem = ticks % freq_hz;
    return secs * 1000000000ull + (rem * 1000000000ull) / freq_hz;
}

uint64_t time_source_now_ns(void)
{
    const time_source_t *src = time_source_best();
    if (!src) return 0;
    return time_source_ticks_to_ns(src->read(), src->freq_hz);
}

uint64_t time_source_uptime_us(void)
{
    return time_source_now_ns() / 1000ull;
}

/* ---- x86_64 sources ------------------------------------------------------- */

static time_source_t ts_hpet = {
    .name = "hpet",
    .freq_hz = 0,        /* filled in by time_source_init() from the period */
    .read = ts_hpet_ticks,
    .prio = 100,
};

static const time_source_t ts_jiffies = {
    .name = "jiffies",
    .freq_hz = 100,      /* the scheduler tick programmed in kernel_arch_init */
    .read = ts_jiffies_ticks,
    .prio = 10,
};

void time_source_init(void)
{
    g_source_count = 0;

    if (hanos_hpet_ready()) {
        ts_hpet.freq_hz = hanos_hpet_frequency_hz();
        if (ts_hpet.freq_hz != 0) time_source_register(&ts_hpet);
    }
    /* The scheduler tick always exists after kernel_arch_init() programs the PIT
     * and the timer IRQ starts incrementing g_tick_count. */
    time_source_register(&ts_jiffies);

    const time_source_t *best = time_source_best();
    console_printf("[time] %d time source(s), best=%s (%llu Hz)\n",
                   (uint64_t)g_source_count,
                   best ? best->name : "none",
                   (unsigned long long)(best ? best->freq_hz : 0));
}

/* ---- self test ------------------------------------------------------------ */

void st_time_source_test(void)
{
    bool ok = true;

    ok = ok && (time_source_count() >= 2);
    const time_source_t *best = time_source_best();
    ok = ok && (best != NULL);
    ok = ok && (best->freq_hz > 0);
    ok = ok && (best->read != NULL);
    /* the high-resolution source must win over the scheduler tick */
    ok = ok && (best->prio >= 100);

    /* monotonic: two reads never go backwards, and time actually advances */
    const uint64_t t0 = time_source_now_ns();
    const uint64_t t1 = time_source_now_ns();
    ok = ok && (t1 >= t0);

    uint64_t t2 = t1;
    for (int i = 0; i < 200000 && t2 <= t1; i++) t2 = time_source_now_ns();
    ok = ok && (t2 > t1);
    ok = ok && (time_source_uptime_us() > 0);

    /* unit conversion is exact for a known pair */
    ok = ok && (time_source_ticks_to_ns(100000000ull, 100000000ull) == 1000000000ull); /* 1 s */
    ok = ok && (time_source_ticks_to_ns(1ull, 100000000ull) == 10ull);                  /* 10 ns */

    st_run("time_source", ok);
}
