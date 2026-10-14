/* SutureOS module: I/O APIC driver (naos_ prefix)
 * Ported from: NAOS (C:\Users\fanqi\Desktop\自研操作系统\NAOS\arch\x86_64\ioapic.c)
 * Original license: GPL-3.0
 * Changes: symbol prefix naos_; MMIO register access; RTE format matches
 *          Intel 82093AA; no MSI/MSI-X; no ACPI 2.0+.
 */
#include "ioapic.h"
#include "acpi.h"      /* acpi_rsdp_t, hanos_acpi_find_rsdp()                  */
#include "mmu.h"
/* arch/x86_64/madt.c. madt.h is deliberately NOT included: it redefines
 * acpi_madt/acpi_madt_t, so any translation unit that includes both acpi.h and
 * madt.h fails to compile. Declaring the single entry point we need keeps this
 * file buildable until those two headers are de-duplicated. */
extern uint32_t hanos_madt_get_ioapic_addr(acpi_rsdp_t *rsdp, uint8_t *ioapic_id);
#include <stitch/io.h>
#include <stitch/console.h>
#include <stitch/types.h>

static volatile uint32_t *ioapic_base = NULL;
static uint8_t rte_count = 0;

static void ioapic_write(uint32_t reg, uint32_t val) {
    if (!ioapic_base) return;
    ioapic_base[0] = reg;
    ioapic_base[4] = val;
}

static uint32_t ioapic_read(uint32_t reg) {
    if (!ioapic_base) return 0;
    ioapic_base[0] = reg;
    return ioapic_base[4];
}

void naos_ioapic_init(uint32_t addr) {
    ioapic_base = (volatile uint32_t *)addr;
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

/* ===================== SutureOS: real bring-up =====================
 * The ported version hardcoded 0xFEC00000 and then REFUSED to touch it because
 * that address is above the 1 GiB identity map ("deferring to Phase-3"), so the
 * IOAPIC was never initialised at all. Consequence: this machine boots in APIC
 * mode, the 8259's INTR is wired to IOAPIC pin 0, and with that redirection
 * entry masked (the firmware/QEMU default) NO legacy PIC interrupt is ever
 * delivered - the PIT tick never arrived (which is why the system tick had to
 * move to the per-CPU LAPIC timer) and keyboard IRQ1 was dead, which is why the
 * keyboard had to stay polled.
 *
 * Fix: take the address from the ACPI MADT when available (falling back to the
 * PIIX default), map it with the same 2 MiB identity mapping used for the LAPIC
 * and the framebuffer, and only then program the redirection table. */
#define IOAPIC_DEFAULT_ADDR 0xFEC00000u
#define IOAPIC_MMIO_SIZE    0x2000u        /* 2 MiB page granularity */

/* INT (8259 cascade) delivery mode in the RTE: 0b111 = ExtINT, which makes the
 * IOAPIC ask the 8259 for a vector through the INTA cycle. LINT0 is already
 * programmed as ExtINT by hanos_lapic_init(), so legacy IRQs reach the CPU. */
#define NAOS_IOAPIC_DELIVERY_EXTINT 7u

bool hanos_ioapic_init(void) {
    uint32_t addr = IOAPIC_DEFAULT_ADDR;
    uint8_t  id = 0;
    acpi_rsdp_t *rsdp = hanos_acpi_find_rsdp();

    if (rsdp) {
        uint32_t madt_addr = hanos_madt_get_ioapic_addr(rsdp, &id);
        if (madt_addr >= 0xF0000000u) {
            addr = madt_addr;
            console_printf("[ioapic] MADT reports IOAPIC id=%u at 0x%x\n",
                           (uint64_t)id, (uint64_t)addr);
        }
    }

    /* The IOAPIC lives far above 1 GiB: map it before the first MMIO access,
     * exactly like the LAPIC/framebuffer/AHCI ABAR. */
    if (mmu_map_mmio_identity((uint64_t)addr, IOAPIC_MMIO_SIZE) != 0) {
        console_printf("[ioapic] mmu_map_mmio_identity(0x%x) failed\n",
                       (uint64_t)addr);
        return false;
    }

    naos_ioapic_init(addr);
    console_printf("[ioapic] IOAPIC at 0x%x ready, %u redirection entries\n",
                   (uint64_t)addr, (uint64_t)rte_count);
    return rte_count != 0u;
}

/* Route the 8259 cascade (IOAPIC pin 0) as ExtINT and unmask it. Without this
 * the legacy PIC line stays invisible on an APIC-mode machine. */
void hanos_ioapic_enable_pic_extint(void) {
    if (!ioapic_base || rte_count == 0u)
        return;
    /* vector 0 (the 8259 supplies the real vector through INTA), ExtINT
     * delivery, unmasked, destination = the BSP's APIC id (0 in this kernel). */
    ioapic_write(NAOS_IOAPIC_REG_REDTBL,
                 (uint32_t)NAOS_IOAPIC_DELIVERY_EXTINT << 8);
    ioapic_write(NAOS_IOAPIC_REG_REDTBL + 1u, 0u);
}

void hanos_ioapic_set_irq(uint8_t irq, uint8_t vector, uint8_t delivery_mode) {
    naos_ioapic_set_irq(irq, vector, delivery_mode);
}

void hanos_ioapic_mask_irq(uint8_t irq) {
    naos_ioapic_mask_irq(irq);
}

void hanos_ioapic_unmask_irq(uint8_t irq) {
    naos_ioapic_unmask_irq(irq);
}