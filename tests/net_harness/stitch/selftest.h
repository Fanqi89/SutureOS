/* Test-only shadow of <stitch/selftest.h>. */
#ifndef TEST_STITCH_SELFTEST_H
#define TEST_STITCH_SELFTEST_H

#include <stitch/types.h>

void st_run(const char *name, bool pass);
int  st_failures(void);
int  st_total(void);

#endif /* TEST_STITCH_SELFTEST_H */
