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
    /* LINT0 must stay UNMASKED and in ExtINT delivery mode: on this machine
     * (QEMU/i440fx, legacy PIC mode) the 8259 delivers its INTR to the CPU
     * through LINT0, so masking it silently kills every legacy IRQ on the BSP -
     * the PIT system tick and the PS/2 keyboard. That is what kept
     * g_tick_count at 0 forever while the AP's own LAPIC timer worked fine. */
    lapic_write(LAPIC_REG_LVT_LINT0, 0x700);       /* ExtINT, unmasked */
    lapic_write(LAPIC_REG_LVT_LINT1, 0x10000);     /* Mask LINT1 (NMI) */
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

/* ---- LAPIC timer calibration -------------------------------------------
 * The LAPIC timer counts at the (unknown) bus clock divided by 16, so the old
 * "assume 1 MHz" constant made the system tick run ~2.4x too fast. Measure it
 * once against the 8254 PIT: channel 0 keeps counting at 1.193182 MHz even on
 * machines where its interrupt is not routed to the CPU (which is exactly the
 * situation here), so its latched counter is a usable time reference.
 * Returns LAPIC ticks per second at the current divider. */
static uint32_t lapic_measure_ticks_per_sec(void) {
    /* read the PIT channel-0 counter (latch, then low/high) */
    #define PIT_CTRL_PORT 0x43u
    #define PIT_CH0_PORT  0x40u
    #define PIT_HZ        1193182u

    outb(PIT_CTRL_PORT, 0x00);            /* latch channel 0 */
    uint8_t lo = inb(PIT_CH0_PORT), hi = inb(PIT_CH0_PORT);
    uint32_t prev = (uint32_t)lo | ((uint32_t)hi << 8);

    /* start a long one-shot measurement run. The LVT entry must be UNMASKED for
     * the counter to actually run (a masked LAPIC timer does not decrement),
     * but interrupts are still disabled here (cli at boot / before sti), so the
     * single pending timer IRQ it raises is simply handled later by the normal
     * vector-0x20 path, which EOIs the LAPIC. */
    lapic_write(LAPIC_REG_TIMER_DIVIDE, 0x3);          /* divide by 16 */
    lapic_write(LAPIC_REG_LVT_TIMER, 0x20);            /* one-shot, unmasked  */
    lapic_write(LAPIC_REG_TIMER_INIT_COUNT, 0x00FFFFFFu);

    /* wait until the PIT has advanced ~55000 counts (~46 ms), tracking wraps */
    uint32_t pit_ticks = 0;
    while (pit_ticks < 55000u) {
        outb(PIT_CTRL_PORT, 0x00);
        lo = inb(PIT_CH0_PORT); hi = inb(PIT_CH0_PORT);
        const uint32_t now = (uint32_t)lo | ((uint32_t)hi << 8);
        pit_ticks += (now <= prev) ? (prev - now) : (prev + 65536u - now);
        prev = now;
    }

    const uint32_t remaining = lapic_read(LAPIC_REG_TIMER_CURRENT_COUNT);
    lapic_write(LAPIC_REG_TIMER_INIT_COUNT, 0);        /* stop */
    const uint32_t lapic_ticks = 0x00FFFFFFu - remaining;

    if (pit_ticks == 0 || lapic_ticks == 0) return 1000000u;   /* give up, keep old default */
    /* ticks/sec = lapic_ticks * PIT_HZ / pit_ticks */
    uint64_t rate = ((uint64_t)lapic_ticks * PIT_HZ) / pit_ticks;
    if (rate == 0 || rate > 0xFFFFFFFFull) return 1000000u;
    return (uint32_t)rate;
}

/* Initialize LAPIC timer */
void hanos_lapic_timer_init(uint32_t hz, bool periodic) {
    if (!lapic_base) return;
    
    /* Disable timer first */
    lapic_write(LAPIC_REG_LVT_TIMER, 0x10000);
    
    if (hz == 0) return;
    
    /* Measure the real LAPIC tick rate once, then reuse it for the BSP and for
     * every AP (same bus clock, same divider). */
    static uint32_t ticks_per_sec = 0;
    if (ticks_per_sec == 0) {
        ticks_per_sec = lapic_measure_ticks_per_sec();
        console_printf("[lapic] timer calibrated: %u ticks/s at divide 16\n",
                       (uint64_t)ticks_per_sec);
    }
    lapic_write(LAPIC_REG_TIMER_DIVIDE, 0x3);  /* divide by 16 */

    /* Calculate initial count */
    uint32_t initial_count = ticks_per_sec / hz;
    if (initial_count == 0) initial_count = 1;
    
    lapic_write(LAPIC_REG_TIMER_INIT_COUNT, initial_count);
    
    /* LVT timer MODE lives in bits 17:18, so the periodic selector must be
     * shifted: writing the raw value 1 here selected one-shot mode and an AP
     * LAPIC timer fired at most once (observed: AP jiffies stayed 0 forever and
     * the idle loop never woke up again). */
    uint32_t lvt = periodic ? (1u << 17) : 0u;
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