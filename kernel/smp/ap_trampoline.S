/* SutureOS: AP Trampoline (移植自 VimtuOS ap_trampoline64.asm)
 * Source: C:\Users\fanqi\Desktop\自研操作系统\VimtuOS\kernel\ap_trampoline64.asm
 * Original license: GPL-3.0
 * Changes: Adapted to SutureOS shared data layout and entry point
 * Note: This file is assembled as 16-bit real mode, then 32-bit protected, then 64-bit
 * Based on VimtuOS ap_trampoline64.asm which is known to work
 */
#define SMP64_TRAMPOLINE_PHYS   0x8000
#define SMP64_OFF_SHARED        0x800
#define SMP64_OFF_GDTR          0x40
#define SMP64_OFF_PM32          0x60
#define SMP64_OFF_LM64          0xC0
#define SMP64_OFF_GDT_COPY      0xC00
#define SMP64_RM_STACK_TOP      0x8F00

/* 共享块内偏移（相对 SMP64_OFF_SHARED） */
#define S_MAGIC      0x00
#define S_INDEX      0x04
#define S_APICID     0x08
#define S_VECTOR     0x0C
#define S_CR3        0x10
#define S_CR0        0x18
#define S_CR4        0x20
#define S_EFER       0x28
#define S_ENTRY      0x30
#define S_RSP        0x38
#define S_FLAGPTR    0x40
#define S_COUNTPTR   0x48
#define S_GDT        0x50
#define S_IDTR       0x90

#define P_BASE       0x8000
#define P_SBLOCK     0x8800
#define P_GDT        (P_BASE + 0x850)
#define P_GDT_COPY   0x8C00

#define SMP64_TRAMPOLINE_PHYS   0x8000
#define SMP64_OFF_SHARED        0x800
#define SMP64_OFF_GDTR          0x40
#define SMP64_OFF_PM32          0x60
#define SMP64_OFF_LM64          0xC0
#define SMP64_OFF_GDT_COPY      0xC00
#define SMP64_RM_STACK_TOP      0x8F00

#define P_BASE       0x8000
#define P_SBLOCK     (P_BASE + 0x800)
#define P_GDT        (P_BASE + 0x850)
#define P_GDT_COPY   0x8C00

#define P_MAGIC      (0x8800 + 0x00)
#define P_INDEX      (0x8800 + 0x04)
#define P_APICID     (0x8800 + 0x08)
#define P_VECTOR     (0x8800 + 0x0C)
#define P_CR3        (0x8800 + 0x10)
#define P_CR0        (0x8800 + 0x18)
#define P_CR4        (0x8800 + 0x20)
#define P_EFER       (0x8800 + 0x28)
#define P_ENTRY      (0x8800 + 0x30)
#define P_RSP        (0x8800 + 0x38)
#define P_GDT        (0x8800 + 0x50)
#define P_GDT_COPY   0x8C00

.code16
.section .text
.global smp64_trampoline_start
.type smp64_trampoline_start, @function

.global smp64_ap_entry64
.type smp64_ap_entry64, @function

/* 实模式入口 */
smp64_trampoline_start:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x8F00, %sp
    cld

    /* 8259 全掩码 */
    movb $0xFF, %al
    outb %al, $0x21
    outb %al, $0xA1

    /* A20 启用 */
    inb $0x92, %al
    orb $0x02, %al
    outb %al, $0x92

    /* 拷贝临时 GDT 到 0x8C00 */
    movw $0x8850, %si
    movw $0x8C00, %di
    movw $64, %cx
    rep movsb

    lgdt P_GDTR

    /* 进入保护模式 */
    movl %cr0, %eax
    orl $0x1, %eax
    movl %eax, %cr0
    ljmp $0x18, _start32

/* 16->32 位过渡 GDTR (offset 0x40) */
    .fill 0x40 - (. - smp64_trampoline_start), 1, 0xCC
P_GDTR:
    .word 63
    .long 0x8C00

/* 32位保护模式入口 */
.code32
_start32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movl $0x8F00, %esp

    /* CR4/CR3/EFER/CR0 从共享块加载 */
    movl 0x8820, %eax
    movl %eax, %cr4
    movl 0x8810, %eax
    movl %eax, %cr3

    movl $0xC0000080, %ecx
    movl 0x8828, %eax
    movl 0x882C, %edx
    wrmsr

    movl 0x8818, %eax
    orl $0x80000001, %eax
    movl %eax, %cr0

    ljmp $0x08, _start64

/* 64位入口 */
.code64
_start64:
    movq 0x8838, %rsp
    andq $~0xF, %rsp
    subq $8, %rsp
    movq $0, (%rsp)

    movq 0x8890, %rax
    lidt (%rax)

    movq 0x8830, %rax
    movq (%rax), %rax
    jmp *%rax

/* 填充到共享块前 */
.fill 0x800 - (. - 0x8000), 1, 0xCC

/* 共享数据块 (0x8800) */
.align 16
.global P_SHARED_BLOCK
.type P_SHARED_BLOCK, @object
P_SHARED_BLOCK:
    .long 0x544D5053  /* 'SMPT' magic */
    .long 0           /* index (filled by BSP) */
    .long 0           /* apic_id (filled by BSP) */
    .long 0x08        /* SIPI vector = 0x08 */
    .quad 0           /* cr3 (filled by BSP) */
    .quad 0           /* cr0 (filled by BSP) */
    .quad 0           /* cr4 (filled by BSP) */
    .quad 0           /* efer (filled by BSP) */
    .quad 0           /* entry_rip (filled by BSP) */
    .quad 0           /* rsp (filled by BSP) */
    .quad 0           /* flag_ptr (filled by BSP) */
    .quad 0           /* count_ptr (filled by BSP) */
    .fill 64, 1, 0    /* gdt[64] */
    .fill 10, 1, 0    /* idtr[10] */

.size smp64_trampoline_start, . - smp64_trampoline_start

.section .note.GNU-stack,"",@progbits