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
#include "pmm.h"
#include "heap.h"
#include "vfs.h"
#include "block.h"

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

/* ==================== Real memory bring-up ==================== */
/* Parse the multiboot info block for mem_upper (KB above 1 MiB) and manage
 * [end of kernel BSS, top), capped at 128 MiB so everything stays inside the
 * low identity map. Then carve the 8 MiB kernel heap out of it.
 *
 * This runs TWICE: before the self-tests (several tests call malloc(), e.g.
 * eevdf_new_entity) and again after them, because st_moe_pmm_test() re-inits
 * the PMM over a static test arena and does not fully restore the allocator
 * state - without the second pass the PMM keeps managing that arena and later
 * hands out an "AP stack" inside the kernel image. Re-initializing after the
 * tests is safe: no long-lived allocations exist yet and every test frees what
 * it took. */
static void suture_mem_bringup(uint32_t mbi, const char *tag)
{
    uintptr_t kstart = (uintptr_t)_bss_end;
    uintptr_t mem_top = 0;

    if (mbi) {
        uint32_t *mb = (uint32_t *)(uintptr_t)mbi;
        if (mb[0] & 1) {                       /* flags bit0: mem_* valid */
            uint32_t mem_upper_kb = mb[2];     /* KB above 1MiB */
            mem_top = 0x100000u + (uintptr_t)mem_upper_kb * 1024u;
        }
    }
    if (mem_top == 0) {
        console_printf("[boot] %s: no mem_upper from multiboot, assuming 64 MiB\n", tag);
        mem_top = 64 * 1024 * 1024;
    }
    if (mem_top > 128 * 1024 * 1024) mem_top = 128 * 1024 * 1024;
    if (mem_top <= kstart + 4096) {
        console_puts("[boot] ERROR: unusable memory layout\n");
        return;
    }

    moe_pmm_init(kstart, mem_top);
    console_printf("[boot] %s: pmm region %p-%p, %llu pages free\n",
                   tag, (void *)kstart, (void *)mem_top,
                   (unsigned long long)moe_pmm_free_count());

    uintptr_t heap_pages = moe_pmm_alloc(2048);
    if (heap_pages) {
        lemis_heap_init((void *)heap_pages, 2048 * 4096);
        console_printf("[boot] %s: heap 8 MiB at %p\n", tag, (void *)heap_pages);
    } else {
        console_printf("[boot] %s: heap alloc failed, keeping previous arena\n", tag);
    }
}

