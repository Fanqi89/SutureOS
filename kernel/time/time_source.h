/* SutureOS: generic time source registry (mechanism re-written for x86_64 from
 * the AArch64/RISC-V timebase idea; see kernel/time/time_source.c for details).
 */
#ifndef STITCH_TIME_SOURCE_H
#define STITCH_TIME_SOURCE_H

#include <stitch/types.h>

typedef struct time_source {
    const char *name;
    uint64_t freq_hz;          /* counter ticks per second */
    uint64_t (*read)(void);    /* raw monotonic counter */
    int prio;                  /* higher wins when several are available */
} time_source_t;

void time_source_init(void);                    /* register the x86_64 sources */
void time_source_register(const time_source_t *src);
int  time_source_count(void);
const time_source_t *time_source_at(int index);
const time_source_t *time_source_best(void);
uint64_t time_source_ticks_to_ns(uint64_t ticks, uint64_t freq_hz);
uint64_t time_source_now_ns(void);
uint64_t time_source_uptime_us(void);

void st_time_source_test(void);

#endif /* STITCH_TIME_SOURCE_H */
