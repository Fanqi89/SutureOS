/* StitchOS (缝合怪系统) - self-test harness */
#include <stitch/selftest.h>
#include <stitch/console.h>

static int g_total = 0;
static int g_failed = 0;

void st_run(const char *name, bool pass)
{
    g_total++;
    if (!pass)
        g_failed++;
    console_printf("[selftest][%s] %s\n", (uint64_t)(uintptr_t)name,
                   pass ? (uint64_t)(uintptr_t)"PASS" : (uint64_t)(uintptr_t)"FAIL");
}

int st_failures(void) { return g_failed; }
int st_total(void)    { return g_total; }
