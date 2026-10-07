/* StitchOS module: GDT/TSS/IDT gate layer (vim_ prefix)
 * Ported from: VimtuOS (C:\Users\fanqi\Desktop\自研操作系统\VimtuOS\kernel\x86_64.cpp)
 * Original license: GPL-3.0
 * Changes: Core rewritten from C++ to freestanding C. Kept: 8-slot GDT layout
 *          (null/kernel code/kernel data/reserved/user data/user code/TSS x2),
 *          104-byte TSS with rsp0, 256 x 16-byte IDT gates, lgdt/lidt/ltr
 *          inline asm, PIC "mask everything" final step of pic_remap().
 *          Changed: gate targets are the MOOS stub symbols moos_isr_stub_N
 *          (module B, extern-declared via VIM_ISR_LIST macro) instead of
 *          isr0_64..isr47_64/isr128_64; TSS.rsp0 points at a static 4KiB
 *          kernel stack instead of the magic constant 0x80000; the IDT
 *          diagnostic dump / PIC remap / PIT / RTC parts of the source stayed
 *          out of scope (other modules own them); ltr is guarded so a second
 *          vim_gdt_init() cannot #GP on the already-busy TSS descriptor;
 *          self test st_vim_gdt_idt_test() added (sgdt/sidt/gate/TSS checks).
 *
 * 64-bit specifics inherited from the source comments:
 *   * IDT gates are 16 bytes, offset split into low/mid/high, 64-bit base;
 *   * GDTR/IDTR bases are 64 bits;
 *   * a valid TR is mandatory in long mode once IDT interrupt gates exist.
 */
#include "gdt_idt.h"

#include <stitch/io.h>
#include <stitch/selftest.h>

/* ---- interrupt stub symbols provided by arch\x86_64\isr_stubs.S (MOOS) ---- */
#define VIM_ISR_LIST(_)  \
    _(0)   _(1)   _(2)   _(3)   _(4)   _(5)   _(6)   _(7)   \
    _(8)   _(9)   _(10)  _(11)  _(12)  _(13)  _(14)  _(15)  \
    _(16)  _(17)  _(18)  _(19)  _(20)  _(21)  _(22)  _(23)  \
    _(24)  _(25)  _(26)  _(27)  _(28)  _(29)  _(30)  _(31)  \
    _(32)  _(33)  _(34)  _(35)  _(36)  _(37)  _(38)  _(39)  \
    _(40)  _(41)  _(42)  _(43)  _(44)  _(45)  _(46)  _(47)  \
    _(128)

#define VIM_DECLARE_STUB(n) extern void moos_isr_stub_##n(void);
VIM_ISR_LIST(VIM_DECLARE_STUB)
#undef VIM_DECLARE_STUB

/* ==================== descriptor layouts (packed) ==================== */
struct vim_gdt_entry {              /* plain 8-byte segment descriptor */
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_high;
} __attribute__((packed));

struct vim_tss_entry {              /* 64-bit TSS descriptor = 16 bytes.
                                     * may_alias: it is overlaid on g_gdt[6..7]
                                     * (two 8-byte slots) like the source did */
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed, may_alias));

struct vim_gdtr { uint16_t limit; uint64_t base; } __attribute__((packed));
struct vim_idtr { uint16_t limit; uint64_t base; } __attribute__((packed));

struct vim_idt_entry {              /* 64-bit gate = 16 bytes */
    uint16_t off_low;
    uint16_t selector;
    uint8_t  ist;                   /* bits 0..2 = IST index       */
    uint8_t  type_attr;             /* P|DPL|0|gate type (0xE int) */
    uint16_t off_mid;
    uint32_t off_high;
    uint32_t zero;
} __attribute__((packed));

