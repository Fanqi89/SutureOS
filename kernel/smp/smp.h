/* SutureOS: SMP 多核支持 (移植自 VimtuOS + Cinux)
 * Sources: 
 *   - VimtuOS: C:\Users\fanqi\Desktop\自研操作系统\VimtuOS\kernel\smp64.cpp
 *   - Cinux: C:\Users\fanqi\Desktop\自研操作系统\Cinux\kernel\arch\x86_64\smp.hpp
 * Original licenses: GPL-3.0 (both)
 * Changes: Combined best parts; per-CPU EEVDF runqueues; IPI reschedule/shootdown
 *          Real AP bring-up: INIT-SIPI-SIPI via ap_trampoline.asm shared block.
 */
#ifndef STITCH_SMP_H
#define STITCH_SMP_H

#include <stitch/types.h>

#define SMP_MAX_CPUS 256
#define SMP_TRAMPOLINE_PHYS 0x8000
#define SMP_SIPI_VECTOR 0x08
#define SMP_AP_STACK_PAGES 4  /* 16 KB per AP */
#define SMP_AP_STACK_BYTES (SMP_AP_STACK_PAGES * 4096)
#define SMP_AP_ONLINE_TIMEOUT_MS 500

/* Shared trampoline data block lives at page offset 0x800 of the trampoline
 * page (physical 0x8800); layout matches ap_trampoline.asm S_* offsets and
 * struct smp_shared below. */
#define SMP_SHARED_PHYS (SMP_TRAMPOLINE_PHYS + 0x800)  /* 0x8800 */
#define SMP_SHARED_MAGIC 0x544D5053u                   /* 'SMPT' */

/* IPI vectors (match Cinux) */
#define SMP_IPI_RESCHEDULE 0xE0
#define SMP_IPI_SHOOTDOWN  0xE1
#define SMP_LAPIC_TIMER    0x30

/* Per-CPU data structure */
struct cpu_local {
    uint32_t cpu_id;
    uint32_t apic_id;
    uint32_t online;           /* 1 = AP has started */
    uint64_t gdt_phys;         /* Per-CPU GDT physical address */
    uint64_t tss_phys;         /* Per-CPU TSS physical address */
    struct sched_task *current_task;  /* Currently running task */
    struct sched_task *idle_task;     /* Idle task for this CPU */
    struct eevdf_t *eevdf;    /* Per-CPU EEVDF scheduler */
    uint64_t lapic_timer_count;
    uint64_t idle_jiffies;
    uint64_t jiffies;
    uint64_t ipis;            /* IPIs (0xE0/0xE1/0xFF) received by this CPU */
    uint64_t work_done;       /* SMP work-queue items executed by this CPU */
    bool is_bsp;
};

/* Shared trampoline data (at SMP_TRAMPOLINE_PHYS + 0x800) */
struct smp_shared {
    volatile uint32_t magic;        /* 'SMPT' */
    volatile uint32_t index;        /* AP index (1-based) */
    volatile uint32_t apic_id;      /* Target LAPIC ID */
    volatile uint32_t sipi_vector;  /* SIPI vector */
    volatile uint64_t cr3;          /* Page table (CR3) */
    volatile uint64_t cr0;          /* CR0 (PE|PG) */
    volatile uint64_t cr4;          /* CR4 (PAE) */
    volatile uint64_t efer;         /* EFER (LME) */
    volatile uint64_t entry_rip;    /* AP entry point (smp_ap_entry64) */
    volatile uint64_t rsp;          /* AP kernel stack top */
    volatile uint64_t flag_ptr;     /* Pointer to g_ap_online[index] */
    volatile uint64_t count_ptr;    /* Pointer to g_ap_online_count */
    volatile uint8_t  gdt[64];      /* Temporary GDT copy (8 entries) */
    volatile uint8_t  idtr[10];     /* IDTR copy */
} __attribute__((packed));

/* AP state */
struct ap_state {
    uint64_t stack_base;
    uint64_t stack_top;
    uint64_t stack_bytes;
    uint32_t apic_id;
    bool allocated;
};

