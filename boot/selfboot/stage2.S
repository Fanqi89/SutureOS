/* SutureOS selfboot - BIOS stage 2 (16-bit real mode -> x86_64 long mode)
 * ============================================================
 * Loaded by boot/selfboot/stage1.S at 0x7E00. SutureOS is an x86_64-only
 * kernel, so this stage hands over in 64-bit long mode:
 *
 *   1. real mode: A20, INT 15h E801 memory size, VESA VBE 1024x768x32
 *      linear framebuffer, multiboot1 info block at 0x6000
 *   2. 32-bit protected mode: load the flat kernel image with ATA PIO
 *      (LBA28, single-sector rep insw) straight to 0x100000, clear the
 *      kernel .bss, build the same identity page tables boot/boot.S builds
 *   3. long mode: jump to boot.S's `_start64` with edi = 0x2BADB002 and
 *      esi = multiboot1 info
 *
 * Why ATA PIO and not INT 13h: the kernel image is ~1 MiB and the BIOS EDD
 * path forces 64 KiB staging + real/protected-mode ping-pong. Driving the IDE
 * controller directly reads straight into high memory in one flat loop, which
 * is what this chain needs. The drive is always the primary master (0x1F0),
 * i.e. the very disk the BIOS booted from.
 *
 * Mechanism sources (read, adapted, credited in docs/provenance.md):
 *   e1ruBSD  sys/arch/amd64/boot/stage2.S     (BSD-2-Clause, yhcmac-prog)
 *            - stage-2 chain, A20 gate, flat GDT, 32-bit entry recipe.
 *   XEKernelOS src/boot/stage2.asm            (MIT, XEKernel)
 *            - MBR -> Stage2 -> protected-mode structure.
 *   zeroAOS  versions/v0.10-float/boot.asm    (Apache-2.0, Holmes-2011)
 *            - VESA VBE mode discovery + linear framebuffer hand-off.
 *   SukiOS   boot/boot.S                      (GPL-3.0, bil-fis)
 *            - x86_64 long-mode bring-up ordering (PAE -> CR3 -> EFER -> CR0.PG).
 *   myos     boot.asm                         (BSD-2 text, doujiaozh)
 *            - A20 / GDT recipe.
 *
 * Image layout (patched by scripts/build_boot_disk.py):
 *   +0x00  jmp stage2_main ; .byte 0,0          (4 bytes)
 *   +0x04  magic      u32  'SB02'
 *   +0x08  entry64    u32  physical address of kernel _start64
 *   +0x0C  ksect      u32  kernel flat image size in 512-byte sectors
 *   +0x10  klba       u32  starting LBA of the flat kernel image
 *   +0x14  mbi        u32  multiboot info address (0x6000)
 *   +0x18  bss_start  u32  kernel __bss_start (physical)
 *   +0x1C  bss_end    u32  kernel __bss_end   (physical)
 *   +0x20  pml4       u32  kernel pml4 page address
 *   +0x24  pdpt       u32
 *   +0x28  pd0        u32
 *   +0x2C  pd_io      u32
 *
 * Memory map used here:
 *   0x5000  VbeInfoBlock (512 B)
 *   0x5400  VbeModeInfo  (256 B)
 *   0x6000  multiboot1 information block (128 B)
 *   0x7000  stage-2 stack top (also the long-mode stack)
 *   0x7C00  stage 1 (MBR)
 *   0x7E00  this stage 2 (fixed 16 KiB slot)
 *   0x100000 kernel final location (multiboot convention)
 *
 * All runtime parameters are read back from the stage-2 header by absolute
 * address (0x7E00 + offset) and everything else lives in registers, so this
 * file has no .data variables at all - the earlier draft lost time to symbol
 * addressing inside a 16/32/64-bit mixed image.
 */
.intel_syntax noprefix
.code16
.section .text
.global stage2_start

stage2_start:
    jmp     stage2_main
    .byte   0, 0
magic_sig:      .long   0x53423032      /* 'SB02' */
entry64:        .long   0
kernel_sectors: .long   0
kernel_lba:     .long   0
mbi_addr:       .long   0x6000
bss_start:      .long   0
bss_end:        .long   0
pml4_addr:      .long   0
pdpt_addr:      .long   0
pd0_addr:       .long   0
pd_io_addr:     .long   0

/* header absolute addresses (stage 2 is always loaded at 0x7E00) */
.set HDR_ENTRY64,   0x7E08
.set HDR_KSECT,     0x7E0C
.set HDR_KLBA,      0x7E10
.set HDR_BSS_START, 0x7E18
.set HDR_BSS_END,   0x7E1C
.set HDR_PML4,      0x7E20
.set HDR_PDPT,      0x7E24
.set HDR_PD0,       0x7E28
.set HDR_PD_IO,     0x7E2C

