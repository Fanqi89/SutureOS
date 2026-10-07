/* Toolchain smoke test: Multiboot1 header + 32-bit entry -> serial 'A'
 * Verifies: clang integrated assembler + ld.lld ELF64 + QEMU -kernel multiboot.
 */
    .section .multiboot, "a"
    .align 4
    .long 0x1BADB002                  /* multiboot magic */
    .long 0x00000003                  /* flags: page-align + memory info */
    .long -(0x1BADB002 + 0x00000003)  /* checksum */

    .section .text
    .code32
    .global _start
_start:
    movw $0x3F8, %dx
    movb $'A', %al
    outb %al, %dx
    movb $'\n', %al
    outb %al, %dx
1:  hlt
    jmp 1b