/* Global state */
extern struct cpu_local cpu_locals[SMP_MAX_CPUS];
extern struct ap_state g_aps[SMP_MAX_CPUS + 1];
extern volatile uint8_t g_ap_online[SMP_MAX_CPUS + 1];
extern volatile uint64_t g_ap_online_count;
extern uint32_t g_bsp_lapic_id;
extern uint32_t g_irq_mode;
extern uint64_t g_ticks;

/* SMP initialization (call from kmain after ACPI/MADT ready) */
int smp_init(void);

/* Start the prepared APs with the INIT-INIT deassert-SIPI-SIPI sequence.
 * Called at the end of smp_init(); exposed so APs can be started later
 * if the BSP collected the AP list earlier. */
void smp_start_aps(void);

/* 64-bit AP C entry point. Reached from ap_trampoline.asm _start64 via the
 * two-level chain in the shared block: [0x8830] holds &s_ap_entry_var and
 * [[0x8830]] is this function's address. Runs on the AP with the BSP's
 * CR3/IDT and the per-AP stack; brings the AP's LAPIC up, publishes
 * cpu_locals[index] + the online flag, then idles with `sti; hlt`.
 * Never returns. */
void smp_ap_entry64(void) __attribute__((noreturn));

/* IPI functions */
void smp_send_ipi(uint32_t dest_apic_id, uint32_t vector);
void smp_send_ipi_all_others(uint32_t vector);

/* Per-CPU access */
struct cpu_local *smp_get_cpu_local(uint32_t cpu_id);
struct cpu_local *smp_this_cpu(void);
uint32_t smp_get_cpu_id(void);

/* Scheduler integration */
void smp_scheduler_init_cpu(uint32_t cpu_id);

/* IPI handlers */
void smp_reschedule_ipi_handler(void);
void smp_shootdown_ipi_handler(void *addr);

/* Wake idle CPU */
void smp_wake_idle_cpu(void);

/* Get online CPU count */
uint32_t smp_online_cpu_count(void);

/* ==================== SMP work queue (AP participation) ====================
 * The BSP can hand kernel work to the other CPUs and every CPU's idle path
 * drains the queue, so an AP is no longer a parked core. Jobs must be short and
 * must not sleep. smp_submit_work returns 0 on success, -1 if full/invalid. */
int      smp_submit_work(void (*fn)(void*), void* arg);
uint32_t smp_work_pending(void);

/* Drain the queue on the calling CPU; cpu_index is the caller's cpu_locals
 * index (0 for the BSP). Used by the shell input loop and by the AP idle loop. */
void smp_drain_work_here(uint32_t cpu_index);

/* AP entry: program this CPU's LAPIC timer and run the idle/work loop forever. */
void smp_ap_idle_loop(uint32_t cpu_index) __attribute__((noreturn));

/* Called from the timer ISR when it fires on an AP (the BSP keeps g_tick_count). */
void smp_ap_timer_tick(uint32_t cpu_index);

/* IPI / LAPIC-spurious dispatch, called by moos_intr_handler for 0xE0/0xE1/0xFF. */
void smp_ipi_dispatch(uint64_t vec);

/* Acceptance test: submit n items, pump until they complete (bounded) and report
 * per-CPU execution counts, proving the APs actually ran kernel work. */
void smp_work_selftest(uint32_t n);

/* LAPIC access (from arch/x86_64/lapic.c) */
uint32_t hanos_lapic_read(uint32_t reg);
void hanos_lapic_write(uint32_t reg, uint32_t val);
void hanos_lapic_eoi(void);
bool hanos_lapic_init(void);
void hanos_lapic_setup_timer(uint8_t vector, uint8_t divide, uint32_t count);

/* IOAPIC access (from arch/x86_64/ioapic.c) */
bool hanos_ioapic_init(void);
void hanos_ioapic_set_irq(uint8_t irq, uint8_t vector, uint8_t delivery_mode);
void hanos_ioapic_mask_irq(uint8_t irq);
void hanos_ioapic_unmask_irq(uint8_t irq);

/* ACPI MADT parsing */
int hanos_acpi_madt_parse_lapic_ids(uint8_t **out_ids, int max_ids);

#endif /* STITCH_SMP_H */