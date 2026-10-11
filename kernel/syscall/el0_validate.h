#ifndef STITCH_EL0_VALIDATE_H
#define STITCH_EL0_VALIDATE_H

#include <stitch/types.h>

void el0_validate_init(void);
void el0_set_active(int active);
int el0_grant_range(uint64_t start, uint64_t len);
int el0_ptr_ok(uint64_t va, uint64_t len);
int el0_str_ok(const char *str);

#define EL0_CHECK(ptr, len) \
    do { if (!el0_ptr_ok((uint64_t)(uintptr_t)(ptr), (len))) return (uint64_t)-1; } while(0)
#define EL0_STR(str) \
    do { if (!el0_str_ok(str)) return (uint64_t)-1; } while(0)

#endif