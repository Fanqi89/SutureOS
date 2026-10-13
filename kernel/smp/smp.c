/* SutureOS: SMP 多核支持 —— 真实 AP 启动 (INIT-SIPI-SIPI + 0x8000 跳板)
 * 移植自 VimtuOS kernel/smp64.cpp（GPL-3.0，本仓库部分模块的原始来源），
 * 适配 kernel/smp/ap_trampoline.asm 的共享块协议（物理 0x8800 共享块）。
 *
 * 流程：
 *   1) BSP: hanos_lapic_init -> 读 MADT -> 过滤出各 AP 的 LAPIC ID；
 *   2) 准备：把跳板 blob 拷到物理 0x8000（boot.S 已恒等映射前 1GiB，
 *      拷之前还会用当前 CR3 走一遍页表确认 present+writable），给每个 AP
 *      分配 16KB 连续栈，填共享块（CR3/CR0/CR4/EFER/入口指针/栈顶/临时
 *      GDT/IDTR 描述符）；
 *   3) 自检（发 SIPI 之前）：共享块/临时 GDT/入口二级间接/栈自洽性；
 *      不通过则一个 SIPI 都不发；
 *   4) 每个 AP: INIT assert(0x4500) -> 10ms -> INIT deassert(0x8500)
 *      -> 200us -> SIPI(vector=8) -> 200us -> SIPI -> 有界等在线(500ms)；
 *   5) AP 侧 (smp_ap_entry64)：启用本核 LAPIC -> 填 cpu_locals[index] ->
 *      打 "[SMP] AP %u online (lapic=%u)" -> lock xadd 在线计数 ->
 *      置 g_ap_online[index] -> sti;hlt 空转（AP 不接调度器，范围外）。
 *
 * 依赖/注意：
 *   - 跳板 blob 由 build.py (nasm -f bin + objcopy) 链进内核 .text。当前
 *     build.py 的 --redefine-sym 因临时文件名匹配不到（真实符号名每次
 *     构建都带随机后缀），因此这里用 weak 外部符号优先取链接进来的 blob，
 *     拿不到就退回本文件内嵌的副本（由 build/ap_trampoline.bin 生成）。
 *   - AP 不参与调度：没有 per-CPU runqueue；AP 只在线 + 空转。
 *   - smp_send_ipi()/smp_send_ipi_all_others() 会向 AP 发 0xE0/0xE1 向量，
 *     调度器接 SMP 之前必须先在 IDT 注册对应 handler（gdt_idt.c 目前只
 *     装了向量 0-47 + 0x80，0xE0/0xE1 是 P=0 空门，收到会 #NP）。
 */
#include "smp.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"
#include <stitch/io.h>
#include "acpi.h"
#include "lapic.h"
#include "ioapic.h"
#include "gdt_idt.h"
#include "mmu.h"
#include "pmm.h"
#include "../sched/eevdf.h"
#include "../sched/task.h"

/* 外部符号 */
extern volatile bool g_need_resched;
extern sched_cpu_t sched_cpu;
extern struct eevdf_t *eevdf_get_global(void);
extern bool hanos_lapic_init(void);
extern uint32_t hanos_lapic_read(uint32_t reg);
extern void hanos_lapic_write(uint32_t reg, uint32_t val);
extern void hanos_lapic_eoi(void);
extern int hanos_acpi_madt_parse_lapic_ids(uint8_t **out_ids, int max_ids);

/* ==================== 跳板 blob ====================
 * 优先用 build.py 链进 .text 的 blob（nasm -f bin + objcopy -I binary）。
 * 注意 build.py 目前把 bin 先拷到随机名临时文件再 objcopy，--redefine-sym
 * 的旧名匹配不到，真实符号名是 _binary_<随机临时路径>_bin_start —— 每次
 * 构建都不同，无法直接引用。所以两个符号声明成 weak：build.py 修好命名
 * 后自动改用链接版；修好之前 weak 未定义解析为 0，退回下面的内嵌副本
 * （由 build/ap_trampoline.bin 生成，与 kernel/smp/ap_trampoline.asm
 * 逐字节一致，共 2208 = 0x8A0 字节，含 0x800 偏移处的共享块模板）。 */
extern const uint8_t ap_trampoline_bin_start[] __attribute__((weak));
extern const uint8_t ap_trampoline_bin_end[]   __attribute__((weak));

