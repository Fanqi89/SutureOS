/* SutureOS (缝合怪系统) kernel linker script
 * Kernel is loaded at 1 MiB (multiboot convention).
 * Output is ELF64; boot path is Limine protocol / multiboot2 (QEMU -kernel
 * rejects ELF64 for the legacy multiboot1 path: "Cannot load x86-64 image").
 */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .text ALIGN(4K) : {
        KEEP(*(.multiboot))
        *(.text.start)
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : { *(.rodata .rodata.*) }

    .data ALIGN(4K) : { *(.data .data.*) }

    .bss ALIGN(4K) : {
        _bss_start = .;
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        _bss_end = .;
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
        *(.gnu*)
    }
}
