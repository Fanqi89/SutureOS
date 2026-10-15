/* SutureOS module: ACPI table parsing (hanos_ prefix)
 * Ported from: HanOS (C:\Users\fanqi\Desktop\自研操作系统\HanOS\arch\x86_64\acpi.c)
 * Original license: GPL-3.0
 * Changes: symbol prefix hanos_; RSDP/RSDT/XSDT parsing; checksum validation;
 *          table iteration; no ACPI 2.0+ XSDT fallback (RSDT only for now).
 *          Added MADT parsing for LAPIC ID extraction.
 */
#include "acpi.h"
#include "mmu.h"
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"

/* ACPI tables live wherever the firmware put them. With little RAM that is
 * inside the kernel's identity-mapped window, but with a lot of RAM (measured:
 * a 4 GB VMware VM, identical hardware at 512 MB works) the RSDT/MADT end up
 * above it, so the very first dereference page-faults and the boot appears to
 * freeze - the symptom was "no [ACPI] output at all, hang right after
 * [SMP] BSP LAPIC timer armed".
 *
 * Everything this file reads is therefore mapped on demand with 2 MiB identity
 * pages before it is touched, and every length is validated first. */
static int acpi_touch(uint64_t phys, uint64_t len)
{
    if (phys == 0u || len == 0u)
        return -1;
    if (phys + len < phys)
        return -1;                                  /* 64-bit overflow */
    return mmu_map_mmio_identity(phys, len) == 0 ? 0 : -1;
}

/* ACPI lengths are attacker/ firmware controlled: clamp before walking. */
#define ACPI_MAX_TABLE_LEN (1024u * 1024u)

/* Scan EBDA and BIOS ROM for RSDP signature "RSD PTR ". */
acpi_rsdp_t *hanos_acpi_find_rsdp(void) {
    /* Try EBDA first (0x40E << 4). The segment word is firmware data: only use
     * it when it looks like a real EBDA, and map the range before reading. */
    uint16_t ebda_seg = *(uint16_t *)0x40E;
    if (ebda_seg >= 0x800u && ebda_seg <= 0xA000u) {
        uintptr_t ebda = (uintptr_t)ebda_seg << 4;
        (void)acpi_touch(ebda, 1024u);
        for (uintptr_t p = ebda; p < ebda + 1024; p += 16) {
            if (memcmp((void *)p, "RSD PTR ", 8) == 0) return (acpi_rsdp_t *)p;
        }
    }
    /* Try BIOS ROM area. */
    (void)acpi_touch(0xE0000u, 0x20000u);
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
    uint64_t rsdt_addr = (uint64_t)rsdp->rsdt_addr;
    if (rsdt_addr == 0u) return NULL;
    /* The RSDP itself is in low memory, but the RSDT it points at may not be. */
    if (acpi_touch(rsdt_addr, sizeof(acpi_sdt_header_t)) != 0) {
        console_printf("[ACPI] RSDT at 0x%llx is unreachable (unmapped)\n",
                       (unsigned long long)rsdt_addr);
        return NULL;
    }

    acpi_rsdt_t *rsdt = (acpi_rsdt_t *)(uintptr_t)rsdt_addr;
    if (memcmp(rsdt->header.signature, "RSDT", 4) != 0) return NULL;

    uint32_t len = rsdt->header.length;
    if (len < sizeof(acpi_sdt_header_t) + 4u || len > ACPI_MAX_TABLE_LEN) {
        console_printf("[ACPI] RSDT length %u is implausible, ignored\n", (uint64_t)len);
        return NULL;
    }
    if (acpi_touch(rsdt_addr, len) != 0) return NULL;
    if (!hanos_acpi_checksum_valid(rsdt, len)) return NULL;

    uint32_t entry_count = (len - sizeof(acpi_sdt_header_t)) / 4;
    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t ent = (uint64_t)(uintptr_t)rsdt->entries[i];
        if (ent == 0u)
            continue;
        /* Each candidate table header must be mapped before it is compared. */
        if (acpi_touch(ent, sizeof(acpi_sdt_header_t)) != 0)
            continue;
        acpi_sdt_header_t *hdr = (acpi_sdt_header_t *)(uintptr_t)ent;
        if (memcmp(hdr->signature, signature, 4) == 0) {
            uint32_t tlen = hdr->length;
            if (tlen < sizeof(acpi_sdt_header_t) || tlen > ACPI_MAX_TABLE_LEN) {
                console_printf("[ACPI] %.4s at 0x%llx has implausible length %u\n",
                               signature, (unsigned long long)ent, (uint64_t)tlen);
                return NULL;
            }
            /* Map the whole table so the caller can walk all of its entries. */
            if (acpi_touch(ent, tlen) != 0)
                return NULL;
            return hdr;
        }
    }
    return NULL;
}

/* Step to the next MADT subtable, validating the header first.
 *
 * SutureOS fix (VMware, hardware version 21): the walk used to do
 * `ptr += hdr->length` unconditionally. A subtable whose length field is 0 -
 * zero padding inside the table, or simply a firmware quirk - leaves `ptr`
 * unchanged, so the loop spins forever. Measured symptom on the user's VM: the
 * boot stopped right after "[SMP] BSP LAPIC timer armed" and never printed
 * "[ACPI] Found N enabled LAPIC(s) in MADT" (QEMU's MADT has no such entry, so
 * the bug never showed up there).
 *
 * A subtable must at least carry its own 2-byte header and must fit inside the
 * table; anything else ends the walk instead of hanging. */
static uintptr_t madt_next(uintptr_t ptr, uintptr_t end, uintptr_t base, int *stopped)
{
    acpi_madt_header_t *hdr;

    if (ptr + sizeof(acpi_madt_header_t) > end || ptr + sizeof(acpi_madt_header_t) < ptr) {
        if (stopped) *stopped = 1;
        return end;
    }
    hdr = (acpi_madt_header_t *)ptr;
    if (hdr->length < sizeof(acpi_madt_header_t) || ptr + hdr->length > end) {
        if (stopped) *stopped = 1;
        console_printf("[ACPI] MADT: bad subtable at +%u (type=%u len=%u), walk stopped\n",
                       (uint64_t)(ptr - base), (uint64_t)hdr->type,
                       (uint64_t)hdr->length);
        return end;
    }
    return ptr + hdr->length;
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
    if (madt->header.length < sizeof(acpi_madt_t)) {
        console_printf("[ACPI] MADT too short (%u bytes), ignored\n",
                       (uint64_t)madt->header.length);
        return 0;
    }
    /* find_table() mapped the header page; the entries may live further on. */
    if (acpi_touch((uint64_t)(uintptr_t)madt, madt->header.length) != 0) {
        console_puts("[ACPI] MADT is unreachable (unmapped)\n");
        return 0;
    }
    uint8_t *entries = madt->entries;
    uintptr_t entries_end = (uintptr_t)madt + madt->header.length;
    int stopped = 0;

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
        ptr = madt_next(ptr, entries_end, (uintptr_t)madt, &stopped);
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
            ptr = madt_next(ptr, entries_end, (uintptr_t)madt, &stopped);
        }
    }

    console_printf("[ACPI] Found %d enabled LAPIC(s) in MADT%s\n", count,
                   stopped ? " (walk stopped early)" : "");
    return count;
}