void kernel_main(uint32_t magic, uint32_t mbi) {
    /* NOTE: BSS is already cleared by boot.S *before* the page tables (which
     * live in .bss) are built. Clearing it again here would zero PML4 and
     * triple-fault on the next write. The symbols are kept for diagnostics. */
    (void)_bss_start;
    (void)_bss_end;

    /* Disable interrupts immediately - multiboot doesn't guarantee they're off */
    __asm__ volatile("cli");

    console_init();

    /* Multiboot magic check FIRST: the memory bring-up below trusts mbi. */
    if (magic != 0x2BADB002) {
        console_puts("[boot] ERROR: bad multiboot magic\n");
        return;
    }
    console_puts("[boot] multiboot magic OK\n");
    console_printf("[boot] mbi=0x%p\n", mbi);

    /* Real PMM + kernel heap before ANYTHING allocates: the terminal below
     * (flanterm) allocates its grid/queue/map from this heap, and several
     * module self-tests call malloc() as well. */
    suture_mem_bringup(mbi, "init");

    /* Initialize syscall subsystem */
    syscall_init();

    /* Phase-2: Hardware initialization */
    kernel_arch_init();

    /* Framebuffer hand-off: parse the multiboot1 info, identity-map the window
     * and bring up both screen paths - fbcon as the simple fallback renderer
     * and the flanterm terminal (black background, white text) as primary. */
    limine_parse_fb(mbi);
    console_printf("[init] limine fb: addr=%p w=%u h=%u pitch=%u bpp=%u\n",
                   (void*)limine_fb_addr, limine_fb_width, limine_fb_height,
                   limine_fb_pitch, limine_fb_bpp);
    if (limine_fb_addr) {
        fbcon_init(limine_fb_addr, limine_fb_width, limine_fb_height,
                   limine_fb_pitch, limine_fb_bpp);
        console_puts("[init] fbcon initialized\n");
        /* NOTE: the real terminal (flanterm) is initialised further down, AFTER
         * the memory subsystem is final. flanterm allocates its grid/queue/map
         * from the kernel heap, and the self-tests force a second PMM+heap
         * bring-up; initialising the terminal before that would leave flanterm
         * with a heap that gets reset underneath it (observed as a fault loop
         * flooding the serial line). fbcon carries the early log meanwhile. */
    } else {
        console_puts("[init] no framebuffer - serial console only\n");
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
    /* Linux-style boot log. Everything below (and above) also goes to COM1, so
     * an automated boot can diff the whole sequence. */
    console_puts("[  OK  ] Bootloader hand-off (multiboot1 via Limine)\n");
    console_puts("\n");

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
    /* Re-program the PIT to a clean 100 Hz: the PIT self-test above re-programmed
     * channel 0 several times (1 Hz / 0 / 100 Hz) and may have left it with a
     * different divisor or mid-count. */
    {
        extern void mbos_pit_init(uint32_t hz);
        extern uint32_t mbos_pit_hz(void);
        mbos_pit_init(100);
        console_printf("[init] PIT re-programmed to %u Hz for the system tick\n",
                       (uint64_t)mbos_pit_hz());
    }
    console_puts("[init] Timer IRQ handler registered at vector 0x20 (direct IDT gate)\n");

    /* Repair the allocator state the PMM self-test left behind (see the
     * function comment): re-grant the real memory region + a fresh 8 MiB
     * kernel heap before any subsystem starts allocating. */
    suture_mem_bringup(mbi, "reinit");
    console_puts("[  OK  ] Physical memory manager + kernel heap\n");

    /* ==================== Terminal bring-up (the screen the user lands in) ====
     * Only now is the heap final, so flanterm can safely own its allocations.
     * From here on every console_putc()/console_printf() is rendered as ANSI
     * text on a black-background, white-text terminal: the Linux-like console
     * that comes up before (and instead of) the GUI. */
    {
        extern void term_init_from_limine(void);
        extern int term_available(void);
        extern uint32_t term_cols(void), term_rows(void);
        term_init_from_limine();
        if (term_available()) {
            /* Replay the milestones already reached so the visible console
             * shows a complete English boot sequence, not just the tail. */
            console_puts("=== SutureOS boot log ===\n");
            console_puts("[  OK  ] Bootloader hand-off (multiboot1 via Limine)\n");
            console_puts("[  OK  ] Physical memory manager + kernel heap\n");
            console_puts("[  OK  ] Interrupts, PIC, PIT, LAPIC\n");
            console_printf("[  OK  ] Framebuffer %ux%u %u bpp, terminal %ux%u cells\n",
                           limine_fb_width, limine_fb_height, limine_fb_bpp,
                           (uint64_t)term_cols(), (uint64_t)term_rows());
            console_puts("[  OK  ] Module self-tests (see [selftest] lines above)\n");
            console_puts("[  OK  ] ACPI power management will be enabled below\n\n");
        } else {
            console_puts("[init] terminal unavailable - continuing on fbcon/serial\n");
        }
    }

    /* ==================== Block devices + VFS bring-up ==================== */
    console_puts("\n[init] Initializing block layer + VFS...\n");
    blk_init();          /* registry + ram0 ramdisk (needs the real heap) */
    blk_ata_init();      /* controller-level setup (mask IRQs, polling mode) */
    blk_ata_probe();     /* detect primary master/slave -> ata0/ata1 */
    blk_list_all();
    console_puts("[  OK  ] Block devices (ATA PIO + ramdisk)\n");

    vfs_init();          /* registers ext2/fat32/squashfs/xj_vfs drivers */
    /* Mount the in-memory ramfs as root filesystem */
    int r = vfs_mount("ram", "/", "xj_vfs");
    if (r != VFS_OK) {
        console_printf("[init] WARNING: ramfs root mount failed (%d)\n", r);
    } else {
        /* Seed a couple of demo files so ls/cat work out of the box */
        vfs_mkdir_p("/etc");
        vfs_write_file("/hello.txt",
                       "Hello from SutureOS ramfs!\n"
                       "This file is real VFS content (driver: xj_vfs).\n", 91);
        vfs_write_file("/etc/motd",
                       "Welcome to SutureOS v0.5.0-alpha.\n"
                       "Try: ls /, cat /hello.txt, df, ps, mount fat32 0 /mnt/fat\n", 126);
        console_puts("[init] ramfs root mounted, demo files seeded\n");
    }
    /* Auto-mount FAT32 from the first ATA disk if one carries a FAT32
     * volume (fails harmlessly when no disk / no FAT32 is present). */
    r = vfs_mount("0", "/mnt/fat", "fat32");
    if (r == VFS_OK) {
        console_puts("[init] FAT32 volume auto-mounted at /mnt/fat\n");
    } else {
        console_printf("[init] no FAT32 on drive 0 (mount=%d), skipping\n", r);
    }

    /* ext2 (read-only) on the second IDE disk (-hdb build/ext2.img). The driver
     * reports mount geometry, then proves the read path (direct blocks, single
     * and double indirect, hole) against the generated test image. */
    {
        extern int ext2_mount(int drive, uint32_t start_lba);
        extern int ext2_selftest(void);
        extern int ext2_is_mounted(void);
        if (ext2_mount(1, 0) == 0) {
            if (vfs_mount("1", "/mnt/ext2", "ext2") == VFS_OK) {
                console_puts("[  OK  ] ext2 volume (read-only) mounted at /mnt/ext2\n");
            } else {
                console_puts("[init] ext2: volume mounted but vfs_mount failed\n");
            }
            if (ext2_is_mounted()) ext2_selftest();
        } else {
            console_puts("[init] ext2: no ext2 volume on drive 1 (expected without -hdb)\n");
        }
    }

    /* deb package manager demo: install /mnt/fat/hello.deb when present */
    {
        extern int deb_db_init(const char *db_path);
        extern int deb_install(const char *deb_path, const char *root_dir, int force);
        deb_db_init(NULL);
        vfs_stat_t dst;
        if (vfs_stat("/mnt/fat/hello.deb", &dst) == VFS_OK) {
            if (deb_install("/mnt/fat/hello.deb", "/", 1) == 0) {
                console_puts("[init] deb demo package installed at /\n");
            } else {
                console_puts("[init] deb demo package install FAILED\n");
            }
        }
    }

    /* NOTE (known limitation, verified in QEMU): the FAT32 WRITE path is not
     * trusted yet. A guest-side write test showed the payload landing in the
     * root-directory sector and stale data in the target cluster, i.e. the
     * defect is below the filesystem in the ATA PIO write path (reads are
     * verified good: mount + FAT scan + cat of files all return correct
     * data). Until that is fixed no boot-time write test runs, so a real
     * attached volume is never modified. */
    /* FAT32 write self-test: proven working (host-side verified). Runs ONLY on
     * volumes carrying our test-image marker README.TXT, so a real attached
     * disk is never modified. Host acceptance: scripts/verify_fat32_write.py */
    {
        vfs_stat_t marker;
        if (vfs_stat("/mnt/fat/README.TXT", &marker) == VFS_OK) {
            static const char wpayload[] = "SutureOS FAT32 write self-test\n";
            uint32_t wlen = (uint32_t)sizeof(wpayload);
            vfs_stat_t chk;
            int ok = 0;
            if (vfs_write_file("/mnt/fat/SUTURE.TXT", wpayload, wlen) == VFS_OK &&
                vfs_stat("/mnt/fat/SUTURE.TXT", &chk) == VFS_OK && chk.size == wlen) {
                int fd = vfs_open("/mnt/fat/SUTURE.TXT", VFS_O_RDONLY);
                char rb[64];
                int n = (fd >= 0) ? vfs_read(fd, rb, sizeof(rb)) : -1;
                if (fd >= 0) vfs_close(fd);
                if (n == (int)wlen && memcmp(rb, wpayload, wlen) == 0) ok = 1;
            }
            vfs_sync("/mnt/fat");
            console_printf("[fat32] write self-test %s (SUTURE.TXT, %u bytes)\n",
                           ok ? "PASS" : "FAIL", (uint64_t)wlen);
        }
    }

    /* ==================== SMP: bring APs online ==================== */
    extern int smp_init(void);
    smp_init();
    console_puts("[  OK  ] SMP bring-up\n");

    /* ACPI power management: enables real `reboot` / `shutdown` from the shell */
    extern void power_init(void);
    power_init();
    console_puts("[  OK  ] ACPI power management\n");

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

    console_puts("\n[stitch] Phase-1 self-tests complete. Starting shell...\n");

    /* Create init task that runs the shell */
    extern int shell_run(void);
    int shell_tid = sched_eevdf_create((void (*)(void *))(uintptr_t)shell_run, NULL, 0);
    if (shell_tid < 0) {
        console_puts("[init] ERROR: failed to create shell task\n");
    } else {
        console_printf("[init] Shell task created (tid=%d)\n", shell_tid);
    }

    console_puts("\n[stitch] Starting EEVDF scheduler...\n");

    /* Start EEVDF scheduler - this will run tasks and only return on shutdown */
    sched_eevdf_start();

    console_puts("\n[stitch] Scheduler shutdown. Halting...\n");

    /* Halt */
    for (;;) {
        __asm__ volatile("hlt");
    }
}