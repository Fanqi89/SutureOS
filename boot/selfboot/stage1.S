/* SutureOS selfboot - BIOS MBR stage 1
 * ============================================================
 * Purpose: the second, fully self-written boot path of SutureOS: a 512-byte
 * MBR that loads our stage 2 (LBA 1..32) to 0x7E00 and jumps to it.  The
 * Limine path in boot/boot.S is untouched; this chain exists so the boot
 * capability domain is actually ported code rather than a third-party
 * bootloader.
 *
 * Mechanism sources (read, adapted, credited in docs/provenance.md):
 *   e1ruBSD  sys/arch/amd64/boot/bootsect.S   (BSD-2-Clause, yhcmac-prog)
 *            - MBR layout, INT 13h AH=42h EDD read of the stage-2 image,
 *              far jump to 0x7E00, real-mode error printer.
 *   AntzOS   boot/mbr.asm                      (MIT, HuangPichao)
 *            - MBR -> protected-mode hand-off pattern.
 *   XEKernelOS src/boot/boot.asm               (MIT, XEKernel)
 *            - MBR -> Stage2 -> 32-bit chain shape.
 *
 * Rewritten in GAS (the upstream is NASM), single-section so the whole image
 * stays inside 512 bytes, and adapted to the SutureOS disk layout: stage 2 is
 * a fixed 16 KiB image at LBA 1 and the kernel is a flat multiboot1 image at
 * LBA 33.
 */
.intel_syntax noprefix
.code16
.section .text
.global _start

_start:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00
    sti
    mov     [boot_drive], dl

    /* INT 13h AH=42h extended read: 32 sectors (16 KiB) at LBA 1 -> 0000:7E00 */
    mov     ah, 0x42
    mov     dl, [boot_drive]
    lea     si, [dap_stage2]
    int     0x13
    jc      disk_error

    /* far jump to stage 2 (offset16:segment16 form, both < 64 KiB) */
    .byte   0xEA
    .word   0x7E00
    .word   0x0000

disk_error:
    lea     si, [msg_err]
    call    print16
    cli
    hlt
    jmp     disk_error

print16:
    lodsb
    or      al, al
    jz      1f
    mov     ah, 0x0E
    int     0x10
    jmp     print16
1:  ret

boot_drive: .byte 0
msg_err:    .asciz "SutureOS selfboot: stage2 read error\r\n"

.align 4
dap_stage2:
    .byte   0x10            /* DAP size */
    .byte   0               /* reserved */
    .word   32              /* sector count: 32 * 512 = 16 KiB stage 2 */
    .word   0x7E00          /* buffer offset */
    .word   0x0000          /* buffer segment */
    .quad   1               /* start LBA */

/* pad to 510 and add the MBR signature */
.org 510
.word 0xAA55
