/* SutureOS module: Local APIC driver (lapic_ prefix)
 * Ported from: various x86_64 OS sources
 * Original license: GPL-3.0
 */
#include "lapic.h"
#include <stitch/io.h>
#include <stitch/console.h>
#include <stitch/types.h>

static volatile uint32_t *lapic_base = NULL;

/* Read LAPIC register (32-bit) */
static inline uint32_t lapic_read(uint32_t reg) {
    if (!lapic_base) return 0;
    return lapic_base[reg / 4];
}

/* Write LAPIC register (32-bit) */
static inline void lapic_write(uint32_t reg, uint32_t val) {
    if (!lapic_base) return;
    lapic_base[reg / 4] = val;
}

/* Initialize Local APIC */
bool hanos_lapic_init(void) {
    console_puts("[lapic] Starting LAPIC init...\n");
    
    /* Get LAPIC base address from MSR */
    uint32_t msr_low, msr_high;
    __asm__ volatile("rdmsr" : "=a"(msr_low), "=d"(msr_high) : "c"(MSR_IA32_APIC_BASE));
    uint64_t msr = ((uint64_t)msr_high << 32) | msr_low;
    uintptr_t addr = (uintptr_t)(msr & 0xFFFFF000);
    
    if (addr == 0) {
        /* Fallback to default address */
        addr = LAPIC_DEFAULT_ADDR;
    }
    
    console_printf("[lapic] LAPIC base address: 0x%lx\n", addr);
    
    /* boot.S identity-maps the LAPIC window (0xFEE00000 via pd_high, see
     * boot.S maps of 0xFEE00000/0xFEC00000), so a >=1GiB LAPIC base is
     * accessible. Warn only for truly unusual bases. */
    if (addr >= 0x40000000 && addr != 0xFEE00000 && addr != 0xFEC00000) {
        console_printf("[lapic] WARNING: unusual LAPIC base 0x%lx, assuming mapped\n", addr);
    }
    
    /* Assign the register window - without this every read/write is a no-op */
    lapic_base = (volatile uint32_t *)addr;
    
    console_puts("[lapic] Mapping LAPIC...\n");
    
    /* Enable LAPIC: set SVR bit 8 */
    uint32_t svr = lapic_read(LAPIC_REG_SVR);
    svr |= LAPIC_SVR_ENABLE | 0xFF;  /* Enable LAPIC, set spurious vector = 0xFF */
    lapic_write(LAPIC_REG_SVR, svr);
    console_puts("[lapic] LAPIC enabled\n");
    
    /* Mask all LVT entries initially */
    lapic_write(LAPIC_REG_LVT_TIMER, 0x10000);     /* Mask timer */
    lapic_write(LAPIC_REG_LVT_THERMAL, 0x10000);   /* Mask thermal */
    lapic_write(LAPIC_REG_LVT_PMI, 0x10000);       /* Mask PMI */
    lapic_write(LAPIC_REG_LVT_LINT0, 0x10000);     /* Mask LINT0 */
    lapic_write(LAPIC_REG_LVT_LINT1, 0x10000);     /* Mask LINT1 */
    lapic_write(LAPIC_REG_LVT_ERROR, 0x10000);     /* Mask error */
    lapic_write(LAPIC_REG_LVT_CMCI, 0x10000);      /* Mask CMCI */
    
    /* Set TPR to 0 (accept all interrupts) */
    lapic_write(LAPIC_REG_TPR, 0);
    
    console_printf("[lapic] LAPIC initialized at 0x%lx\n", addr);
    return true;
}

/* Send EOI to LAPIC */
void hanos_lapic_eoi(void) {
    if (lapic_base) {
        lapic_write(LAPIC_REG_EOI, 0);
    }
}

/* Initialize LAPIC timer */
void hanos_lapic_timer_init(uint32_t hz, bool periodic) {
    if (!lapic_base) return;
    
    /* Disable timer first */
    lapic_write(LAPIC_REG_LVT_TIMER, 0x10000);
    
    if (hz == 0) return;
    
    /* Calibrate: run timer for a known period using PIT or HPET */
    /* For now, use a reasonable default divide value */
    lapic_write(LAPIC_REG_TIMER_DIVIDE, 0x3);  /* Divide by 16 */
    
    /* Calculate initial count */
    uint32_t ticks_per_sec = 1000000;  /* Approximate - should be calibrated */
    uint32_t initial_count = ticks_per_sec / hz;
    
    lapic_write(LAPIC_REG_TIMER_INIT_COUNT, initial_count);
    
    uint32_t lvt = periodic ? LAPIC_TIMER_PERIODIC : LAPIC_TIMER_ONESHOT;
    lvt |= 0x20;  /* Vector 0x20 for timer interrupt */
    lapic_write(LAPIC_REG_LVT_TIMER, lvt);
}

/* Stop LAPIC timer */
void hanos_lapic_timer_stop(void) {
    if (lapic_base) {
        lapic_write(LAPIC_REG_LVT_TIMER, 0x10000);  /* Mask */
    }
}

/* Get local APIC ID */
uint32_t hanos_lapic_get_id(void) {
    if (!lapic_base) return 0xFF;
    uint32_t id_reg = lapic_read(LAPIC_REG_ID);
    return (id_reg >> 24) & 0xFF;
}

/* Send IPI to another processor */
void hanos_lapic_send_ipi(uint8_t dest_apic_id, uint8_t vector, uint8_t delivery_mode) {
    if (!lapic_base) return;
    
    /* Wait for ICR to be ready */
    while (lapic_read(LAPIC_REG_ICR_LOW) & (1u << 12)) {
        /* Wait for delivery status to be idle */
    }
    
    /* Write high register first (destination) */
    lapic_write(LAPIC_REG_ICR_HIGH, ((uint32_t)dest_apic_id) << 24);
    
    /* Write low register (trigger IPI) */
    uint32_t icr = vector | ((uint32_t)delivery_mode << 8) | (0x0 << 18); /* Physical destination */
    lapic_write(LAPIC_REG_ICR_LOW, icr);
}

/* Get LAPIC base address (for external use) */
volatile uint32_t *hanos_lapic_get_base(void) {
    return lapic_base;
}

/* SMP 用的直接寄存器读写 */
uint32_t hanos_lapic_read(uint32_t reg) {
    if (!lapic_base) return 0;
    return lapic_base[reg / 4];
}

void hanos_lapic_write(uint32_t reg, uint32_t val) {
    if (!lapic_base) return;
    lapic_base[reg / 4] = val;
}