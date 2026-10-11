/* SutureOS module: Local APIC driver (lapic_ prefix)
 * Ported from: various x86_64 OS sources
 * Original license: GPL-3.0
 */
#ifndef STITCH_LAPIC_H
#define STITCH_LAPIC_H

#include <stitch/types.h>

/* LAPIC register offsets (from IA-32 SDM Vol 3A) */
#define LAPIC_REG_ID              0x020
#define LAPIC_REG_VERSION         0x030
#define LAPIC_REG_TPR             0x080  /* Task Priority Register */
#define LAPIC_REG_APR             0x090  /* Arbitration Priority Register */
#define LAPIC_REG_PPR             0x0A0  /* Processor Priority Register */
#define LAPIC_REG_EOI             0x0B0  /* End of Interrupt */
#define LAPIC_REG_LDR             0x0D0  /* Logical Destination Register */
#define LAPIC_REG_DFR             0x0E0  /* Destination Format Register */
#define LAPIC_REG_SVR             0x0F0  /* Spurious Interrupt Vector Register */
#define LAPIC_REG_ISR_BASE        0x100  /* In-Service Registers (8 x 32-bit) */
#define LAPIC_REG_TMR_BASE        0x180  /* Trigger Mode Registers */
#define LAPIC_REG_IRR_BASE        0x200  /* Interrupt Request Registers */
#define LAPIC_REG_ESR             0x280  /* Error Status Register */
#define LAPIC_REG_LVT_CMCI        0x2F0  /* LVT Corrected Machine Check */
#define LAPIC_REG_ICR_LOW         0x300  /* Interrupt Command Register Low */
#define LAPIC_REG_ICR_HIGH        0x310  /* Interrupt Command Register High */
#define LAPIC_REG_LVT_TIMER       0x320  /* LVT Timer */
#define LAPIC_REG_LVT_THERMAL     0x330  /* LVT Thermal Sensor */
#define LAPIC_REG_LVT_PMI         0x340  /* LVT Performance Monitoring */
#define LAPIC_REG_LVT_LINT0       0x350  /* LVT LINT0 */
#define LAPIC_REG_LVT_LINT1       0x360  /* LVT LINT1 */
#define LAPIC_REG_LVT_ERROR       0x370  /* LVT Error */
#define LAPIC_REG_TIMER_INIT_COUNT 0x380 /* Timer Initial Count */
#define LAPIC_REG_TIMER_CURRENT_COUNT 0x390 /* Timer Current Count */
#define LAPIC_REG_TIMER_DIVIDE    0x3E0  /* Timer Divide Configuration */

/* SVR bits */
#define LAPIC_SVR_ENABLE          (1u << 8)
#define LAPIC_SVR_VECTOR_MASK     0xFF

/* LVT Timer modes */
#define LAPIC_TIMER_ONESHOT       0x0
#define LAPIC_TIMER_PERIODIC      0x1
#define LAPIC_TIMER_TSC_DEADLINE  0x2

/* ICR delivery modes */
#define ICR_DELIVERY_FIXED        0x0
#define ICR_DELIVERY_LOWEST_PRI   0x1
#define ICR_DELIVERY_SMI          0x2
#define ICR_DELIVERY_NMI          0x4
#define ICR_DELIVERY_INIT         0x5
#define ICR_DELIVERY_STARTUP      0x6

/* ICR destination modes */
#define ICR_DEST_PHYSICAL         0x0
#define ICR_DEST_LOGICAL          0x1

/* LAPIC base MSR */
#define MSR_IA32_APIC_BASE        0x1B

/* Default LAPIC physical address */
#define LAPIC_DEFAULT_ADDR        0xFEE00000

/* Function prototypes */
bool hanos_lapic_init(void);
void hanos_lapic_eoi(void);
void hanos_lapic_timer_init(uint32_t hz, bool periodic);
void hanos_lapic_timer_stop(void);
uint32_t hanos_lapic_get_id(void);
void hanos_lapic_send_ipi(uint8_t dest_apic_id, uint8_t vector, uint8_t delivery_mode);

/* SMP 用的直接寄存器读写 */
uint32_t hanos_lapic_read(uint32_t reg);
void hanos_lapic_write(uint32_t reg, uint32_t value);

#endif /* STITCH_LAPIC_H */