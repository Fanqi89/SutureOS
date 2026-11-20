/* SutureOS module: HPET timer (hanos_ prefix)
 * Ported from: HanOS (C:\Users\fanqi\Desktop\自研操作系统\HanOS\arch\x86_64\hpet.c)
 * Original license: GPL-3.0
 * Changes: symbol prefix hanos_; full implementation instead of the earlier
 *          stub: the firmware's ACPI HPET table is preferred with 0xFED00000
 *          as the fallback, the MMIO window is identity-mapped, ENABLE_CNF is
 *          set before the counter is used, the 64-bit main counter is read
 *          atomically (high-low-high retry) and the tick frequency is exposed
 *          so the LAPIC timer can be calibrated against HPET rather than the
 *          8254 (whose mode-3 counter decrements at half the input clock and
 *          made the old PIT-based calibration off by 2x).
 */
#include "hpet.h"
#include <stitch/io.h>
#include <stitch/console.h>
#include "acpi.h"
#include "mmu.h"

#define HPET_REG_CAPS     0x000u   /* 64-bit capabilities */
#define HPET_REG_CONFIG   0x010u
#define HPET_REG_INTSTAT  0x020u
#define HPET_REG_COUNTER  0x0F0u   /* 64-bit main counter */
#define HPET_CFG_ENABLE   0x0001u

#define HPET_FALLBACK_MMIO 0xFED00000ull

static volatile uint8_t *hpet_mmio = NULL;
static uint64_t hpet_period_fs = 0;
static uint64_t hpet_freq_hz = 0;

static uint32_t hpet_rd32(uint32_t off)
{
    return *(volatile uint32_t *)(hpet_mmio + off);
}

static void hpet_wr32(uint32_t off, uint32_t value)
{
    *(volatile uint32_t *)(hpet_mmio + off) = value;
}

uint64_t hanos_hpet_frequency_hz(void)
{
    return hpet_freq_hz;
}

bool hanos_hpet_ready(void)
{
    return hpet_mmio != NULL && hpet_freq_hz != 0;
}

bool hanos_hpet_init(void)
{
    uint64_t base = 0;

    /* Prefer the address the firmware published: HPET MMIO is usually
     * 0xFED00000 but the ACPI table is authoritative. */
    acpi_rsdp_t *rsdp = hanos_acpi_find_rsdp();
    if (rsdp) {
        acpi_sdt_header_t *hdr = hanos_acpi_find_table(rsdp, "HPET");
        if (hdr) {
            const acpi_hpet_t *t = (const acpi_hpet_t *)hdr;
            if (t->address_space_id == 0 && t->address != 0) {
                base = (uint64_t)t->address;
            }
        }
    }
    if (base == 0) base = HPET_FALLBACK_MMIO;

    if (mmu_map_mmio_identity(base, 0x1000) != 0) {
        console_printf("[hpet] cannot map MMIO at 0x%llx\n",
                       (unsigned long long)base);
        return false;
    }
    hpet_mmio = (volatile uint8_t *)(uintptr_t)base;

    uint32_t cap_lo = hpet_rd32(HPET_REG_CAPS);
    uint32_t cap_hi = hpet_rd32(HPET_REG_CAPS + 4);
    uint64_t period_fs = (uint64_t)cap_hi;      /* period lives in the high dword */
    if (period_fs == 0) {
        console_printf("[hpet] implausible period 0 at 0x%llx\n",
                       (unsigned long long)base);
        hpet_mmio = NULL;
        return false;
    }
    hpet_period_fs = period_fs;
    hpet_freq_hz = 1000000000000000ull / period_fs;

    /* stop, zero, then enable the main counter */
    uint32_t cfg = hpet_rd32(HPET_REG_CONFIG);
    hpet_wr32(HPET_REG_CONFIG, cfg & ~HPET_CFG_ENABLE);
    hpet_wr32(HPET_REG_COUNTER, 0);
    hpet_wr32(HPET_REG_COUNTER + 4, 0);
    hpet_wr32(HPET_REG_CONFIG, (cfg & ~HPET_CFG_ENABLE) | HPET_CFG_ENABLE);

    console_printf("[hpet] ready: mmio=0x%llx rev=%u period=%llu fs freq=%llu Hz\n",
                   (unsigned long long)base, (uint64_t)(cap_lo & 0xFFu),
                   (unsigned long long)period_fs,
                   (unsigned long long)hpet_freq_hz);
    return true;
}

uint64_t hanos_hpet_read_counter(void)
{
    if (!hpet_mmio) return 0;
    uint32_t hi, lo, hi2;
    /* A 64-bit read is two 32-bit reads: re-read the high word and retry if it
     * changed, otherwise a wrap between the reads would corrupt the value. */
    do {
        hi = hpet_rd32(HPET_REG_COUNTER + 4);
        lo = hpet_rd32(HPET_REG_COUNTER);
        hi2 = hpet_rd32(HPET_REG_COUNTER + 4);
    } while (hi != hi2);
    return ((uint64_t)hi2 << 32) | (uint64_t)lo;
}

void hanos_hpet_wait_us(uint32_t us)
{
    if (!hanos_hpet_ready()) return;
    uint64_t ticks = (uint64_t)us * hpet_freq_hz / 1000000ull;
    if (ticks == 0) ticks = 1;
    const uint64_t start = hanos_hpet_read_counter();
    while (hanos_hpet_read_counter() - start < ticks) {
        /* busy wait */
    }
}
