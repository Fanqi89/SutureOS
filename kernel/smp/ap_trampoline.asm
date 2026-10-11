; SutureOS: AP Trampoline (NASM syntax, based on VimtuOS ap_trampoline64.asm)
; Source: C:\Users\fanqi\Desktop\自研操作系统\VimtuOS\kernel\ap_trampoline64.asm
; Original license: GPL-3.0
; Assembled with: nasm -f bin ap_trampoline.asm -o ap_trampoline.bin

%define SMP64_TRAMPOLINE_PHYS   0x8000
%define SMP64_OFF_SHARED        0x800
%define SMP64_OFF_GDTR          0x40
%define SMP64_OFF_PM32          0x60
%define SMP64_OFF_LM64          0xC0
%define SMP64_OFF_GDT_COPY      0xC00
%define SMP64_RM_STACK_TOP      0x8F00

%define S_MAGIC      0x00
%define S_INDEX      0x04
%define S_APICID     0x08
%define S_VECTOR     0x0C
%define S_CR3        0x10
%define S_CR0        0x18
%define S_CR4        0x20
%define S_EFER       0x28
%define S_ENTRY      0x30
%define S_RSP        0x38
%define S_FLAGPTR    0x40
%define S_COUNTPTR   0x48
%define S_GDT        0x50
%define S_IDTR       0x90

%define P_BASE       0x8000
%define P_SBLOCK     0x8800
%define P_GDT        0x8850
%define P_GDT_COPY   0x8C00

%define SMP64_TRAMPOLINE_PHYS   0x8000
%define SMP64_OFF_SHARED        0x800
%define SMP64_OFF_GDTR          0x40
%define SMP64_OFF_PM32          0x60
%define SMP64_OFF_LM64          0xC0
%define SMP64_OFF_GDT_COPY      0xC00
%define SMP64_RM_STACK_TOP      0x8F00

%define P_BASE       0x8000
%define P_SBLOCK     0x8800
%define P_GDT        0x8850
%define P_GDT_COPY   0x8C00

%define P_MAGIC      0x8800
%define P_INDEX      0x8804
%define P_APICID     0x8808
%define P_VECTOR     0x880C
%define P_CR3        0x8810
%define P_CR0        0x8818
%define P_CR4        0x8820
%define P_EFER       0x8828
%define P_ENTRY      0x8830
%define P_RSP        0x8838
%define P_GDT        0x8850
%define P_GDT_COPY   0x8C00

[bits 16]
[org 0x8000]

smp64_trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x8F00
    cld

    ; 8259 全掩码
    mov al, 0xFF
    out 0x21, al
    out 0xA1, al

    ; A20 启用
    in al, 0x92
    or al, 0x02
    out 0x92, al

    ; 拷贝临时 GDT 到 0x8C00
    mov si, 0x8850
    mov di, 0x8C00
    mov cx, 64
    rep movsb

    lgdt [P_GDTR]

    ; 进入保护模式
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x18:_start32

; 16->32 位过渡 GDTR
    times 0x40 - ($ - $$) db 0xCC
P_GDTR:
    dw 63
    dd 0x8C00

[bits 32]
_start32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, 0x8F00

    ; CR4/CR3/EFER/CR0 从共享块加载
    mov eax, [0x8820]
    mov cr4, eax
    mov eax, [0x8810]
    mov cr3, eax

    mov ecx, 0xC0000080
    mov eax, [0x8828]
    mov edx, [0x882C]
    wrmsr

    mov eax, [0x8818]
    or eax, 0x80000001
    mov cr0, eax

    jmp 0x08:_start64

[bits 64]
_start64:
    mov rax, [0x8838]
    mov rsp, rax
    and rsp, ~0xF
    sub rsp, 8
    mov qword [rsp], 0

    mov rax, [0x8890]
    lidt [rax]

    mov rax, [0x8830]
    mov rax, [rax]
    jmp rax

; 填充到共享块前
    times 0x800 - ($ - $$) db 0xCC

; 共享数据块 (0x8800)
align 16
P_SHARED_BLOCK:
    dd 0x544D5053  ; 'SMPT' magic
    dd 0           ; index (filled by BSP)
    dd 0           ; apic_id (filled by BSP)
    dd 0x08        ; SIPI vector = 0x08
    dq 0           ; cr3 (filled by BSP)
    dq 0           ; cr0 (filled by BSP)
    dq 0           ; cr4 (filled by BSP)
    dq 0           ; efer (filled by BSP)
    dq 0           ; entry_rip (filled by BSP)
    dq 0           ; rsp (filled by BSP)
    dq 0           ; flag_ptr (filled by BSP)
    dq 0           ; count_ptr (filled by BSP)
    times 64 db 0  ; gdt[64]
    times 10 db 0  ; idtr[10]

; GDTR at offset 0x40
P_GDTR_VALUE:
    dw 63
    dd 0x8C00