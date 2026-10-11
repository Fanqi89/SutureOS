/* SutureOS kernel entry point */
#include <stitch/types.h>
#include <stitch/console.h>
#include <stitch/string.h>
#include <stitch/selftest.h>
#include <stitch/init.h>
#include <stitch/io.h>
#include "intr.h"
#include "task.h"
#include "gdt_idt.h"
#include "fbcon.h"
#include "syscall.h"

/* Module self-test declarations */
void st_vim_gdt_idt_test(void);
void st_moos_intr_test(void);
void st_mbos_kbd_test(void);
void st_mbos_pit_test(void);
void st_moe_pmm_test(void);
void st_lemis_heap_test(void);
void st_sfox_slab_test(void);
void st_phobos_queue_test(void);
void st_cp_rbtree_test(void);
void st_helos_fifo32_test(void);
void st_sched_test(void);
void st_eevdf_test(void);
void st_xj_vfs_test(void);

/* External symbols from boot.S */
extern uint32_t _bss_start[];
extern uint32_t _bss_end[];

/* Timer interrupt handler for vector 0x20 - assembly wrapper */
void timer_irq_handler_asm(void);

/* Timer tick function (C version for assembly handler) */
void kernel_timer_tick_c(void);

void kernel_main(uint32_t magic, uint32_t mbi) {
    /* NOTE: BSS is already cleared by boot.S *before* the page tables (which
     * live in .bss) are built. Clearing it again here would zero PML4 and
     * triple-fault on the next write. The symbols are kept for diagnostics. */
    (void)_bss_start;
    (void)_bss_end;

    /* Disable interrupts immediately - multiboot doesn't guarantee they're off */
    __asm__ volatile("cli");

    console_init();

    /* Initialize syscall subsystem */
    syscall_init();

    /* Phase-2: Hardware initialization */
    kernel_arch_init();

    /* Parse Limine framebuffer tag and initialize fbcon */
    limine_parse_fb(mbi);
    console_printf("[init] limine fb: addr=%p w=%u h=%u pitch=%u bpp=%u\n",
                   (void*)limine_fb_addr, limine_fb_width, limine_fb_height,
                   limine_fb_pitch, limine_fb_bpp);
    if (limine_fb_addr) {
        fbcon_init(limine_fb_addr, limine_fb_width, limine_fb_height,
                   limine_fb_pitch, limine_fb_bpp);
        console_puts("[init] fbcon initialized\n");
    } else {
        console_puts("[init] No framebuffer (BIOS mode?), using serial only\n");
    }

    /* Banner */
    console_puts("\n");
    console_puts("  ____  _   _ _                 ____  ____\n");
    console_puts(" / ___|| | | | |_ _   _ _ __ __ / ___|/ ___|\n");
    console_puts(" \\___ \\| | | | __| | | | '__/ _\\___ \\___ \\\n");
    console_puts("  ___) | |_| | |_| |_| | | | (_| ____) |__) |\n");
    console_puts(" |____/ \\___/ \\__|\\__,_|_|  \\__,_|____/____/\n");
    console_puts("\n");
    console_puts("SutureOS - a stitched-together hobby OS kernel\n");
    console_puts("Built from 15 open-source hobby OS kernels\n");
    console_puts("\n");

    /* Multiboot magic check */
    if (magic != 0x2BADB002) {
        console_puts("[boot] ERROR: bad multiboot magic\n");
        return;
    }
    console_puts("[boot] multiboot magic OK\n");

    /* Memory info */
    console_printf("[boot] mbi=0x%p\n", mbi);

    /* Set timer interrupt handler (vector 0x20) directly in IDT */
    vim_idt_set_timer_gate();
    console_puts("[init] Timer IRQ handler registered at vector 0x20 (direct IDT gate)\n");

    /* Run all module self-tests */
    console_puts("\n[selftest] running module self-tests...\n");

    st_vim_gdt_idt_test();
    st_moos_intr_test();
    st_mbos_kbd_test();
    st_mbos_pit_test();
    st_moe_pmm_test();
    st_lemis_heap_test();
    st_sfox_slab_test();
    st_phobos_queue_test();
    st_cp_rbtree_test();
    st_helos_fifo32_test();
    st_sched_test();
    st_eevdf_test();
    st_xj_vfs_test();

    /* Summary */
    console_printf("\n[stitch] selftests: %u/%u passed\n",
                   st_total() - st_failures(), st_total());

    if (st_failures() > 0) {
        console_puts("[stitch] WARNING: some self-tests FAILED\n");
    } else {
        console_puts("[stitch] all self-tests PASSED\n");
    }

    /* Set timer interrupt handler (vector 0x20) AFTER self-tests (which reinitialize IDT) */
    vim_idt_set_timer_gate();
    console_puts("[init] Timer IRQ handler registered at vector 0x20 (direct IDT gate)\n");

    console_puts("\n[stitch] Phase-1 self-tests complete. Initializing scheduler...\n");

    /* Initialize EEVDF scheduler and create idle task BEFORE enabling interrupts */
    sched_eevdf_init();
    int idle_tid = sched_create_idle();
    if (idle_tid < 0) {
        console_puts("[init] ERROR: failed to create idle task\n");
        return;
    }
    console_puts("[init] Idle task created\n");
    
    /* Initialize idle task in EEVDF */
    struct sched_task *idle_task = &sched_cpu.tasks[idle_tid];
    eevdf_init_cpu_idle(idle_task);

    console_puts("\n[stitch] Phase-1 self-tests complete. Starting EEVDF scheduler...\n");

    /* Start EEVDF scheduler - this will run tasks and only return on shutdown */
    sched_eevdf_start();

    console_puts("\n[stitch] Scheduler shutdown. Halting...\n");

    /* Halt */
    for (;;) {
        __asm__ volatile("hlt");
    }
}