/* SutureOS: ACPI power management (see power.c for the implementation notes) */
#ifndef STITCH_POWER_H
#define STITCH_POWER_H

#include <stitch/types.h>

/* Cache FADT-derived power information (safe to call more than once; does not
 * touch the hardware). */
void power_init(void);

/* Enter soft-off (S5) / reset the machine. Both only return when every
 * method failed, in which case they halt the CPU. */
void power_off(void);
void power_reset(void);

/* Diagnostics */
uint32_t power_pm1a_cnt(void);

#endif /* STITCH_POWER_H */