struct vim_tss {                    /* 104 bytes; only rsp0 is used */
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

_Static_assert(sizeof(struct vim_gdt_entry) == 8,  "gdt entry must be 8B");
_Static_assert(sizeof(struct vim_tss_entry) == 16, "tss descriptor must be 16B");
_Static_assert(sizeof(struct vim_idt_entry) == 16, "idt gate must be 16B");
_Static_assert(sizeof(struct vim_tss) == 104,      "tss must be 104B");
_Static_assert(sizeof(struct vim_gdtr) == 10 && sizeof(struct vim_idtr) == 10,
               "descriptor registers must be 10B");

/* ==================== tables ==================== */
/* GDT layout (order is a hard constraint - kept verbatim from VimtuOS):
 *   index  selector  DPL  description
 *     0      ----      0   null
 *     1     0x08      0   kernel code (64-bit, L=1)
 *     2     0x10      0   kernel data
 *     3     0x18      0   reserved (slot kept free for history)
 *     4     0x23      3   user data
 *     5     0x2B      3   user code (64-bit, L=1)
 *   6..7    0x30      0   TSS (64-bit TSS descriptor spans two slots)
 * SYSRET computes user CS = STAR[63:48]+16 = 0x2B, SS = STAR[63:48]+8 = 0x23
 * from fixed indices, so user segments must stay at 4/5 and kernel CS/DS at
 * 1/2 (0x08/0x10 are hard-wired in the stubs/frame builders); the TSS is
 * therefore parked at index 6. */
static struct vim_gdt_entry g_gdt[8];
static struct vim_gdtr      g_gdtr;
static struct vim_idt_entry g_idt[VIM_IDT_ENTRIES];
static struct vim_idtr      g_idtr;
static struct vim_tss       g_tss;

/* RSP0: static 4KiB kernel stack top (TSS rsp0 must be 16-byte aligned).
 * The source parked rsp0 at the magic constant 0x80000; a named stack is
 * auditable and survives linker layout changes. */
static uint64_t vim_kernel_stack[512] __attribute__((aligned(16)));

static bool g_gdt_loaded = false;   /* ltr may run only once (busy bit) */

typedef void (*vim_isr_fn)(void);
#define VIM_REF_STUB(n) moos_isr_stub_##n,
static const vim_isr_fn g_isr_table[49] = {
    VIM_ISR_LIST(VIM_REF_STUB)
};
#undef VIM_REF_STUB

/* ==================== GDT ==================== */
static void vim_tss_write_descriptor(void)
{
    const uint64_t base = (uint64_t)(uintptr_t)&g_tss;
    const uint32_t limit = (uint32_t)(sizeof(struct vim_tss) - 1);

    /* TSS descriptor lives at index 6 (+7 = its upper 8 bytes), selector 0x30 */
    g_gdt[6].limit_low = (uint16_t)(limit & 0xFFFFu);
    g_gdt[6].base_low  = (uint16_t)(base & 0xFFFFu);
    g_gdt[6].base_mid  = (uint8_t)((base >> 16) & 0xFFu);
    g_gdt[6].access    = 0x89;                            /* P=1 DPL=0 type 0x9 (available 64-bit TSS) */
    g_gdt[6].gran      = (uint8_t)((limit >> 16) & 0x0Fu);
    g_gdt[6].base_high = (uint8_t)((base >> 24) & 0xFFu);
    {
        struct vim_tss_entry *hi = (struct vim_tss_entry *)&g_gdt[6];
        hi->base_upper = (uint32_t)(base >> 32);
        hi->reserved   = 0;
    }
}

void vim_gdt_init(void)
{
    /* zero the TSS (rsp0 set below, iomap_base marks "no bitmap") */
    {
        uint8_t *p = (uint8_t *)&g_tss;
        for (uint32_t i = 0; i < sizeof(struct vim_tss); i++)
            p[i] = 0;
    }

    g_gdt[0] = (struct vim_gdt_entry){ 0, 0, 0, 0, 0, 0 };
    g_gdt[1] = (struct vim_gdt_entry){ 0xFFFF, 0x0000, 0x00, 0x9A, 0x20, 0x00 }; /* 0x08 kernel 64-bit code (L=1) */
    g_gdt[2] = (struct vim_gdt_entry){ 0xFFFF, 0x0000, 0x00, 0x92, 0x00, 0x00 }; /* 0x10 kernel data             */
    g_gdt[3] = (struct vim_gdt_entry){ 0, 0, 0, 0, 0, 0 };                       /* reserved                     */
    /* user segments: access = kernel access | DPL(0x60); code carries L=1 */
    g_gdt[4] = (struct vim_gdt_entry){ 0xFFFF, 0x0000, 0x00, 0xF2, 0x00, 0x00 }; /* 0x23 user data   DPL=3       */
    g_gdt[5] = (struct vim_gdt_entry){ 0xFFFF, 0x0000, 0x00, 0xFA, 0x20, 0x00 }; /* 0x2B user code   DPL=3 L=1   */

    g_tss.rsp0      = (uint64_t)(uintptr_t)&vim_kernel_stack[512];
    g_tss.iomap_base = (uint16_t)sizeof(struct vim_tss);   /* past the end -> no I/O bitmap */
    vim_tss_write_descriptor();

    g_gdtr.limit = (uint16_t)(sizeof(g_gdt) - 1u);
    g_gdtr.base  = (uint64_t)(uintptr_t)g_gdt;
    __asm__ volatile("lgdt %0" : : "m"(g_gdtr));

    if (!g_gdt_loaded) {
        /* ltr sets the descriptor's busy bit; a second ltr would #GP, so the
         * whole load happens exactly once (kernel boots with interrupts off). */
        uint16_t sel = VIM_TSS_SEL;
        __asm__ volatile("ltr %0" : : "r"(sel) : "memory");
        g_gdt_loaded = true;
    }
}

/* ==================== IDT ==================== */
static void vim_idt_set_gate(uint8_t num, uint64_t base, uint16_t sel,
                             uint8_t flags, uint8_t ist)
{
    g_idt[num].off_low   = (uint16_t)(base & 0xFFFFu);
    g_idt[num].selector  = sel;
    g_idt[num].ist       = (uint8_t)(ist & 0x07u);
    g_idt[num].type_attr = flags;
    g_idt[num].off_mid   = (uint16_t)((base >> 16) & 0xFFFFu);
    g_idt[num].off_high  = (uint32_t)((base >> 32) & 0xFFFFFFFFu);
    g_idt[num].zero      = 0;
}

void vim_idt_init(void)
{
    /* everything starts out "not installed": offset=0, P=0 -> #GP if fired */
    for (uint32_t n = 0; n < VIM_IDT_ENTRIES; n++)
        vim_idt_set_gate((uint8_t)n, 0, VIM_KERNEL_CS, 0x0E, 0);

    /* table order matches VIM_ISR_LIST: vectors 0..47, then 0x80 */
    for (uint32_t i = 0; i < 49; i++) {
        const uint8_t vec = (i < 48) ? (uint8_t)i : (uint8_t)0x80;
        /* 0x8E = P=1 DPL=0 interrupt gate; int 0x80 uses 0xEE (DPL=3) */
        const uint8_t flags = (vec == 0x80) ? 0xEEu : 0x8Eu;
        vim_idt_set_gate(vec, (uint64_t)(uintptr_t)g_isr_table[i],
                         VIM_KERNEL_CS, flags, 0);
    }

    g_idtr.limit = (uint16_t)(sizeof(g_idt) - 1u);   /* 256 * 16 - 1 = 4095 */
    g_idtr.base  = (uint64_t)(uintptr_t)g_idt;
    __asm__ volatile("lidt %0" : : "m"(g_idtr) : "memory");
}

/* ==================== PIC ==================== */
/* VimtuOS pic_remap() finishes with "mask everything, each driver unmasks on
 * demand". The kernel keeps interrupts off (cli) in this phase, so only that
 * final step is ported: no ICW sequence, no EOI, no unmask. */
void vim_pic_mask_all(void)
{
    outb(0x21, 0xFF);   /* 8259 master  IMR: all masked */
    outb(0xA1, 0xFF);   /* 8259 slave   IMR: all masked */
}

/* ==================== self test ==================== */
/* Runs without ever firing an interrupt (kernel is cli): everything is read
 * back through sgdt/sidt and direct table inspection. */
void st_vim_gdt_idt_test(void)
{
    bool ok = true;
    struct vim_gdtr gdtr_rb;
    struct vim_idtr idtr_rb;

    /* make the test self-contained: tables must be built and loaded */
    vim_gdt_init();
    vim_idt_init();

    /* 1) sgdt readback: base/limit match our GDT */
    __asm__ volatile("sgdt %0" : "=m"(gdtr_rb));
    ok = ok && (gdtr_rb.limit == (uint16_t)(sizeof(g_gdt) - 1u));
    ok = ok && (gdtr_rb.base == (uint64_t)(uintptr_t)g_gdt);

    /* 2) sidt readback: limit == 256*16-1 and base == our IDT */
    __asm__ volatile("sidt %0" : "=m"(idtr_rb));
    ok = ok && (idtr_rb.limit == (uint16_t)(VIM_IDT_ENTRIES * VIM_IDT_GATE_SZ - 1u));
    ok = ok && (idtr_rb.base == (uint64_t)(uintptr_t)g_idt);

    /* 3) gate vector 14: present=1, selector=0x08, DPL=0, offset = stub */
    {
        const struct vim_idt_entry *g = &g_idt[14];
        const uint64_t off = (uint64_t)g->off_low
                           | ((uint64_t)g->off_mid << 16)
                           | ((uint64_t)g->off_high << 32);
        ok = ok && ((g->type_attr & 0x80u) != 0);                 /* P bit      */
        ok = ok && (g->selector == VIM_KERNEL_CS);
        ok = ok && (((g->type_attr >> 5) & 0x03u) == 0);          /* DPL=0      */
        ok = ok && ((g->type_attr & 0x0Fu) == 0x0E);              /* interrupt gate */
        ok = ok && (off == (uint64_t)(uintptr_t)moos_isr_stub_14);
        /* vector 0x80 gate must be present with DPL=3 */
        ok = ok && ((g_idt[0x80].type_attr & 0x80u) != 0);
        ok = ok && (((g_idt[0x80].type_attr >> 5) & 0x03u) == 3);
        /* a vector with no stub stays not-present */
        ok = ok && ((g_idt[100].type_attr & 0x80u) == 0);
    }

    /* 4) TSS descriptor exists in the GDT at index 6 (selector 0x30) */
    {
        const uint8_t type = (uint8_t)(g_gdt[6].access & 0x0Fu);
        const struct vim_tss_entry *hi = (const struct vim_tss_entry *)&g_gdt[6];
        const uint64_t base = (uint64_t)g_gdt[6].base_low
                            | ((uint64_t)g_gdt[6].base_mid << 16)
                            | ((uint64_t)g_gdt[6].base_high << 24)
                            | ((uint64_t)hi->base_upper << 32);
        const uint32_t limit = (uint32_t)g_gdt[6].limit_low
                             | ((uint32_t)(g_gdt[6].gran & 0x0Fu) << 16);
        ok = ok && (type == 0x09u || type == 0x0Bu);  /* available or busy 64-bit TSS */
        ok = ok && (g_gdt[6].access == 0x8Bu || g_gdt[6].access == 0x89u); /* P=1 + type */
        ok = ok && (base == (uint64_t)(uintptr_t)&g_tss);
        ok = ok && (limit == (uint32_t)(sizeof(struct vim_tss) - 1u));
        ok = ok && (g_tss.iomap_base == (uint16_t)sizeof(struct vim_tss));
        ok = ok && (g_tss.rsp0 == (uint64_t)(uintptr_t)&vim_kernel_stack[512]);
    }

    st_run("vimtuos_gdtidt", ok);
}
