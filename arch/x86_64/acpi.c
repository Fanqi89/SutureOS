/* SutureOS module: ACPI table parsing (hanos_ prefix)
 * Ported from: HanOS (C:\Users\fanqi\Desktop\自研操作系统\HanOS\arch\x86_64\acpi.c)
 * Original license: GPL-3.0
 * Changes: symbol prefix hanos_; RSDP/RSDT/XSDT parsing; checksum validation;
 *          table iteration; no ACPI 2.0+ XSDT fallback (RSDT only for now).
 *          Added MADT parsing for LAPIC ID extraction.
 */
#include "acpi.h"
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"

/* Scan EBDA and BIOS ROM for RSDP signature "RSD PTR ". */
acpi_rsdp_t *hanos_acpi_find_rsdp(void) {
    /* Try EBDA first (0x40E << 4). */
    uint16_t ebda_seg = *(uint16_t *)0x40E;
    uintptr_t ebda = (uintptr_t)ebda_seg << 4;
    for (uintptr_t p = ebda; p < ebda + 1024; p += 16) {
        if (memcmp((void *)p, "RSD PTR ", 8) == 0) return (acpi_rsdp_t *)p;
    }
    /* Try BIOS ROM area. */
    for (uintptr_t p = 0xE0000; p < 0x100000; p += 16) {
        if (memcmp((void *)p, "RSD PTR ", 8) == 0) return (acpi_rsdp_t *)p;
    }
    return NULL;
}

bool hanos_acpi_checksum_valid(void *ptr, uint32_t length) {
    uint8_t sum = 0;
    uint8_t *p = (uint8_t *)ptr;
    for (uint32_t i = 0; i < length; i++) sum += p[i];
    return sum == 0;
}

acpi_sdt_header_t *hanos_acpi_find_table(acpi_rsdp_t *rsdp, const char *signature) {
    if (!rsdp) return NULL;
    acpi_rsdt_t *rsdt = (acpi_rsdt_t *)(uintptr_t)rsdp->rsdt_addr;
    if (!rsdt) return NULL;
    if (memcmp(rsdt->header.signature, "RSDT", 4) != 0) return NULL;
    if (!hanos_acpi_checksum_valid(rsdt, rsdt->header.length)) return NULL;

    uint32_t entry_count = (rsdt->header.length - sizeof(acpi_sdt_header_t)) / 4;
    for (uint32_t i = 0; i < entry_count; i++) {
        acpi_sdt_header_t *hdr = (acpi_sdt_header_t *)(uintptr_t)rsdt->entries[i];
        if (memcmp(hdr->signature, signature, 4) == 0) return hdr;
    }
    return NULL;
}

/* Parse MADT table to extract LAPIC IDs */
int hanos_acpi_madt_parse_lapic_ids(uint8_t **out_ids, int max_ids) {
    acpi_rsdp_t *rsdp = hanos_acpi_find_rsdp();
    if (!rsdp) {
        console_puts("[ACPI] RSDP not found\n");
        return 0;
    }

    acpi_sdt_header_t *madt_hdr = hanos_acpi_find_table(rsdp, "APIC");
    if (!madt_hdr) {
        console_puts("[ACPI] MADT not found\n");
        return 0;
    }

    acpi_madt_t *madt = (acpi_madt_t *)madt_hdr;
    uint8_t *entries = madt->entries;
    uintptr_t entries_end = (uintptr_t)madt + madt->header.length;

    /* First pass: count enabled LAPICs */
    int count = 0;
    uintptr_t ptr = (uintptr_t)entries;
    while (ptr < entries_end) {
        acpi_madt_header_t *hdr = (acpi_madt_header_t *)ptr;
        
        if (hdr->type == 0 && hdr->length >= sizeof(acpi_madt_lapic_t)) {
            acpi_madt_lapic_t *lapic = (acpi_madt_lapic_t *)hdr;
            if (lapic->flags & 1) {  /* processor enabled */
                count++;
            }
        }
        ptr += hdr->length;
    }

    if (count == 0) return 0;
    if (count > max_ids) count = max_ids;

    if (out_ids && count > 0) {
        uint8_t *ids = (uint8_t*)malloc(count);
        if (!ids) {
            console_puts("[ACPI] malloc failed for LAPIC IDs\n");
            return 0;
        }
        *out_ids = ids;

        int idx = 0;
        uintptr_t ptr = (uintptr_t)entries;
        while (ptr < entries_end && idx < count) {
            acpi_madt_header_t *hdr = (acpi_madt_header_t *)ptr;
            if (hdr->type == 0 && hdr->length >= sizeof(acpi_madt_lapic_t)) {
                acpi_madt_lapic_t *lapic = (acpi_madt_lapic_t *)hdr;
                if (lapic->flags & 1) {
                    ids[idx++] = lapic->apic_id;
                }
            }
            ptr += hdr->length;
        }
    }

    console_printf("[ACPI] Found %d enabled LAPIC(s) in MADT\n", count);
    return count;
}