/* SutureOS module: ACPI table parsing (hanos_ prefix)
 * Ported from: HanOS (C:\Users\fanqi\Desktop\自研操作系统\HanOS\arch\x86_64\acpi.c)
 * Original license: GPL-3.0
 * Changes: symbol prefix hanos_; RSDP/RSDT/XSDT parsing; checksum validation;
 *          table iteration; no ACPI 2.0+ XSDT fallback (RSDT only for now).
 */
#ifndef STITCH_ACPI_H
#define STITCH_ACPI_H

#include <stitch/types.h>

typedef struct acpi_rsdp {
    uint8_t  signature[8];
    uint8_t  checksum;
    uint8_t  oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

typedef struct acpi_sdt_header {
    uint8_t  signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    uint8_t  oem_id[6];
    uint8_t  oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

typedef struct acpi_rsdt {
    acpi_sdt_header_t header;
    uint32_t entries[];
} acpi_rsdt_t;

acpi_rsdp_t *hanos_acpi_find_rsdp(void);
acpi_sdt_header_t *hanos_acpi_find_table(acpi_rsdp_t *rsdp, const char *signature);
bool hanos_acpi_checksum_valid(void *ptr, uint32_t length);

#endif /* STITCH_ACPI_H */
