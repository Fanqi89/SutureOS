/* StitchOS module: GDT/TSS/IDT gate layer (vim_ prefix)
 * Ported from: VimtuOS (C:\Users\fanqi\Desktop\自研操作系统\VimtuOS\kernel\x86_64.cpp)
 * Original license: GPL-3.0
 * Changes: C++ rewritten as freestanding C; PIC reduced to mask-all only;
 *          IDT gate targets point at MOOS stubs (moos_isr_stub_N, module B);
 *          TSS.rsp0 uses a static kernel stack; self test added.
 */
#ifndef STITCH_GDT_IDT_H
#define STITCH_GDT_IDT_H

#include <stitch/types.h>

/* Selectors - hard constraints kept from VimtuOS (SYSRET-computed user CS/SS
 * land on index 4/5, so the TSS slot is pushed to index 6). */
#define VIM_KERNEL_CS 0x08u
#define VIM_KERNEL_DS 0x10u
#define VIM_USER_DS   0x23u
#define VIM_USER_CS   0x2Bu
#define VIM_TSS_SEL   0x30u   /* index 6 << 3 */

#define VIM_IDT_ENTRIES 256u
#define VIM_IDT_GATE_SZ 16u

void vim_gdt_init(void);      /* build + lgdt + ltr (idempotent)   */
void vim_idt_init(void);      /* build 256x16B gates + lidt         */
void vim_pic_mask_all(void);  /* 8259 master/slave: mask every line */

/* boot-time self test (selftest.h st_run("vimtuos_gdtidt", ...)) */
void st_vim_gdt_idt_test(void);

#endif /* STITCH_GDT_IDT_H */
