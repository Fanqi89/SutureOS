/* SutureOS Phase-2: Kernel hardware initialization
 * Using legacy PIC for timer (IOAPIC/LAPIC mapping deferred to Phase-3)
 */
#include <stitch/init.h>

#include <stitch/console.h>
#include <stitch/types.h>
#include <stitch/io.h>
#include "gdt_idt.h"

#include "hpet.h"
#include "task.h"
#include "fbcon.h"

volatile uint64_t g_tick_count = 0;
extern volatile bool g_need_resched;

void kernel_arch_init(void)
{
    console_puts("\n[init] Phase-2: Hardware initialization (legacy PIC mode)\n");

    /* HPET: initialize (optional) */
    bool hpet_ok = hanos_hpet_init();
    if (hpet_ok) {
        console_puts("[init] HPET init OK\n");
    } else {
        console_puts("[init] WARNING: HPET init failed, will fall back to PIT\n");
    }

    /* Initialize legacy PIC (remap to vectors 0x20-0x2F, mask all) */
    vim_pic_init();
    console_puts("[init] Using legacy PIC for timer (IOAPIC/LAPIC deferred)\n");

    /* This kernel drives the 8259 through the CPU's LINT0, but a PC that has an
     * IOAPIC powers up in APIC mode, where the 8259's INTR is wired to IOAPIC
     * pin 0 instead. In that mode the PIT is invisible to us: IRQ0 is never
     * delivered, g_tick_count stays 0 forever and nothing time-based works (the
     * AP's own LAPIC timer kept ticking, which made this look like an SMP bug).
     * Select legacy PIC mode through the IMCR; together with LINT0 = ExtINT in
     * hanos_lapic_init() the PIT reaches the BSP again. */
    outb(0x22, 0x70);   /* IMCR: 0x70 = interrupt mode select */
    outb(0x23, 0x00);   /* 0x00 = PIC mode (8259 INTR -> LINT0) */
    console_puts("[init] IMCR: legacy PIC mode selected (8259 INTR -> LINT0)\n");

    /* Unmask IRQ 0 (timer) on PIC master */
    outb(0x21, inb(0x21) & ~0x01);  /* Unmask IRQ 0 */

    /* Program PIT channel 0 to a real 100 Hz before anything depends on the
     * system tick. Without this the PIT kept its post-reset default (~18 Hz) and
     * g_tick_count did not match the 100 Hz assumption used by the GUI/terminal
     * pacing and the uptime counters. */
    {
        extern void mbos_pit_init(uint32_t hz);
        mbos_pit_init(100);
        console_puts("[init] PIT channel 0 programmed to 100 Hz\n");
    }

    console_puts("[init] Hardware init complete (legacy PIC mode)\n");
}

/* Called by timer ISR (vector 0x20) - just set reschedule flag, send EOI */
void kernel_timer_tick_c(void)
{
    extern uint32_t smp_get_cpu_id(void);
    const uint32_t cpu = smp_get_cpu_id();

    if (cpu == 0) {
        /* BSP: the system time base and the reschedule flag live here. Keeping
         * them BSP-only means arming the APs' LAPIC timers at the same rate does
         * not double the observed tick rate (the GUI/FPS and uptime depend on
         * g_tick_count being a true 100 Hz). */
        g_tick_count++;
        g_need_resched = true;

        /* keep the BSP's own per-CPU jiffies live too, so `cpus` shows it */
        extern void smp_ap_timer_tick(uint32_t);
        smp_ap_timer_tick(0);

        /* fbcon's blinking cursor must not paint onto a screen the terminal owns. */
        {
            extern int term_available(void);
            if (!term_available())
                fbcon_timer_tick();
        }

        /* Acknowledge the interrupt. The tick is produced by this CPU's LAPIC
         * timer (see smp_init), but a legacy PIT IRQ would still need the PIC
         * EOI, so both are sent: EOIing a controller with nothing in service is
         * harmless, and a missing EOI wedges it forever. */
        extern void hanos_lapic_eoi(void);
        hanos_lapic_eoi();
        __asm__ volatile("outb %%al, %w0" : : "Nd"(0x20) : "al");
    } else {
        /* AP: its LAPIC timer fires vector 0x20 as well, so the interrupt must be
         * acknowledged at the LAPIC (not the PIC, which this CPU does not own). */
        extern void smp_ap_timer_tick(uint32_t);
        extern void hanos_lapic_eoi(void);
        smp_ap_timer_tick(cpu);
        hanos_lapic_eoi();
    }
}