static const uint8_t s_trampoline_blob[] = {
    0xFA, 0x31, 0xC0, 0x8E, 0xD8, 0x8E, 0xC0, 0x8E, 0xD0, 0xBC, 0x00, 0x8F, 0xFC, 0xB0, 0xFF, 0xE6,
    0x21, 0xE6, 0xA1, 0xE4, 0x92, 0x0C, 0x02, 0xE6, 0x92, 0xBE, 0x50, 0x88, 0xBF, 0x00, 0x8C, 0xB9,
    0x40, 0x00, 0xF3, 0xA4, 0x0F, 0x01, 0x16, 0x40, 0x80, 0x0F, 0x20, 0xC0, 0x66, 0x83, 0xC8, 0x01,
    0x0F, 0x22, 0xC0, 0xEA, 0x46, 0x80, 0x18, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0x3F, 0x00, 0x00, 0x8C, 0x00, 0x00, 0x66, 0xB8, 0x10, 0x00, 0x8E, 0xD8, 0x8E, 0xC0, 0x8E, 0xD0,
    0x8E, 0xE0, 0x8E, 0xE8, 0xBC, 0x00, 0x8F, 0x00, 0x00, 0xA1, 0x20, 0x88, 0x00, 0x00, 0x0F, 0x22,
    0xE0, 0xA1, 0x10, 0x88, 0x00, 0x00, 0x0F, 0x22, 0xD8, 0xB9, 0x80, 0x00, 0x00, 0xC0, 0xA1, 0x28,
    0x88, 0x00, 0x00, 0x8B, 0x15, 0x2C, 0x88, 0x00, 0x00, 0x0F, 0x30, 0xA1, 0x18, 0x88, 0x00, 0x00,
    0x0D, 0x01, 0x00, 0x00, 0x80, 0x0F, 0x22, 0xC0, 0xEA, 0x8F, 0x80, 0x00, 0x00, 0x08, 0x00, 0x48,
    0x8B, 0x04, 0x25, 0x38, 0x88, 0x00, 0x00, 0x48, 0x89, 0xC4, 0x48, 0x83, 0xE4, 0xF0, 0x48, 0x83,
    0xEC, 0x08, 0x48, 0xC7, 0x04, 0x24, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x04, 0x25, 0x90, 0x88,
    0x00, 0x00, 0x0F, 0x01, 0x18, 0x48, 0x8B, 0x04, 0x25, 0x30, 0x88, 0x00, 0x00, 0x48, 0x8B, 0x00,
    0xFF, 0xE0,
    /* 到 offset 0xC1 为止是真实代码（jmp rax 收尾）；0xC2..0x7FF 的 int3
     * 填充和 0x800..0x89F 的共享块模板由 smp_finalize_blob() 运行时补全。 */
};
static uint8_t s_blob_storage[0x8A0];

/* 把"代码段 + int3 填充 + 共享块模板"拼成完整 blob 到 s_blob_storage。 */
static void smp_finalize_blob(void) {
    static bool built = false;
    if (built) return;
    for (uint32_t i = 0; i < sizeof(s_blob_storage); i++)
        s_blob_storage[i] = (i < sizeof(s_trampoline_blob)) ? s_trampoline_blob[i] : 0xCC;
    /* 共享块模板（对应 ap_trampoline.asm P_SHARED_BLOCK，物理 0x8800）*/
    uint8_t *sb = s_blob_storage + 0x800;
    for (uint32_t i = 0; i < 0x9A; i++) sb[i] = 0;
    sb[0] = 0x53; sb[1] = 0x50; sb[2] = 0x4D; sb[3] = 0x54;   /* 'SMPT' */
    sb[0x0C] = SMP_SIPI_VECTOR;                               /* 向量 8 */
    /* 尾部 GDTR 模板（limit 63, base 0x8C00；跳板实际用的是 0x40 处那份，
     * 这里保持与 build/ap_trampoline.bin 逐字节一致） */
    sb[0x9A] = 0x3F; sb[0x9B] = 0x00;
    sb[0x9C] = 0x00; sb[0x9D] = 0x8C; sb[0x9E] = 0x00; sb[0x9F] = 0x00;
    built = true;
}

/* ==================== LAPIC / ICR 常量 ==================== */
#define SMP_ICR_DELIVERY_BUSY (1u << 12)   /* ICR_LOW bit12: 投递中 */
#define SMP_ICR_INIT_ASSERT   0x00004500u  /* delivery=INIT(101) | level=assert */
#define SMP_ICR_INIT_DEASSERT 0x00008500u  /* delivery=INIT | trigger=level | deassert */
#define SMP_ICR_SIPI          0x00000600u  /* delivery=Start-Up(110)，发送时 | 向量 */

/* 有界延时：QEMU 下 1 次 pause 约 1ns 量级，1000 次≈1us，10ms≈10^7 次
 * （宽松标定，宁长勿短）。绝不依赖 tick/中断：smp_init 时 IF=0。 */
#define SMP_SPIN_PER_US     1000ull
#define SMP_ICR_IDLE_SPINS  2000000u

/* ==================== 全局状态 ==================== */
struct cpu_local cpu_locals[SMP_MAX_CPUS];
struct ap_state g_aps[SMP_MAX_CPUS + 1];
volatile uint8_t g_ap_online[SMP_MAX_CPUS + 1] = {0};
volatile uint64_t g_ap_online_count = 1;   /* BSP 恒为 1；AP 用 lock xadd 递增 */
uint32_t g_bsp_lapic_id = 0;

static uint32_t g_ap_ids[SMP_MAX_CPUS];    /* 待启动 AP 的 LAPIC ID（不含 BSP） */
static int g_ap_n = 0;                     /* 待启动 AP 个数 */
static uint64_t s_ap_entry_var;            /* 跳板 [[entry_rip]] 指向的入口地址变量 */

/* 10 字节 IDTR 描述符（sidt 写入；跳板 lidt [rax] 从它加载） */
struct smp_idtr10 { uint16_t limit; uint64_t base; } __attribute__((packed));
_Static_assert(sizeof(struct smp_idtr10) == 10, "idtr descriptor must be 10 bytes");
static struct smp_idtr10 s_ap_idtr;

/* LAPIC 访问 */
static inline uint32_t lapic_read_reg(uint32_t reg) {
    return hanos_lapic_read(reg);
}
static inline void lapic_write_reg(uint32_t reg, uint32_t value) {
    hanos_lapic_write(reg, value);
}

/* ==================== 底层：CR/MSR/sidt ==================== */
static inline uint64_t smp_read_cr0(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}
static inline uint64_t smp_read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline uint64_t smp_read_cr4(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}
static inline uint64_t smp_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static void smp_sidt(void *dst10) {
    __asm__ volatile("sidt %0" : "=m"(*(uint8_t (*)[10])dst10));
}

