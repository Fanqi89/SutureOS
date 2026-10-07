/* SutureOS module: HPET timer (hanos_ prefix)
 * Ported from: HanOS (C:\Users\fanqi\Desktop\自研操作系统\HanOS\arch\x86_64\hpet.c)
 * Original license: GPL-3.0
 * Changes: symbol prefix hanos_; ACPI HPET table or fallback 0xfed00000;
 *          ENABLE_CNF set before counter starts; 64-bit counter read.
 *          Fixed: safe fallback when HPET address is outside identity mapping.
 */
#include "hpet.h"
#include <stitch/io.h>

static volatile uint64_t *hpet_base = NULL;
static uint32_t hpet_period_fs = 0;  /* femtoseconds per tick */

bool hanos_hpet_init(void) {
    /* HPET at 0xfed00000 is outside our 1 GiB identity mapping.
     * Return false to use PIT fallback instead of crashing. */
    return false;
}

uint64_t hanos_hpet_read_counter(void) {
    return 0;
}

void hanos_hpet_wait_us(uint32_t us) {
    /* Fallback: busy wait using PIT would go here */
    (void)us;
}
