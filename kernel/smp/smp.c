/* SutureOS: SMP 多核支持 - 最小框架版
 * 只提供编译通过的框架，实际 AP 启动留待 Phase-2 完善
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
#include "../sched/eevdf.h"
#include "../sched/task.h"

/* 外部符号 */
extern volatile bool g_need_resched;
extern sched_cpu_t sched_cpu;
extern struct eevdf_t *eevdf_get_global(void);
extern uint64_t g_ticks;
extern bool hanos_lapic_init(void);
extern uint32_t hanos_lapic_read(uint32_t reg);
extern void hanos_lapic_write(uint32_t reg, uint32_t val);
extern void hanos_lapic_eoi(void);
extern int hanos_acpi_madt_parse_lapic_ids(uint8_t **out_ids, int max_ids);

/* 内存管理 */
extern void *moe_pmm_alloc_page(void);
extern void moe_pmm_free_page(uintptr_t page);

/* 全局变量 */
extern uint64_t g_ticks;

/* LAPIC 访问 */
static inline uint32_t lapic_read_reg(uint32_t reg) {
    return hanos_lapic_read(reg);
}
static inline void lapic_write_reg(uint32_t reg, uint32_t value) {
    hanos_lapic_write(reg, value);
}
static inline void lapic_eoi(void) {
    hanos_lapic_eoi();
}

/* 全局状态 */
struct cpu_local cpu_locals[SMP_MAX_CPUS];
struct ap_state g_aps[SMP_MAX_CPUS + 1];
volatile uint8_t g_ap_online[SMP_MAX_CPUS + 1] = {0};
volatile uint64_t g_ap_online_count = 0;
uint32_t g_bsp_lapic_id = 0;
extern uint64_t g_ticks;

/* 简单的 MADT 解析收集 AP APIC ID */
static int smp_collect_apic_ids(uint32_t bsp_id, uint32_t *out, int max) {
    uint8_t *ids = NULL;
    int count = hanos_acpi_madt_parse_lapic_ids(&ids, max);
    if (count > 0 && ids) {
        for (int i = 0; i < count; i++) {
            out[i] = ids[i];
        }
        free(ids);
    }
    return count;
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
    g_bsp_lapic_id = (lapic_read_reg(0x20) >> 24) & 0xFF;
    console_printf("[SMP] BSP LAPIC ID: %u\n", g_bsp_lapic_id);

    /* 获取 CPU 数量和 APIC ID */
    int total_cpus = hanos_acpi_madt_parse_lapic_ids(NULL, 0);
    if (total_cpus <= 1) {
        console_puts("[SMP] single CPU or MADT parsing failed\n");
        return 0;
    }

    uint32_t apic_ids[SMP_MAX_CPUS];
    uint8_t *ids = NULL;
    int count = hanos_acpi_madt_parse_lapic_ids(&ids, SMP_MAX_CPUS);
    if (count <= 0 || !ids) {
        console_puts("[SMP] no AP APIC IDs from MADT\n");
        return 0;
    }

    for (int i = 0; i < count; i++) {
        apic_ids[i] = ids[i];
    }
    free(ids);

    if (count > total_cpus - 1) count = total_cpus - 1;

    console_printf("[SMP] cpus=%d ap=%d bsp_lapic_id=%u\n",
                   total_cpus, count, g_bsp_lapic_id);

    /* 初始化 BSP 的 cpu_local */
    cpu_locals[0].cpu_id = 0;
    cpu_locals[0].apic_id = g_bsp_lapic_id;
    cpu_locals[0].online = 1;
    cpu_locals[0].is_bsp = true;
    cpu_locals[0].current_task = &sched_cpu.tasks[sched_cpu.current];
    cpu_locals[0].eevdf = eevdf_get_global();

    /* TODO: AP 启动需要完整的跳板代码和上下文设置
     * 这里只做框架，实际 AP 启动留待后续实现 */

    console_puts("[SMP] framework initialized (AP boot pending)\n");

    /* 汇总 */
    uint32_t online = smp_online_cpu_count();
    console_printf("[SMP] online=%u/%d bsp_lapic_id=%u\n", online, total_cpus, g_bsp_lapic_id);

    return 0;
}

/* ==================== IPI 发送 ==================== */
void smp_send_ipi(uint32_t dest_apic_id, uint32_t vector) {
    while (lapic_read_reg(0x300) & (1 << 12)) __asm__ volatile("pause");
    lapic_write_reg(0x310, (dest_apic_id & 0xFF) << 24);
    lapic_write_reg(0x300, 0x00004000 | vector);
}

void smp_send_ipi_all_others(uint32_t vector) {
    if (smp_online_cpu_count() > 1) {
        lapic_write_reg(0x310, 0);  /* all-others shorthand */
        lapic_write_reg(0x300, 0x000C4000 | vector);
    }
}

/* ==================== Per-CPU 访问 ==================== */
struct cpu_local *smp_get_cpu_local(uint32_t cpu_id) {
    if (cpu_id < SMP_MAX_CPUS) return &cpu_locals[cpu_id];
    return NULL;
}

struct cpu_local *smp_this_cpu(void) {
    return &cpu_locals[0];
}

uint32_t smp_get_cpu_id(void) {
    return 0;
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
    return 1;
}