/* ==================== 只读页表走查（恒等映射自证） ====================
 * 走当前 CR3 的 4 级页表，确认某物理页 present(+writable)。boot.S 把前
 * 1GiB 恒等映射（boot.S:64-87，2MiB 大页，P|W|PS），内核链接在 1MiB
 * （boot/linker.ld:12，非高半区），这里再在运行时自证一遍：宁可跳过 AP
 * 启动也不 #PF。移植自 VimtuOS smp64_pa_mapped64。 */
static bool smp_pa_page_mapped(uint64_t pa, bool need_writable) {
    uint64_t cr3 = smp_read_cr3() & 0x000FFFFFFFFFF000ull;
    if (cr3 == 0) return false;
    uint64_t e = ((const volatile uint64_t *)(uintptr_t)cr3)[(pa >> 39) & 0x1FF];
    if (!(e & 1ull)) return false;
    e = ((const volatile uint64_t *)(uintptr_t)(e & 0x000FFFFFFFFFF000ull))[(pa >> 30) & 0x1FF];
    if (!(e & 1ull)) return false;
    if (e & 0x80ull) return !need_writable || (e & 2ull) != 0;      /* 1GB 大页 */
    e = ((const volatile uint64_t *)(uintptr_t)(e & 0x000FFFFFFFFFF000ull))[(pa >> 21) & 0x1FF];
    if (!(e & 1ull)) return false;
    if (e & 0x80ull) return !need_writable || (e & 2ull) != 0;      /* 2MB 大页 */
    e = ((const volatile uint64_t *)(uintptr_t)(e & 0x000FFFFFFFFFF000ull))[(pa >> 12) & 0x1FF];
    if (!(e & 1ull)) return false;
    return !need_writable || (e & 2ull) != 0;
}
static bool smp_pa_range_ok(uint64_t pa, uint64_t bytes, bool need_writable) {
    if (bytes == 0) return false;
    for (uint64_t p = pa & ~0xFFFull; p < pa + bytes; p += 0x1000) {
        if (!smp_pa_page_mapped(p, need_writable)) return false;
    }
    return true;
}

/* ==================== 有界 ICR / 延时 / 等待 ==================== */
static bool smp_icr_idle(void) {
    for (uint32_t i = 0; i < SMP_ICR_IDLE_SPINS; i++) {
        if (!(lapic_read_reg(LAPIC_REG_ICR_LOW) & SMP_ICR_DELIVERY_BUSY)) return true;
        __asm__ volatile("pause");
    }
    return false;
}
static void smp_send_ipi_raw(uint32_t dest_apic_id, uint32_t icr_low) {
    (void)smp_icr_idle();
    lapic_write_reg(LAPIC_REG_ICR_HIGH, (dest_apic_id & 0xFFu) << 24);
    lapic_write_reg(LAPIC_REG_ICR_LOW, icr_low);
    __asm__ volatile("" ::: "memory");
    (void)smp_icr_idle();
}
static void smp_delay_us(uint32_t us) {
    uint64_t n = (uint64_t)us * SMP_SPIN_PER_US;
    while (n--) __asm__ volatile("pause");
}
static void smp_delay_ms(uint32_t ms) { smp_delay_us(ms * 1000u); }

/* 有界等待 g_ap_online[index]（约 SMP_AP_ONLINE_TIMEOUT_MS）。tick 可能
 * 不走（IF=0），所以只用 pause 上界计时，绝不挂死。 */
static bool smp_wait_ap_online(uint32_t index, uint32_t timeout_ms) {
    uint64_t spins = (uint64_t)timeout_ms * SMP_SPIN_PER_US * 1000ull;
    while (spins--) {
        if (g_ap_online[index]) return true;
        __asm__ volatile("pause");
    }
    return g_ap_online[index] != 0;
}

