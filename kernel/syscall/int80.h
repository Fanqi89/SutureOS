/* SutureOS: the Vimtu64-compatible `int 0x80` syscall gate (see int80.c). */
#ifndef SUTUREOS_INT80_H
#define SUTUREOS_INT80_H

#include <stitch/types.h>

/* Register the handler on vector 0x80 (the DPL=3 gate already exists in the
 * IDT). Call once during boot, after the syscall table is populated. */
int int80_init(void);

/* Number of int 0x80 calls seen / number answered with -ENOSYS. */
uint32_t int80_call_count(void);
uint32_t int80_denied_count(void);

/* Acceptance line for the boot log: gate reached + no unimplemented number. */
int int80_selftest(void);

#endif /* SUTUREOS_INT80_H */
