/* SutureOS: EL0 指针/字符串合法性检查 (hudos el0_ptr_ok/el0_str_ok 移植)
 * 仅当 g_el0_active=1 时检查，防止用户传内核指针
 */
#include <stitch/types.h>
#include <stitch/console.h>
#include "el0_validate.h"

#define EL0_MAX_RANGES 8

typedef struct {
    uint64_t start;
    uint64_t len;
} el0_range_t;

static el0_range_t el0_ranges[EL0_MAX_RANGES];
static int el0_range_cnt = 0;
static int g_el0_active = 0;

void el0_validate_init(void) { el0_range_cnt = 0; g_el0_active = 0; }

void el0_set_active(int active) { g_el0_active = active; }

/* 授权用户可访问范围：镜像、栈、参数页 */
int el0_grant_range(uint64_t start, uint64_t len)
{
    (void)start; (void)len;
    if (el0_range_cnt >= EL0_MAX_RANGES) return -1;
    el0_ranges[el0_range_cnt].start = start;
    el0_ranges[el0_range_cnt].len = len;
    el0_range_cnt++;
    return 0;
}

/* 检查指针是否在授权范围内 */
int el0_ptr_ok(uint64_t va, uint64_t len)
{
    if (!g_el0_active || !len) return 1;
    if (!va) return 0;
    for (int i = 0; i < el0_range_cnt; i++)
        if (va >= el0_ranges[i].start &&
            va + len <= el0_ranges[i].start + el0_ranges[i].len)
            return 1;
    console_printf("[el0] bad ptr va=%p len=%llu\n", (void*)va, len);
    return 0;
}

/* 检查字符串是否以 \0 结尾且在授权范围内 */
int el0_str_ok(const char *str)
{
    if (!g_el0_active) return 1;
    if (!str) return 0;
    for (int i = 0; i < el0_range_cnt; i++) {
        uint64_t start = el0_ranges[i].start;
        uint64_t end = start + el0_ranges[i].len;
        uint64_t p = (uint64_t)(uintptr_t)str;
        if (p < start || p >= end) continue;
        while (p < end) {
            if (*(volatile char *)p == 0) return 1;
            p++;
        }
        return 0;  /* 没找到 \0 */
    }
    return 0;
}