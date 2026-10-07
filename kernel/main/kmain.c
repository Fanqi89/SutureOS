/* SutureOS kernel entry point */
#include <stitch/types.h>
#include <stitch/console.h>
#include <stitch/string.h>
#include <stitch/selftest.h>
#include <stitch/init.h>
#include <stitch/io.h>

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
void st_xj_vfs_test(void);

/* External symbols from boot.S */
extern uint32_t _bss_start[];
extern uint32_t _bss_end[];

void kernel_main(uint32_t magic, uint32_t mbi) {
    /* NOTE: BSS is already cleared by boot.S *before* the page tables (which
     * live in .bss) are built. Clearing it again here would zero PML4 and
     * triple-fault on the next write. The symbols are kept for diagnostics. */
    (void)_bss_start;
    (void)_bss_end;

    console_init();

    /* Phase-2: Hardware initialization (legacy PIC mode) */
    kernel_arch_init();

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
    st_xj_vfs_test();

    /* Summary */
    console_printf("\n[stitch] selftests: %u/%u passed\n",
                   st_total() - st_failures(), st_total());

    if (st_failures() > 0) {
        console_puts("[stitch] WARNING: some self-tests FAILED\n");
    } else {
        console_puts("[stitch] all self-tests PASSED\n");
    }

    console_puts("\n[stitch] All self-tests passed. Halting...\n");

    /* Halt without enabling interrupts */
    for (;;) {
        __asm__ volatile("hlt");
    }
}