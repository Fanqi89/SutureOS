/* SutureOS module: MADT (APIC) table parsing (hanos_ prefix)
 * Ported from: HanOS (C:\Users\fanqi\Desktop\自研操作系统\HanOS\arch\x86_64\madt.c)
 * Original license: GPL-3.0
 * Changes: symbol prefix hanos_; LAPIC/IOAPIC entry parsing; no ACPI 2.0+.
 */
#include "madt.h"

uint32_t hanos_madt_get_lAPIC_addr(acpi_rsdp_t *rsdp) {
    acpi_madt_t *madt = (acpi_madt_t *)hanos_acpi_find_table(rsdp, "APIC");
    if (!madt) return 0;
    return madt->lapic_addr;
}

uint32_t hanos_madt_get_ioapic_addr(acpi_rsdp_t *rsdp, uint8_t *ioapic_id) {
    acpi_madt_t *madt = (acpi_madt_t *)hanos_acpi_find_table(rsdp, "APIC");
    if (!madt) return 0;
    uint8_t *p = (uint8_t *)(madt + 1);
    uint8_t *end = (uint8_t *)madt + madt->header.length;
    while (p < end) {
        madt_entry_header_t *hdr = (madt_entry_header_t *)p;
        if (hdr->type == MADT_TYPE_IOAPIC) {
            madt_ioapic_t *io = (madt_ioapic_t *)hdr;
            if (ioapic_id) *ioapic_id = io->ioapic_id;
            return io->ioapic_addr;
        }
        p += hdr->length;
    }
    return 0;
}