stage2_main:
    cli
    cld                             /* BIOS leaves DF unspecified; a set DF
                                     * makes rep stos/movs run backwards */
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00
    sti

    call    serial_init
    lea     si, [msg_stage2]
    call    serial_puts

    call    enable_a20
    call    e801_mem
    call    vbe_setup
    call    build_mbi

    lea     si, [msg_lm]
    call    serial_puts

    /* enter 32-bit protected mode and finish the hand-off there */
    cli
    lgdt    [gdt_ptr]
    mov     eax, cr0
    or      eax, 1
    mov     cr0, eax
    .byte   0xEA
    .word   pm32_entry
    .word   0x0018

/* ------------------------------------------------------------------ */
/* A20 gate                                                           */
/* ------------------------------------------------------------------ */
enable_a20:
    in      al, 0x92
    test    al, 2
    jnz     a20_done
    or      al, 2
    and     al, 0xFE
    out     0x92, al
a20_done:
    ret

/* ------------------------------------------------------------------ */
/* INT 15h E801: memory above 1 MiB -> multiboot mem_upper (none needed  */
/* later, kept for the MBI build)                                       */
/* ------------------------------------------------------------------ */
e801_mem:
    mov     ax, 0xE801
    int     0x15
    jc      e801_fail
    mov     [0x4800], ax            /* scratch: KiB 1..16 MiB */
    mov     [0x4802], bx            /* scratch: 64 KiB units above 16 MiB */
    ret
e801_fail:
    mov     word ptr [0x4800], 0x3C00
    mov     word ptr [0x4802], 0
    ret

/* ------------------------------------------------------------------ */
/* VESA VBE: find a 1024x768x32 linear-framebuffer mode                */
/* ------------------------------------------------------------------ */
vbe_setup:
    xor     ax, ax
    mov     es, ax
    mov     di, 0x5000
    mov     dword ptr es:[di], 0x32454256       /* 'VBE2' */
    mov     ax, 0x4F00
    int     0x10
    cmp     ax, 0x004F
    jne     vbe_fail

    mov     si, word ptr [0x500E]                /* VideoModePtr offset */
    mov     bx, word ptr [0x5010]                /* VideoModePtr segment */

vbe_loop:
    mov     ax, bx
    mov     es, ax
    mov     ax, es:[si]
    cmp     ax, 0xFFFF
    je      vbe_fail
    mov     [0x4804], ax                         /* candidate mode */

    xor     cx, cx
    mov     es, cx
    mov     di, 0x5400
    mov     cx, [0x4804]
    mov     ax, 0x4F01
    int     0x10
    cmp     ax, 0x004F
    jne     vbe_next

    /* ModeAttributes is a bit field: bit0 = supported, bit7 = linear
     * framebuffer available. Testing for equality with 0x0081 silently
     * rejected every mode (real attrs carry the other bits too). */
    mov     ax, word ptr [0x5400]
    test    ax, 0x0001
    jz      vbe_next
    test    ax, 0x0080
    jz      vbe_next
    mov     ax, word ptr [0x5412]
    cmp     ax, 1024
    jne     vbe_next
    mov     ax, word ptr [0x5414]
    cmp     ax, 768
    jne     vbe_next
    cmp     byte ptr [0x5419], 32
    jne     vbe_next
    jmp     vbe_found

vbe_next:
    add     si, 2
    jmp     vbe_loop

vbe_found:
    mov     bx, [0x4804]
    or      bx, 0x4000                  /* linear framebuffer */
    mov     ax, 0x4F02
    int     0x10
    cmp     ax, 0x004F
    jne     vbe_fail

    /* stash fb geometry for build_mbi in scratch words */
    mov     eax, dword ptr [0x5428]
    mov     [0x4808], eax               /* framebuffer physical base */
    mov     ax, word ptr [0x5410]
    mov     [0x480C], ax                /* pitch */
    mov     ax, word ptr [0x5412]
    mov     [0x480E], ax                /* width */
    mov     ax, word ptr [0x5414]
    mov     [0x4810], ax                /* height */
    mov     al, byte ptr [0x5419]
    mov     [0x4812], al                /* bpp */
    lea     si, [msg_vbe_ok]
    call    serial_puts
    ret

vbe_fail:
    mov     dword ptr [0x4808], 0
    lea     si, [msg_vbe_fail]
    call    serial_puts
    ret

