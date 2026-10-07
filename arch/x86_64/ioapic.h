/* SutureOS module: I/O APIC driver (naos_ prefix)
 * Ported from: NAOS (C:\Users\fanqi\Desktop\自研操作系统\NAOS\arch\x86_64\ioapic.c)
 * Original license: GPL-3.0
 * Changes: symbol prefix naos_; MMIO register access; RTE format matches
 *          Intel 82093AA; no MSI/MSI-X; no ACPI 2.0+.
 */
#ifndef STITCH_IOAPIC_H
#define STITCH_IOAPIC_H

#include <stitch/types.h>

#define NAOS_IOAPIC_REG_ID       0x00
#define NAOS_IOAPIC_REG_VER      0x01
#define NAOS_IOAPIC_REG_ARB      0x02
#define NAOS_IOAPIC_REG_REDTBL   0x10

void  naos_ioapic_init(uint32_t addr);
void  naos_ioapic_set_irq(uint8_t irq, uint8_t vector, uint8_t delivery_mode);
void  naos_ioapic_mask_irq(uint8_t irq);
void  naos_ioapic_unmask_irq(uint8_t irq);
uint8_t naos_ioapic_get_rte_count(void);

#endif /* STITCH_IOAPIC_H */
