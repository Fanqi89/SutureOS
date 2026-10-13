/* Test-only shadow of "mb_pit.h" (kernel/drivers/mb_pit.h).
 * mbos_pit_counter() is implemented by the harness as a fast monotonic counter
 * so net_now_us() can be exercised without real hardware. */
#ifndef TEST_MB_PIT_H
#define TEST_MB_PIT_H

#include <stitch/types.h>

#define MBOS_PIT_BASE_HZ 1193182u

void     mbos_pit_init(uint32_t hz);
uint32_t mbos_pit_hz(void);
uint32_t mbos_pit_divisor(void);
uint16_t mbos_pit_counter(void);

#endif /* TEST_MB_PIT_H */