/* ------------------------------------------------------------------ */
/* multiboot1 information block at 0x6000                              */
/* ------------------------------------------------------------------ */
build_mbi:
    xor     ax, ax
    mov     es, ax
    mov     di, 0x6000
    xor     eax, eax
    mov     cx, 32
    rep     stosd

    mov     dword ptr [0x6000], 0x1001  /* bit0 mem, bit12 framebuffer */
    mov     dword ptr [0x6004], 640

    movzx   eax, word ptr [0x4800]
    movzx   edx, word ptr [0x4802]
    shl     edx, 6
    add     eax, edx
    test    eax, eax
    jnz     mbi_mem_ok
    mov     eax, 65536
mbi_mem_ok:
    mov     dword ptr [0x6008], eax

    mov     eax, dword ptr [0x4808]
    test    eax, eax
    jz      mbi_no_fb
    mov     dword ptr [0x6058], eax     /* +88 framebuffer_addr low */
    mov     dword ptr [0x605C], 0       /* +92 high */
    movzx   eax, word ptr [0x480C]
    mov     dword ptr [0x6060], eax     /* +96 pitch */
    movzx   eax, word ptr [0x480E]
    mov     dword ptr [0x6064], eax     /* +100 width */
    movzx   eax, word ptr [0x4810]
    mov     dword ptr [0x6068], eax     /* +104 height */
    mov     al, [0x4812]
    mov     byte ptr [0x606C], al       /* +108 bpp */
    mov     byte ptr [0x606D], 1        /* +109 RGB */
    ret

mbi_no_fb:
    and     dword ptr [0x6000], 0xFFFFEFFF
    ret

/* ------------------------------------------------------------------ */
/* 32-bit: ATA PIO load, clear BSS, build page tables, enter long mode */
/* ------------------------------------------------------------------ */
.code32
pm32_entry:
    cld
    mov     ax, 0x20
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     esp, 0x7000

    /* ATA PIO: read ksect sectors from klba to 0x100000 */
    mov     ebx, dword ptr [HDR_KLBA]
    mov     esi, dword ptr [HDR_KSECT]
    mov     edi, 0x100000
    call    ata_read
    mov     esi, msg_loaded
1:  movzx   eax, byte ptr [esi]         /* explicit loop: the 32-bit lodsb path
                                         * did not reach the UART reliably */
    test    al, al
    jz      2f
    call    serial_putc32
    inc     esi
    jmp     1b
2:

    /* clear kernel .bss (page tables live there: clear first, build after) */
    mov     edi, dword ptr [HDR_BSS_START]
    mov     ecx, dword ptr [HDR_BSS_END]
    sub     ecx, edi
    xor     eax, eax
    rep     stosb

    /* pml4[0] = pdpt | 3 */
    mov     edi, dword ptr [HDR_PML4]
    mov     eax, dword ptr [HDR_PDPT]
    or      eax, 3
    mov     dword ptr [edi], eax
    mov     dword ptr [edi+4], 0

    /* pdpt[0] = pd0 | 3 ; pdpt[3] = pd_io | 3 (LAPIC/IOAPIC window) */
    mov     edi, dword ptr [HDR_PDPT]
    mov     eax, dword ptr [HDR_PD0]
    or      eax, 3
    mov     dword ptr [edi], eax
    mov     dword ptr [edi+4], 0
    mov     eax, dword ptr [HDR_PD_IO]
    or      eax, 3
    mov     dword ptr [edi+24], eax
    mov     dword ptr [edi+28], 0

    /* pd0[0..511] = 2 MiB pages identity-mapping the first 1 GiB */
    mov     edi, dword ptr [HDR_PD0]
    xor     ecx, ecx
    mov     eax, 0x83
pd0_loop:
    mov     dword ptr [edi], eax
    mov     dword ptr [edi+4], 0
    add     eax, 0x200000
    add     edi, 8
    inc     ecx
    cmp     ecx, 512
    jb      pd0_loop

    /* pd_io[502] = 0xFEC00000 (IOAPIC), pd_io[503] = 0xFEE00000 (LAPIC) */
    mov     edi, dword ptr [HDR_PD_IO]
    add     edi, 502*8
    mov     eax, 0xFEC00083
    mov     dword ptr [edi], eax
    mov     dword ptr [edi+4], 0
    mov     eax, 0xFEE00083
    mov     dword ptr [edi+8], eax
    mov     dword ptr [edi+12], 0

    /* PAE */
    mov     eax, cr4
    or      eax, 1 << 5
    mov     cr4, eax

    /* CR3 = pml4 */
    mov     eax, dword ptr [HDR_PML4]
    mov     cr3, eax

    /* EFER.LME = 1 */
    mov     ecx, 0xC0000080
    rdmsr
    or      eax, 1 << 8
    wrmsr

    /* CR0.PG = 1 -> long mode (compatibility) */
    mov     eax, cr0
    or      eax, 0x80000000
    mov     cr0, eax

    /* carry the kernel entry in ebx (64-bit code cannot use a 64-bit absolute
     * memory operand here) */
    mov     ebx, dword ptr [HDR_ENTRY64]

    .byte   0xEA                    /* jmp 0x08:pm64 */
    .long   pm64
    .word   0x0008

