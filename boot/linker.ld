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
    /* SB16 8237 DMA buffer: must sit below 16 MiB (24-bit address bus) and be
     * 64 KiB aligned inside one 64 KiB page window. NOLOAD so it is not part of
     * the flat kernel image. */
    .sb16_dma ALIGN(64K) (NOLOAD) : {
        __sb16_dma_section_start = .;
        KEEP(*(.sb16_dma))
        __sb16_dma_section_end = .;
    }

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
