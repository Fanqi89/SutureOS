/* SutureOS (缝合怪系统) - Multiboot1 bootstrap
 * ============================================================
 * Enters from Limine multiboot1 in 32-bit protected mode,
 * builds an identity map, switches to x86_64 long mode and
 * calls kernel_main(magic, multiboot_info).
 *
 * Flow adapted from:
 *   Cinux kernel/arch/x86_64/boot.S (MIT License) - the
 *   64-bit stage (stack setup / BSS clear / kernel_main call
 *   / halt loop) follows Cinux's bootstrap structure.
 * The 32-bit -> long-mode transition follows the standard
 * OSDev multiboot1 recipe.
 */

/* ---------------- Multiboot1 header ---------------- */
.section .multiboot, "a"
.align 4
.long 0x1BADB002                        /* magic */
.long 0x00000007                        /* flags: align modules | memory map | VIDEO MODE */
.long -(0x1BADB002 + 0x00000007)        /* checksum */
/* Multiboot1 header layout (spec order): the a.out kludge fields follow the
 * checksum (offsets 12..31, unused because flag bit 16 is clear) and the
 * VIDEO MODE fields come AFTER them at offsets 32/36/40/44. Putting the video
 * fields at 12..27 makes a spec-compliant loader read mode_type/width/height/
 * depth as 0, silently leaving the machine in VGA text mode - which is exactly
 * why the framebuffer used to be an 80x25 text buffer at 0xB8000. */
.long 0                                 /* header_addr  \                                */
.long 0                                 /* load_addr     | a.out kludge (unused)         */
.long 0                                 /* load_end_addr | flag bit 16 is clear          */
.long 0                                 /* bss_end_addr  |                               */
.long 0                                 /* entry_addr   /                                */
.long 0                                 /* mode_type: 0 = linear graphics framebuffer    */
.long 1024                              /* width  */
.long 768                               /* height */
.long 32                                /* depth (bpp) */

/* ---------------- Static storage (page tables, stack) ---------------- */
.section .bss
.align 4096
/* Exported for the self-written BIOS boot chain (boot/selfboot/stage2.S):
 * scripts/build_boot_disk.py patches these addresses into stage 2, which
 * builds the very same identity map before entering long mode and jumping to
 * _start64. Kept as one definition so both chains share one page-table layout. */
.global pml4
pml4:       .skip 4096
.global pdpt
pdpt:       .skip 4096
.global pd0
pd0:        .skip 4096
pd_high:    .skip 4096          /* PD for the top-2GiB alias window (PDPT[511]) */
.global pd_io
pd_io:      .skip 4096          /* PD for 0xC0000000-0xFFFFFFFF (LAPIC/IOAPIC) */
.align 16
stack_bottom:
            .skip 16384                 /* 16 KiB kernel stack */
.global stack_top
stack_top:

/* ---------------- GDT (64-bit code + data) ---------------- */
.section .rodata
.align 8
gdt64:
    .quad 0                             /* null descriptor */
    .quad (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)  /* code: L=1, P, S, X/R */
    .quad (1 << 41) | (1 << 44) | (1 << 47)              /* data: P, S, W */
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .quad gdt64
gdt64_end:

/* ---------------- Entry ---------------- */
.section .text
.code32
.global _start
.type _start, @function
_start:
    cli
    cld
    movl %eax, %ebp                     /* EBP = multiboot magic (0x2BADB002) */
    movl %ebx, %esi                     /* ESI = multiboot info struct */

    /* clear BSS first: page tables & stack live in .bss */
    movl $__bss_start, %edi
    movl $__bss_end, %ecx
    subl %edi, %ecx
    xorl %eax, %eax
    rep stosb

    /* PML4[0] -> PDPT */
    movl $pdpt, %eax
    orl  $0b11, %eax                    /* present | writable */
    movl %eax, pml4
    /* PDPT[0] -> PD for low 1 GiB */
    movl $pd0, %eax
    orl  $0b11, %eax
    movl %eax, pdpt

    /* PDPT[511] -> PD for high memory (4GB-2MB to 4GB) */
    movl $pd_high, %eax
    orl  $0b11, %eax
    movl %eax, pdpt+4088        /* PDPT[511] at offset 511*8=4088 */

    /* PD[0..511] -> 2 MiB pages, identity map first 1 GiB */
    xorl %ecx, %ecx