/* ATA PIO read: ebx = LBA28 start, esi = sector count, edi = destination */
ata_read:
    test    esi, esi
    jz      ata_done
ata_sector:
    mov     dx, 0x1F7                   /* wait BSY clear */
1:  in      al, dx
    test    al, 0x80
    jnz     1b

    mov     dx, 0x1F6                   /* drive/head: master, LBA */
    mov     eax, ebx
    shr     eax, 24
    and     al, 0x0F
    or      al, 0xE0
    out     dx, al

    mov     dx, 0x1F2                   /* sector count = 1 */
    mov     al, 1
    out     dx, al

    mov     dx, 0x1F3                   /* LBA 7:0 */
    mov     al, bl
    out     dx, al

    mov     dx, 0x1F4                   /* LBA 15:8 */
    mov     eax, ebx
    shr     eax, 8
    out     dx, al

    mov     dx, 0x1F5                   /* LBA 23:16 */
    mov     eax, ebx
    shr     eax, 16
    out     dx, al

    mov     dx, 0x1F7                   /* READ SECTORS */
    mov     al, 0x20
    out     dx, al

    mov     dx, 0x1F7                   /* wait DRQ */
1:  in      al, dx
    test    al, 0x08
    jz      1b

    mov     dx, 0x1F0
    mov     ecx, 256
    rep     insw

    inc     ebx
    dec     esi
    jnz     ata_sector
ata_done:
    ret

/* 32-bit serial putter: al = character (clobbers ax/dx) */
serial_putc32:
    push    ax
    mov     dx, 0x3FD
1:  in      al, dx
    test    al, 0x20
    jz      1b
    pop     ax
    out     0x3F8, al
    ret

serial_puts32:
    lodsb
    or      al, al
    jz      sp32_done
    call    serial_putc32
    jmp     serial_puts32
sp32_done:
    ret

.code64
pm64:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax
    mov     esp, 0x7000
    mov     edi, 0x2BADB002
    mov     esi, 0x6000                 /* multiboot info block */
    jmp     rbx

/* ------------------------------------------------------------------ */
/* COM1 debug output (115200 8N1)                                      */
/* ------------------------------------------------------------------ */
.code16
serial_init:
    mov     dx, 0x3F9
    xor     al, al
    out     dx, al
    mov     dx, 0x3FB
    mov     al, 0x80
    out     dx, al
    mov     dx, 0x3F8
    mov     al, 1
    out     dx, al
    mov     dx, 0x3F9
    xor     al, al
    out     dx, al
    mov     dx, 0x3FB
    mov     al, 0x03
    out     dx, al
    mov     dx, 0x3FA
    mov     al, 0xC7
    out     dx, al
    mov     dx, 0x3FC
    mov     al, 0x0B
    out     dx, al
    ret

serial_putc:
    push    dx
    push    ax
    mov     dx, 0x3FD
sd_wait:
    in      al, dx
    test    al, 0x20
    jz      sd_wait
    pop     ax
    pop     dx
    push    dx
    mov     dx, 0x3F8
    out     dx, al
    pop     dx
    ret

serial_puts:
    lodsb
    or      al, al
    jz      sp_done
    call    serial_putc
    jmp     serial_puts
sp_done:
    ret

/* ------------------------------------------------------------------ */
.section .data
.align 8
gdt:
    .quad   0
    .quad   0x00AF9A000000FFFF          /* 0x08: 64-bit code, flat */
    .quad   0x00CF92000000FFFF          /* 0x10: 64-bit data, flat */
    .quad   0x00CF9A000000FFFF          /* 0x18: 32-bit code, flat */
    .quad   0x00CF92000000FFFF          /* 0x20: 32-bit data, flat */
    .quad   0x00009A000000FFFF          /* 0x28: 16-bit code, 64 KiB */
    .quad   0x000092000000FFFF          /* 0x30: 16-bit data, 64 KiB */
gdt_ptr:
    .word   55
    .long   gdt

msg_stage2:     .asciz "[selfboot] stage2 @0x7E00 (x86_64 long mode target)\n"
msg_vbe_ok:     .asciz "[selfboot] vbe 1024x768x32 linear framebuffer ok\n"
msg_vbe_fail:   .asciz "[selfboot] vbe FAIL: no 1024x768x32 mode, serial only\n"
msg_lm:         .asciz "[selfboot] entering protected mode, loading kernel via ATA PIO\n"
msg_loaded:     .asciz "[selfboot] kernel loaded, entering long mode\n"
