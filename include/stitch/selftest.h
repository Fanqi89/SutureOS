/* StitchOS (缝合怪系统) - self-test harness (排异检查动态部分)
 * Each ported module registers its boot-time self test; results
 * are printed over serial as [selftest][<name>] PASS/FAIL so the
 * QEMU verification script can parse them mechanically.
 */
#ifndef STITCH_SELFTEST_H
#define STITCH_SELFTEST_H

#include <stitch/types.h>

void st_run(const char *name, bool pass);   /* record + print result */
int  st_failures(void);                     /* 0 == all green       */
int  st_total(void);

#endif /* STITCH_SELFTEST_H */
