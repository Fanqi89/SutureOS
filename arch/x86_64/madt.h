/* SutureOS module: MADT (APIC) table parsing (hanos_ prefix)
 * Ported from: HanOS (C:\Users\fanqi\Desktop\自研操作系统\HanOS\arch\x86_64\madt.c)
 * Original license: GPL-3.0
 * Changes: symbol prefix hanos_; LAPIC/IOAPIC entry parsing; no ACPI 2.0+.
 */
#ifndef STITCH_MADT_H
#define STITCH_MADT_H

#include <stitch/types.h>
#include "acpi.h"

typedef struct acpi_madt {
    acpi_sdt_header_t header;
    uint32_t lapic_addr;
    uint32_t flags;
} __attribute__((packed)) acpi_madt_t;

typedef struct madt_entry_header {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) madt_entry_header_t;

#define MADT_TYPE_LAPIC  0
#define MADT_TYPE_IOAPIC 1

typedef struct madt_lapic {
    madt_entry_header_t header;
    uint8_t  processor_id;
    uint8_t  apic_id;
    uint32_t flags;
} __attribute__((packed)) madt_lapic_t;

typedef struct madt_ioapic {
    madt_entry_header_t header;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;
    uint32_t gsi_base;
} __attribute__((packed)) madt_ioapic_t;

uint32_t hanos_madt_get_lAPIC_addr(acpi_rsdp_t *rsdp);
uint32_t hanos_madt_get_ioapic_addr(acpi_rsdp_t *rsdp, uint8_t *ioapic_id);

#endif /* STITCH_MADT_H */
