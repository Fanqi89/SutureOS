/* SutureOS Phase-2: Kernel hardware initialization
 * Using legacy PIC for timer (IOAPIC/LAPIC mapping deferred to Phase-3)
 */
#include <stitch/init.h>

#include <stitch/console.h>
#include <stitch/types.h>
#include <stitch/io.h>

#include "hpet.h"
#include "task.h"

volatile uint64_t g_tick_count = 0;

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

    /* Use legacy PIC for timer interrupt (vector 0x20) */
    console_puts("[init] Using legacy PIC for timer (IOAPIC/LAPIC deferred)\n");

    /* Unmask IRQ 0 (timer) on PIC master */
    outb(0x21, inb(0x21) & ~0x01);  /* Unmask IRQ 0 */

    console_puts("[init] Hardware init complete (legacy PIC mode)\n");
}

/* Called by timer ISR (vector 0x20) - just tick counter, no context switch */
void kernel_timer_tick_c(void)
{
    /* Send EOI to PIC master */
    movb $0x20, %al
    outb %al, $0x20
    ret