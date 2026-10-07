/* SutureOS module: HPET timer (hanos_ prefix)
 * Ported from: HanOS (C:\Users\fanqi\Desktop\自研操作系统\HanOS\arch\x86_64\hpet.c)
 * Original license: GPL-3.0
 * Changes: symbol prefix hanos_; ACPI HPET table or fallback 0xfed00000;
 *          ENABLE_CNF set before counter starts; 64-bit counter read.
 */
#ifndef STITCH_HPET_H
#define STITCH_HPET_H

#include <stitch/types.h>
#include "acpi.h"

typedef struct acpi_hpet {
    acpi_sdt_header_t header;
    uint8_t  hardware_rev_id;
    uint8_t  comparator_count : 5;
    uint8_t  counter_size    : 1;
    uint8_t  reserved        : 1;
    uint8_t  legacy_replacement : 1;
    uint16_t pci_vendor_id;
    uint8_t  address_space_id;
    uint8_t  register_bit_width;
    uint8_t  register_bit_offset;
    uint8_t  reserved2;
    uint32_t address;
    uint8_t  hpet_number;
    uint16_t minimum_tick;
    uint8_t  page_protection;
} __attribute__((packed)) acpi_hpet_t;

bool hanos_hpet_init(void);
uint64_t hanos_hpet_read_counter(void);
void hanos_hpet_wait_us(uint32_t us);

#endif /* STITCH_HPET_H */