/* ==================== 共享块填充 ==================== */
static void smp_put64(volatile uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}
static uint64_t smp_get64(const volatile uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* 临时 GDT（写到共享块 0x8850；跳板 rep movsb 拷到 0x8C00 后 lgdt）：
 *   entry0 空 / entry1(sel 0x08) 64 位代码 / entry2(sel 0x10) 数据 /
 *   entry3(sel 0x18) 32 位代码 —— AP 进长模式前要在保护模式里跑一段
 *   _start32，必须有 32 位代码段（跳板 jmp 0x18:_start32）。 */
static void smp_fill_gdt(volatile uint8_t *gdt) {
    smp_put64(gdt + 0,  0x0000000000000000ull);
    smp_put64(gdt + 8,  0x00AF9A000000FFFFull);   /* 64-bit code: L=1 G=1 P S X/R */
    smp_put64(gdt + 16, 0x00CF92000000FFFFull);   /* data:        D=1 G=1 P S RW */
    smp_put64(gdt + 24, 0x00CF9A000000FFFFull);   /* 32-bit code: D=1 G=1 P S X/R */
    for (int i = 4; i < 8; i++) smp_put64(gdt + (uint32_t)i * 8, 0);
}

/* 填共享块（每次启动一个 AP 前刷新）。跳板 64 位段是
 *   mov rax,[0x8830]; mov rax,[rax]; jmp rax   —— 二级间接：
 *   entry_rip 字段里放的是"指向入口地址变量的指针"，不是入口本身；
 *   同理 lidt 用的是 mov rax,[0x8890]; lidt [rax] —— idtr 字段前 8 字节
 *   是"指向 10 字节 IDTR 描述符的指针"。 */
static void smp_fill_shared(uint32_t index, uint32_t apic_id, uint64_t stack_top) {
    struct smp_shared *sh = (struct smp_shared *)(uintptr_t)SMP_SHARED_PHYS;

    sh->magic       = SMP_SHARED_MAGIC;
    sh->index       = index;
    sh->apic_id     = apic_id;
    sh->sipi_vector = SMP_SIPI_VECTOR;
    sh->cr3         = smp_read_cr3();
    sh->cr0         = smp_read_cr0();
    sh->cr4         = smp_read_cr4();
    sh->efer        = smp_rdmsr(0xC0000080u);     /* IA32_EFER */

    s_ap_entry_var  = (uint64_t)(uintptr_t)smp_ap_entry64;
    sh->entry_rip   = (uint64_t)(uintptr_t)&s_ap_entry_var;   /* [[entry_rip]] */

    sh->rsp         = stack_top;
    sh->flag_ptr    = (uint64_t)(uintptr_t)&g_ap_online[index];
    sh->count_ptr   = (uint64_t)(uintptr_t)&g_ap_online_count;

    smp_fill_gdt(sh->gdt);

    smp_sidt((void *)&s_ap_idtr);
    smp_put64(sh->idtr, (uint64_t)(uintptr_t)&s_ap_idtr);     /* [[idtr]] */
}

/* ==================== 准备：跳板拷贝 + 每核栈 ==================== */
static const uint8_t *smp_trampoline_src(uint32_t *len_out) {
    uintptr_t s = (uintptr_t)ap_trampoline_bin_start;
    uintptr_t e = (uintptr_t)ap_trampoline_bin_end;
    if (s != 0 && e > s && (e - s) <= 0x0C00u &&
        (e - s) >= 0x800u + sizeof(struct smp_shared)) {
        *len_out = (uint32_t)(e - s);
        return (const uint8_t *)s;
    }
    smp_finalize_blob();
    *len_out = (uint32_t)sizeof(s_blob_storage);
    return s_blob_storage;
}

/* 返回 0 = 就绪；-1 = 致命（一个 AP 都不启动） */
static int smp_prepare(void) {
    uint32_t len = 0;
    const uint8_t *src = smp_trampoline_src(&len);
    if (len == 0 || len > 0x0C00u || len < 0x800u + sizeof(struct smp_shared)) {
        console_printf("[SMP] trampoline blob invalid (len=%u) -> no ap started\n", len);
        return -1;
    }

    /* 0x8000 页必须在当前页表里 present+writable（boot.S 恒等映射前 1GiB；
     * 若将来内核改成高半区无低端恒等映射，这里会干净地拒绝启动） */
    if (!smp_pa_range_ok(SMP_TRAMPOLINE_PHYS, 0x1000u, true)) {
        console_puts("[SMP] trampoline page 0x8000 not identity-mapped+writable -> no ap started\n");
        return -1;
    }

    /* 整页清零 -> 拷入 blob（blob 自带共享块模板，magic='SMPT'） */
    memset((void *)(uintptr_t)SMP_TRAMPOLINE_PHYS, 0, 0x1000u);
    memcpy((void *)(uintptr_t)SMP_TRAMPOLINE_PHYS, src, len);
    console_printf("[SMP] trampoline copied to 0x%x (%u bytes)\n",
                   (uint64_t)SMP_TRAMPOLINE_PHYS, len);

    /* 每核栈：SMP_AP_STACK_PAGES 连续页（16KB），栈顶 16 字节对齐。
     * 用 moe_pmm_alloc(n) 而不是逐页 alloc：AP 栈在恒等映射下虚拟=物理，
     * 必须连续，单页分配器不保证连续。栈页同样要求在恒等映射内。 */
    for (int i = 1; i <= g_ap_n; i++) {
        struct ap_state *ap = &g_aps[i];
        ap->allocated   = false;
        ap->stack_base  = 0;
        ap->stack_top   = 0;
        ap->stack_bytes = 0;
        ap->apic_id     = g_ap_ids[i - 1];

        uintptr_t base = moe_pmm_alloc(SMP_AP_STACK_PAGES);
        if (base == 0) {
            console_printf("[SMP] stack alloc failed for AP %u -> skipped\n",
                           (uint64_t)ap->apic_id);
            continue;
        }
        if (!smp_pa_range_ok((uint64_t)base, SMP_AP_STACK_BYTES, true)) {
            console_printf("[SMP] AP %u stack at 0x%lx outside identity map -> skipped\n",
                           (uint64_t)ap->apic_id, (uint64_t)base);
            moe_pmm_free(base, SMP_AP_STACK_PAGES);
            continue;
        }
        memset((void *)base, 0, SMP_AP_STACK_BYTES);
        ap->stack_base  = base;
        ap->stack_bytes = SMP_AP_STACK_BYTES;
        ap->stack_top   = base + SMP_AP_STACK_BYTES;   /* 16KB 是 16 的倍数 -> 16B 对齐 */
        ap->allocated   = true;
    }
    return 0;
}

/* 发 SIPI 之前的自检：不通过则一个 SIPI 都不发（AP 起不起来是环境事实，
 * 共享块写错是我方 bug —— 后者必须在发 IPI 之前拦住）。 */
static uint32_t smp_selftest(void) {
    uint32_t fails = 0;
    const struct smp_shared *sh = (const struct smp_shared *)(uintptr_t)SMP_SHARED_PHYS;

    if (sh->magic != SMP_SHARED_MAGIC) fails |= 0x01;
    if (sh->sipi_vector != SMP_SIPI_VECTOR) fails |= 0x01;
    if ((sh->cr0 & 1ull) == 0 || (sh->cr0 & (1ull << 31)) == 0) fails |= 0x01;   /* PE|PG */
    if ((smp_read_cr3() & 0x000FFFFFFFFFF000ull) == 0) fails |= 0x01;

    /* 临时 GDT：0x08 64 位代码 / 0x10 数据 / 0x18 32 位代码（access/gran） */
    if (smp_get64(sh->gdt + 8)  != 0x00AF9A000000FFFFull) fails |= 0x02;
    if (smp_get64(sh->gdt + 16) != 0x00CF92000000FFFFull) fails |= 0x02;
    if (sh->gdt[3 * 8 + 5] != 0x9A || sh->gdt[3 * 8 + 6] != 0xCF) fails |= 0x02;

    /* 入口二级间接：[[entry_rip]] == smp_ap_entry64 */
    if (sh->entry_rip == 0 ||
        smp_get64((const volatile uint8_t *)(uintptr_t)sh->entry_rip) !=
            (uint64_t)(uintptr_t)smp_ap_entry64)
        fails |= 0x04;

    /* IDTR：字段是指针，指向的 10 字节描述符 limit/base 均非 0（sidt 自取） */
    if (smp_get64(sh->idtr) == 0) fails |= 0x04;
    else {
        const struct smp_idtr10 *id =
            (const struct smp_idtr10 *)(uintptr_t)smp_get64(sh->idtr);
        if (id->limit == 0 || id->base == 0) fails |= 0x04;
    }

    /* 每核栈：已分配的必须页对齐、栈顶 16B 对齐、大小正确 */
    for (int i = 1; i <= g_ap_n; i++) {
        const struct ap_state *ap = &g_aps[i];
        if (!ap->allocated) continue;
        if (ap->stack_base == 0 || (ap->stack_base & 0xFFFull) != 0 ||
            (ap->stack_top & 0xFull) != 0 ||
            ap->stack_top != ap->stack_base + ap->stack_bytes ||
            ap->stack_bytes != SMP_AP_STACK_BYTES)
            fails |= 0x08;
    }
    return fails;
}

/* ==================== AP 启动序列（BSP 侧） ==================== */
void smp_start_aps(void) {
    if (g_ap_n <= 0) {
        console_puts("[SMP] no AP to start\n");
        return;
    }

    if (smp_prepare() != 0) return;

    /* 先按 index=1 填一遍共享块，让自检覆盖 GDT/IDTR/入口链（这些每核
     * 重填值相同；index/apic_id/rsp 每核启动前还会刷新） */
    smp_fill_shared(1, g_aps[1].apic_id, g_aps[1].stack_top);
    uint32_t smask = smp_selftest();
    if (smask != 0) {
        console_printf("[SMP] ap start skipped (selftest FAIL mask=0x%x)\n", smask);
        return;
    }

    for (int i = 1; i <= g_ap_n; i++) {
        const struct ap_state *ap = &g_aps[i];
        if (!ap->allocated) continue;   /* 栈没分到的那核前面已记过日志 */

        /* 刷新共享块：本核的序号/APIC ID/栈顶（AP 起来第一件事就是读它；
         * 只有看到上一个 AP 的在线标志才会走到这里，无竞争） */
        smp_fill_shared((uint32_t)i, ap->apic_id, ap->stack_top);

        /* BSP 的"意图"日志必须在发 SIPI 之前打完；之后等待期间保持沉默，
         * AP 打完自己那行 online 并置标志后 BSP 才继续打 —— 两边日志不交错 */
        console_printf("[SMP] starting AP %u (index=%u stack=0x%lx)\n",
                       (uint64_t)ap->apic_id, (uint64_t)i, ap->stack_top);

        /* 标准 INIT-SIPI-SIPI（Intel MP/ACPI 通用做法）：
         *   INIT assert (0x4500) -> 10ms -> INIT deassert (0x8500) -> 200us
         *   -> SIPI (0x600|8) -> 200us -> SIPI 再发一次（容错） */
        smp_send_ipi_raw(ap->apic_id, SMP_ICR_INIT_ASSERT);
        smp_delay_ms(10);
        smp_send_ipi_raw(ap->apic_id, SMP_ICR_INIT_DEASSERT);
        smp_delay_us(200);
        smp_send_ipi_raw(ap->apic_id, SMP_ICR_SIPI | SMP_SIPI_VECTOR);
        smp_delay_us(200);
        smp_send_ipi_raw(ap->apic_id, SMP_ICR_SIPI | SMP_SIPI_VECTOR);

        /* 有界等待在线标志（约 500ms）；超时只记日志，继续下一个 AP */
        if (!smp_wait_ap_online((uint32_t)i, SMP_AP_ONLINE_TIMEOUT_MS)) {
            console_printf("[SMP] AP %u timeout after %u ms\n",
                           (uint64_t)ap->apic_id, (uint64_t)SMP_AP_ONLINE_TIMEOUT_MS);
        }
    }
}

/* ==================== AP 侧 64 位 C 入口 ====================
 * 由跳板 _start64 经 [[entry_rip]] jmp 进来：CS=0x08/SS=0x10（临时 GDT）、
 * CR3=BSP 页表（前 1GiB 恒等映射 + 高位 LAPIC 窗口）、IDTR=BSP 的 IDT、
 * RSP=本核 16KB 栈顶（跳板已 16B 对齐）。进入时 IF=0。 */
void smp_ap_entry64(void) {
    const struct smp_shared *sh = (const struct smp_shared *)(uintptr_t)SMP_SHARED_PHYS;
    /* 先把本核身份取出来：BSP 只有看到在线标志后才会覆写共享块 */
    uint32_t index   = sh->index;
    uint32_t apic_id = sh->apic_id;

    if (sh->magic != SMP_SHARED_MAGIC || index < 1 || index > (uint32_t)SMP_MAX_CPUS) {
        for (;;) __asm__ volatile("cli; hlt");
    }

    /* 给本核装好 LAPIC（SVR 使能 + 全部 LVT 掩码，含 LVT timer + TPR=0） */
    (void)hanos_lapic_init();

    struct cpu_local *cl = &cpu_locals[index];
    cl->cpu_id       = index;
    cl->apic_id      = apic_id;
    cl->online       = 1;
    cl->is_bsp       = false;
    cl->current_task = NULL;
    cl->idle_task    = NULL;
    cl->eevdf        = eevdf_get_global();

    /* 第二个 %u 是从本核自己的 LAPIC MMIO 读回的 ID（lapic=0 说明 LAPIC
     * 没装上）。这行由 AP 自己打 —— "AP 真的在跑"的直接证据。 */
    console_printf("[SMP] AP %u online (lapic=%u)\n",
                   (uint64_t)apic_id,
                   (uint64_t)((hanos_lapic_read(LAPIC_REG_ID) >> 24) & 0xFFu));

    /* 原子递增在线计数（lock xadd），最后再置在线标志放行 BSP：
     * ① BSP 看到标志时本核数据必已就绪；
     * ② 本核这行日志先打完，BSP 后续日志不会与它交错。 */
    __sync_fetch_and_add(&g_ap_online_count, 1);
    g_ap_online[index] = 1;

    /* AP 参与内核执行（本轮新增，取代原来的 `sti; hlt` 空转）：
     *   ① 本核 LAPIC 定时器 100 Hz（向量 0x20）——AP 也有时间基准与节拍，
     *      中断处理里按本核计数（BSP 才递增 g_tick_count，避免计时翻倍）；
     *   ② 空闲/派发循环：取 SMP 工作队列并执行（work_done 计数），无事可做
     *      时 hlt 等中断，同时响应 0xE0/0xE1 IPI（IDT 门已注册）。
     * 刻意不进 task.c/eevdf 的上下文切换：那两套调度器共用 context 结构，
     * 合并之前让 AP 切任务会重演 vector=6 (#UD) 洪水。 */
    hanos_lapic_timer_init(100, true);
    console_puts("[SMP] AP timer 100 Hz armed, entering idle/work loop\n");
    smp_ap_idle_loop(index);
}

/* ==================== SMP 初始化入口 ==================== */
int smp_init(void) {
    console_puts("[SMP] initializing...\n");

    /* Initialize LAPIC through hanos_lapic_init */
    if (!hanos_lapic_init()) {
        console_puts("[SMP] LAPIC init failed, skipped\n");
        return 0;
    }

    /* 获取 BSP LAPIC ID */
    g_bsp_lapic_id = (lapic_read_reg(LAPIC_REG_ID) >> 24) & 0xFF;
    console_printf("[SMP] BSP LAPIC ID: %u\n", (uint64_t)g_bsp_lapic_id);

    /* LAPIC MMIO 自检：hanos_lapic_init 会置 SVR 的 bit8（LAPIC_SVR_ENABLE）。
     * 读回来没有 = lapic 驱动的 MMIO 没真正生效（比如 lapic_base 没赋值），
     * 此时写 ICR 只会石沉大海 —— 提前报错，不做任何 AP 启动尝试。 */
    uint32_t svr = lapic_read_reg(LAPIC_REG_SVR);
    if ((svr & LAPIC_SVR_ENABLE) == 0) {
        console_printf("[SMP] LAPIC not responding (SVR=0x%x), AP start skipped\n",
                       (uint64_t)svr);
        return 0;
    }

    /* System tick for the BSP comes from its own LAPIC timer (vector 0x20).
     * Dependency on the legacy PIT was removed after QEMU's `-d int` trace showed
     * that this machine delivers NO IRQ0 to the CPU at all: it powers up in APIC
     * mode (IOAPIC present), so the 8259's INTR is not routed to LINT0 and the
     * PIC path is dead whatever we do with the IMCR. The LAPIC timer already
     * ticks on the APs, so it is the mechanism that actually works here. */
    hanos_lapic_timer_init(100, true);
    console_puts("[SMP] BSP LAPIC timer armed (~100 Hz system tick)\n");

    /* MADT: enabled 的本地 APIC ID（含 BSP 自己；一次调用拿全量） */
    uint8_t *ids = NULL;
    int total_cpus = hanos_acpi_madt_parse_lapic_ids(&ids, SMP_MAX_CPUS);
    if (total_cpus <= 1 || !ids) {
        console_puts("[SMP] single CPU or MADT parsing failed\n");
        if (ids) free(ids);
        return 0;
    }

    /* 过滤掉 BSP 自己 -> 待启动 AP 列表（index 从 1 起，与 g_aps 对齐）。
     * 不做 count 截断：MADT 里 BSP 的位置不定，截断会误伤尾部的真 AP。 */
    g_ap_n = 0;
    for (int i = 0; i < total_cpus; i++) {
        if ((uint32_t)ids[i] == g_bsp_lapic_id) continue;
        if (g_ap_n >= SMP_MAX_CPUS) break;
        g_ap_ids[g_ap_n++] = ids[i];
    }
    free(ids);

    if (g_ap_n == 0) {
        console_puts("[SMP] no AP APIC IDs from MADT\n");
        return 0;
    }

    console_printf("[SMP] cpus=%d ap=%d bsp_lapic_id=%u\n",
                   (uint64_t)total_cpus, (uint64_t)g_ap_n, (uint64_t)g_bsp_lapic_id);

    /* 初始化 BSP 的 cpu_local */
    cpu_locals[0].cpu_id = 0;
    cpu_locals[0].apic_id = g_bsp_lapic_id;
    cpu_locals[0].online = 1;
    cpu_locals[0].is_bsp = true;
    cpu_locals[0].current_task = &sched_cpu.tasks[sched_cpu.current];
    cpu_locals[0].eevdf = eevdf_get_global();

    /* 真正把 AP 拉起来（跳板 + INIT-SIPI-SIPI，见 smp_start_aps） */
    smp_start_aps();

    /* 汇总 */
    uint32_t online = smp_online_cpu_count();
    console_printf("[SMP] online=%u/%d bsp_lapic_id=%u\n",
                   online, (uint64_t)total_cpus, (uint64_t)g_bsp_lapic_id);
    if (online < (uint32_t)total_cpus) {
        console_puts("[SMP] note: some AP did not come up (env fact, not fatal)\n");
    }

    return 0;
}

/* ==================== IPI 发送 ==================== */
/* 固定投递模式（ICR_LOW[10:8]=000）+ level=assert(bit14)，向量在低 8 位。
 * 公式与 LAPIC 规范/原实现兼容。 */
void smp_send_ipi(uint32_t dest_apic_id, uint32_t vector) {
    smp_send_ipi_raw(dest_apic_id, 0x00004000u | (vector & 0xFFu));
}

void smp_send_ipi_all_others(uint32_t vector) {
    if (smp_online_cpu_count() > 1) {
        (void)smp_icr_idle();
        lapic_write_reg(LAPIC_REG_ICR_HIGH, 0);   /* 简写模式：目标字段被忽略 */
        lapic_write_reg(LAPIC_REG_ICR_LOW, 0x000C4000u | (vector & 0xFFu));
        /* 0xC0000 = 简写"除自身外全部"(bits[19:18]=11)，0x4000 = level assert */
        __asm__ volatile("" ::: "memory");
        (void)smp_icr_idle();
    }
}

/* ==================== Per-CPU 访问 ==================== */
struct cpu_local *smp_get_cpu_local(uint32_t cpu_id) {
    if (cpu_id < SMP_MAX_CPUS) return &cpu_locals[cpu_id];
    return NULL;
}

/* 用本核 LAPIC ID 反查 cpu_locals（lapic 驱动未生效时读回 0，退化为
 * cpu_locals[0]，与旧的"恒返回 BSP"行为兼容；AP 上则返回 AP 自己）。 */
uint32_t smp_get_cpu_id(void) {
    uint32_t apic_id = (lapic_read_reg(LAPIC_REG_ID) >> 24) & 0xFFu;
    for (uint32_t i = 0; i < SMP_MAX_CPUS; i++) {
        if (cpu_locals[i].online && cpu_locals[i].apic_id == apic_id)
            return cpu_locals[i].cpu_id;
    }
    return 0;
}

struct cpu_local *smp_this_cpu(void) {
    return &cpu_locals[smp_get_cpu_id()];
}

/* ==================== 调度器集成 ==================== */
void smp_scheduler_init_cpu(uint32_t cpu_id) {
    struct cpu_local *cl = &cpu_locals[cpu_id];
    if (cpu_id == 0) {
        cl->eevdf = eevdf_get_global();
    }
}

/* ==================== IPI 处理器 ==================== */
void smp_reschedule_ipi_handler(void) {
    g_need_resched = true;
    hanos_lapic_eoi();
}

void smp_shootdown_ipi_handler(void *addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
    hanos_lapic_eoi();
}

/* ==================== 唤醒空闲 CPU ==================== */
void smp_wake_idle_cpu(void) {
    if (smp_online_cpu_count() > 1) {
        smp_send_ipi_all_others(SMP_IPI_RESCHEDULE);
    }
}

/* ==================== 在线 CPU 计数 ==================== */
uint32_t smp_online_cpu_count(void) {
    return (uint32_t)g_ap_online_count;   /* BSP=1，每个上线 AP lock xadd +1 */
}

/* ==================== SMP 工作队列（让 AP 真正参与内核执行） ====================
 * 背景：AP 上线后只 `sti; hlt`，是"停着的核"。这里给出最小可验证的参与机制：
 *   - BSP 用 smp_submit_work() 把内核工作项投进队列；
 *   - 每个 CPU 的空闲/主循环用 smp_drain_work_here() 取走并执行（AP 在自己的
 *     idle 循环里，BSP 在 shell 输入循环里），并给本核 work_done 计数。
 * 这与 Linux 的 per-CPU workqueue 思路一致，但刻意不碰 task.c/eevdf 那套
 * 上下文切换（两套调度器共用 context 是已知雷区），因此是"AP 执行内核任务"
 * 而不是"AP 调度用户任务"——后者需要先合并调度器。 */
#define SMP_WORK_MAX 64
struct smp_work { void (*fn)(void*); void* arg; };
static struct smp_work g_work[SMP_WORK_MAX];
static volatile uint32_t g_work_head, g_work_tail;
static volatile int g_work_lock;

static void smp_work_lock(void) {
    while (__sync_lock_test_and_set(&g_work_lock, 1)) __asm__ volatile("pause");
}
static void smp_work_unlock(void) { __sync_lock_release(&g_work_lock); }

int smp_submit_work(void (*fn)(void*), void* arg) {
    if (!fn) return -1;
    int rc = -1;
    smp_work_lock();
    uint32_t next = (g_work_tail + 1u) % SMP_WORK_MAX;
    if (next != g_work_head) {              /* not full */
        g_work[g_work_tail].fn = fn;
        g_work[g_work_tail].arg = arg;
        g_work_tail = next;
        rc = 0;
    }
    smp_work_unlock();

    /* Wake the other CPUs: an idle AP sits in `hlt`, so it will not look at the
     * queue until an interrupt arrives. This is the same "wake an idle CPU"
     * step Linux does when work is queued to a per-CPU worker. */
    if (rc == 0 && g_ap_online_count > 1)
        smp_send_ipi_all_others(SMP_IPI_RESCHEDULE);
    return rc;
}

static int smp_work_pop(void (**fn)(void*), void** arg) {
    int got = 0;
    smp_work_lock();
    if (g_work_head != g_work_tail) {
        *fn = g_work[g_work_head].fn;
        *arg = g_work[g_work_head].arg;
        g_work_head = (g_work_head + 1u) % SMP_WORK_MAX;
        got = 1;
    }
    smp_work_unlock();
    return got;
}

uint32_t smp_work_pending(void) {
    uint32_t h, t;
    smp_work_lock();
    h = g_work_head; t = g_work_tail;
    smp_work_unlock();
    return (t + SMP_WORK_MAX - h) % SMP_WORK_MAX;
}

void smp_drain_work_here(uint32_t cpu_index) {
    void (*fn)(void*) = NULL;
    void* arg = NULL;
    while (smp_work_pop(&fn, &arg)) {
        fn(arg);
        if (cpu_index < (uint32_t)SMP_MAX_CPUS)
            cpu_locals[cpu_index].work_done++;
    }
}

void smp_ap_timer_tick(uint32_t cpu_index) {
    if (cpu_index < (uint32_t)SMP_MAX_CPUS) {
        cpu_locals[cpu_index].jiffies++;
        cpu_locals[cpu_index].lapic_timer_count++;
    }
}

/* 找出本核的 cpu_locals（用本核 LAPIC ID 匹配，不依赖任何 per-CPU 寄存器） */
static struct cpu_local* smp_local_by_lapic(void) {
    const uint32_t id = (hanos_lapic_read(LAPIC_REG_ID) >> 24) & 0xFFu;
    for (uint32_t i = 0; i < (uint32_t)SMP_MAX_CPUS; i++) {
        if (cpu_locals[i].online && cpu_locals[i].apic_id == id)
            return &cpu_locals[i];
    }
    return NULL;
}

void smp_ipi_dispatch(uint64_t vec) {
    struct cpu_local* cl = smp_local_by_lapic();
    if (cl) cl->ipis++;

    switch (vec) {
    case SMP_IPI_RESCHEDULE:
        smp_reschedule_ipi_handler();      /* 内含 LAPIC EOI */
        break;
    case SMP_IPI_SHOOTDOWN:
        smp_shootdown_ipi_handler(NULL);   /* 内含 LAPIC EOI */
        break;
    default:
        /* 0xFF = LAPIC spurious：只 EOI，不做任何事 */
        hanos_lapic_eoi();
        break;
    }
}

void smp_ap_idle_loop(uint32_t cpu_index) {
    for (;;) {
        __asm__ volatile("sti");
        smp_drain_work_here(cpu_index);
        if (cpu_index < (uint32_t)SMP_MAX_CPUS)
            cpu_locals[cpu_index].idle_jiffies++;
        __asm__ volatile("hlt");
    }
}

/* ---- 验收自检：投 n 个任务，看是谁执行的 ---- */
static volatile uint32_t g_selftest_done;

static void smp_selftest_work(void* arg) {
    (void)arg;
    __sync_fetch_and_add(&g_selftest_done, 1);
}

extern volatile uint64_t g_tick_count;   /* kernel/main/init.c, ~100 Hz */

void smp_work_selftest(uint32_t n) {
    if (n == 0) n = 8;
    if (n > (uint32_t)(SMP_WORK_MAX - 1)) n = (uint32_t)(SMP_WORK_MAX - 1);

    const uint32_t cpus = smp_online_cpu_count();
    uint64_t before[SMP_MAX_CPUS];
    for (uint32_t i = 0; i < (uint32_t)SMP_MAX_CPUS; i++) before[i] = cpu_locals[i].work_done;

    console_printf("[SMP] work selftest: queuing %u items for %u CPU(s)\n",
                   (uint64_t)n, (uint64_t)cpus);
    g_selftest_done = 0;
    uint32_t queued = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (smp_submit_work(smp_selftest_work, NULL) == 0) queued++;
    }
    console_printf("[SMP] work selftest: queued=%u pending=%u\n",
                   (uint64_t)queued, (uint64_t)smp_work_pending());

    /* Give the APs a real chance first: smp_submit_work() woke them with an IPI,
     * so wait a bounded number of PAUSE iterations WITHOUT draining on this CPU.
     * The bound is a raw spin count on purpose (not g_tick_count): a stalled
     * system timer must never be able to wedge the shell. Only afterwards does
     * the BSP pick up whatever is left. */
    for (volatile uint64_t spin = 0; spin < 200000000ull && g_selftest_done < queued; spin++) {
        __asm__ volatile("pause");
    }
    const uint32_t ap_took = (uint32_t)g_selftest_done;
    console_printf("[SMP] work selftest: APs finished %u of %u before the BSP helped\n",
                   (uint64_t)ap_took, (uint64_t)queued);
    while (g_selftest_done < queued) smp_drain_work_here(0);

    uint64_t total = 0;
    for (uint32_t i = 0; i < cpus && i < (uint32_t)SMP_MAX_CPUS; i++) {
        const uint64_t d = cpu_locals[i].work_done - before[i];
        total += d;
        console_printf("[SMP]   cpu%u (%s): same-window work_done=%u jiffies=%u ipis=%u\n",
                       (uint64_t)i, cpu_locals[i].is_bsp ? "BSP" : "AP",
                       d, cpu_locals[i].jiffies, cpu_locals[i].ipis);
    }
    console_printf("[SMP] work selftest: completed %u/%u in window, total executed=%u\n",
                   (uint64_t)g_selftest_done, (uint64_t)queued, total);
    if (cpus > 1 && cpu_locals[1].work_done > before[1]) {
        console_puts("[SMP] work selftest PASS (an AP executed kernel work)\n");
    } else if (cpus <= 1) {
        console_puts("[SMP] work selftest PASS (single CPU, BSP executed the work)\n");
    } else {
        console_puts("[SMP] work selftest PARTIAL (AP online but executed nothing)\n");
    }
}