1:
    movl $0x200000, %eax
    mull %ecx                           /* EDX:EAX = 0x200000 * index */
    orl  $0b10000011, %eax              /* present | writable | page-size(2MB) */
    movl %eax, pd0(,%ecx,8)
    incl %ecx
    cmpl $512, %ecx
    jne 1b

    /* Map LAPIC (0xFEE00000) and IOAPIC (0xFEC00000) at their REAL virtual
     * addresses. They live in the 3rd GiB (0xC0000000-0xFFFFFFFF), so they
     * need PDPT entry index 3 -> a dedicated PD, with PDE indices
     *   502: 0xC0000000 + 502*2MiB = 0xFEC00000 (IOAPIC)
     *   503: 0xC0000000 + 503*2MiB = 0xFEE00000 (LAPIC)
     * (The previous version wrote address 0x10000011 into pd_high via
     * PDPT[511], which mapped physical 0x10000000 at VA 0x7FC0000000+ and
     * left the LAPIC unreachable - every MMIO access faulted.) */
    movl $pd_io, %eax
    orl  $0b11, %eax                    /* present | writable */
    movl %eax, pdpt+24                  /* PDPT[3] at offset 3*8 = 24 */

    movl $0xFEC00083, %eax              /* IOAPIC: present|writable|PS|0xFEC00000 */
    movl %eax, pd_io+4016               /* pd_io[502] at 502*8 */
    movl $0xFEE00083, %eax              /* LAPIC: present|writable|PS|0xFEE00000 */
    movl %eax, pd_io+4024               /* pd_io[503] at 503*8 */

    /* enable PAE */
    movl %cr4, %eax
    orl  $(1 << 5), %eax                /* CR4.PAE */
    movl %eax, %cr4

    /* load CR3 */
    movl $pml4, %eax
    movl %eax, %cr3

    /* EFER.LME = 1 */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax
    wrmsr

    /* enable paging (CR0.PG) - enters long mode (compatibility) */
    movl %cr0, %eax
    orl  $(1 << 31), %eax
    movl %eax, %cr0

    lgdt gdt64_ptr
    ljmp $0x08, $.Llong_mode             /* load 64-bit CS */

/* ---------------- 64-bit stage (structure: Cinux boot.S) ---------------- */
.code64
.Llong_mode:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movq $stack_top, %rsp               /* identity-mapped kernel stack */

    /* Move saved magic/info into args BEFORE zeroing RBP: the multiboot
     * magic is stashed in EBP, and xorq %rbp,%rbp would destroy it. */
    movl %ebp, %edi                     /* arg0 = multiboot magic (zero-extends) */
    movl %esi, %esi                     /* arg1 = multiboot info (zero-extends) */
    xorq %rbp, %rbp                     /* end of frame chain */

    call kernel_main

.halt:
    cli
    hlt
    jmp .halt

.size _start, . - _start

/* ---------------- 64-bit entry for the self-written boot chain -----------
 * Used by the second boot path (boot/selfboot/stage1.S + stage2.S, staged by
 * scripts/build_boot_disk.py) - the primary Limine path still enters through
 * _start above. By the time stage 2 jumps here the CPU is ALREADY in long
 * mode with the identity page tables built at pml4/pdpt/pd0/pd_io, .bss
 * cleared, edi = 0x2BADB002 and esi = multiboot1 info pointer.
 *
 * So this entry must NOT clear .bss again (that would zero the live page
 * tables and the kernel stack would fault) and must not rebuild them; it only
 * establishes the kernel stack and calls kernel_main with the multiboot
 * (magic, mbi) pair.
 */
.global _start64
_start64:
    cli
    cld
    movq $stack_top, %rsp
    xorq %rbp, %rbp
    call kernel_main
.halt64:
    cli
    hlt
    jmp .halt64
.size _start64, . - _start64

.section .note.GNU-stack,"",@progbits
