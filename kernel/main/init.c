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

    /* Unmask IRQ 0 (timer) on PIC master */
    outb(0x21, inb(0x21) & ~0x01);  /* Unmask IRQ 0 */

    console_puts("[init] Hardware init complete (legacy PIC mode)\n");
}

/* Called by timer ISR (vector 0x20) - just set reschedule flag, send EOI */
void kernel_timer_tick_c(void)
{
    g_tick_count++;
    g_need_resched = true;
    
    /* Update framebuffer console cursor blink */
    fbcon_timer_tick();
    
    /* Send EOI to PIC master */
    __asm__ volatile("outb %%al, %w0" : : "Nd"(0x20) : "al");
}