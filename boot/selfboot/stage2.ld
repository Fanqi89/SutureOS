/* SutureOS selfboot - stage 2 link script.
 * Keeps .text at 0x7E00 and lays .data immediately after it (no page padding),
 * so the whole stage-2 image fits the fixed 32-sector (16 KiB) slot that
 * stage1.S loads. .bss is emitted last and has no file content.
 */
SECTIONS
{
    . = 0x7E00;
    .text : { *(.text) *(.text.*) }
    . = ALIGN(16);
    .data : { *(.data) *(.data.*) }
    . = ALIGN(16);
    .bss  : { *(.bss) *(.bss.*) *(COMMON) }
}
