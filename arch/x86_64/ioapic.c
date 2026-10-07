/* SutureOS module: I/O APIC driver (naos_ prefix)
 * Ported from: NAOS (C:\Users\fanqi\Desktop\自研操作系统\NAOS\arch\x86_64\ioapic.c)
 * Original license: GPL-3.0
 * Changes: symbol prefix naos_; MMIO register access; RTE format matches
 *          Intel 82093AA; no MSI/MSI-X; no ACPI 2.0+.
 */
#include "ioapic.h"
#include <stitch/io.h>

static volatile uint32_t *ioapic_base = NULL;
static uint8_t rte_count = 0;

static void ioapic_write(uint32_t reg, uint32_t val) {
    ioapic_base[0] = reg;
    ioapic_base[4] = val;
}

static uint32_t ioapic_read(uint32_t reg) {
    ioapic_base[0] = reg;
    return ioapic_base[4];
}

void naos_ioapic_init(uint32_t addr) {
    ioapic_base = (volatile uint32_t *)(uintptr_t)addr;
    uint32_t ver = ioapic_read(NAOS_IOAPIC_REG_VER);
    rte_count = (uint8_t)((ver >> 16) & 0xFF) + 1;
}

void naos_ioapic_set_irq(uint8_t irq, uint8_t vector, uint8_t delivery_mode) {
    if (!ioapic_base || irq >= rte_count) return;
    uint32_t rte = vector | ((uint32_t)delivery_mode << 8);
    ioapic_write(NAOS_IOAPIC_REG_REDTBL + irq * 2, rte);
    ioapic_write(NAOS_IOAPIC_REG_REDTBL + irq * 2 + 1, 0);
}

void naos_ioapic_mask_irq(uint8_t irq) {
    if (!ioapic_base || irq >= rte_count) return;
    uint32_t lo = ioapic_read(NAOS_IOAPIC_REG_REDTBL + irq * 2);
    lo |= (1u << 16);
    ioapic_write(NAOS_IOAPIC_REG_REDTBL + irq * 2, lo);
}

void naos_ioapic_unmask_irq(uint8_t irq) {
    if (!ioapic_base || irq >= rte_count) return;
    uint32_t lo = ioapic_read(NAOS_IOAPIC_REG_REDTBL + irq * 2);
    lo &= ~(1u << 16);
    ioapic_write(NAOS_IOAPIC_REG_REDTBL + irq * 2, lo);
}

uint8_t naos_ioapic_get_rte_count(void) { return rte_count; }
