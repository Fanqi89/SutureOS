/* StitchOS module: interrupt dispatcher + IRQ handler registry (moos_ prefix)
 * Ported from: MOOS (C:\Users\fanqi\Desktop\自研操作系统\MOOS\Kernel\Misc\IDT.cs
 *              intr_handler split/dispatch logic,
 *              C:\Users\fanqi\Desktop\自研操作系统\MOOS\Kernel\Misc\Interrupts.cs
 *              IRQ handler registry)
 * Original license: Unlicense (MOOS LICENSE = public-domain dedication; MIT also cited)
 * Changes: C# Panic/Console/framebuffer paths replaced by bookkeeping that is
 *          safe while interrupts are permanently masked: last-vector + per-
 *          vector counters, a warning (never a panic) for unexpected CPU
 *          exceptions, and a 256-slot handler table instead of MOOS's
 *          List-of-delegates. int 0x80 named-syscall lookup dropped (phase-2).
 */
#include "intr.h"

#include <stitch/console.h>
#include <stitch/selftest.h>
#include <stitch/string.h>

/* ---- statistics ---- */
static uint32_t g_counts[256];
static uint64_t g_last_vector;

/* ---- handler registry (MOOS Interrupts.cs delegate table -> array) ---- */
static moos_irq_fn g_table[256];

/* Vectors tolerated without a warning during bring-up: #DB (1) and #BP (3)
 * may legitimately arrive from a debugger, and anything with a driver
 * handler registered is by definition expected. Everything else below 32 is
 * an unexpected CPU exception: print it, keep running (the kernel cli's, so
 * no real exception should ever get this far in this phase). */
static bool vector_expected(uint64_t vec)
{
    if (vec < 256u && g_table[vec] != NULL)
        return true;
    if (vec == 1u || vec == 3u)
        return true;
    return false;
}

/* Called from every isr_stubs.S stub (SysV: rcx-less, arg1 in rdi). */
void moos_intr_handler(uint64_t vec, struct moos_regs *frame)
{
    if (vec < 256u)
        g_counts[vec]++;
    g_last_vector = vec;

    if (vec < 32u && !vector_expected(vec)) {
        /* MOOS IDT.cs panicked here; bring-up must not die on a stray
         * exception, so only report (numeric args go as uint64_t). */
        console_printf("[moos] unexpected cpu exception vector=%u err=0x%x (no panic: interrupts masked in this phase)\n",
                       (uint64_t)vec,
                       frame ? (uint64_t)frame->error_code : 0u);
    }

    if (vec < 256u && g_table[vec] != NULL)
        g_table[vec](frame);        /* MOOS Interrupts.HandleInterrupt equivalent */
}

/* Registry: one slot per vector; registering again overwrites (documented
 * behaviour - MOOS appended to a List and fired every match; the fixed table
 * makes "replace the handler" explicit). vec out of range is ignored. */
void moos_irq_register(int vec, moos_irq_fn fn)
{
    if (vec < 0 || vec > 255)
        return;
    g_table[vec] = fn;
}

void moos_irq_dispatch(int vec, struct moos_regs *frame)
{
    if (vec < 0 || vec > 255)
        return;
    if (g_table[vec] != NULL)
        g_table[vec](frame);
}

moos_irq_fn moos_irq_get(int vec)
{
    if (vec < 0 || vec > 255)
        return NULL;
    return g_table[vec];
}

uint64_t moos_intr_last_vector(void)
{
    return g_last_vector;
}

uint32_t moos_intr_count(int vec)
{
    if (vec < 0 || vec > 255)
        return 0;
    return g_counts[vec];
}

void moos_intr_reset_stats(void)
{
    memset(g_counts, 0, sizeof(g_counts));
    g_last_vector = 0;
}

/* ==================== self test ==================== */
/* Synthetic frames only - never waits for a real interrupt (kernel is cli). */
static int g_h1_calls;
static int g_h2_calls;
static uint64_t g_h1_vec;
static struct moos_regs *g_h1_frame;

static void test_handler_a(struct moos_regs *r)
{
    g_h1_calls++;
    g_h1_vec = r->vector;
    g_h1_frame = r;
}

static void test_handler_b(struct moos_regs *r)
{
    (void)r;
    g_h2_calls++;
}

void st_moos_intr_test(void)
{
    bool ok = true;
    struct moos_regs frame;
    uint32_t before;

    memset(&frame, 0, sizeof(frame));

    /* 1) registry starts empty for all 256 slots */
    for (int i = 0; i < 256; i++)
        ok = ok && (moos_irq_get(i) == NULL);

    /* 2) register + dispatch: handler runs once, sees the right vector and
     *    the very frame we passed in */
    frame.vector = 40;
    moos_irq_register(40, test_handler_a);
    g_h1_calls = 0;
    g_h1_vec = 0xFFFFull;
    g_h1_frame = NULL;
    moos_irq_dispatch(40, &frame);
    ok = ok && (g_h1_calls == 1);
    ok = ok && (g_h1_vec == 40);
    ok = ok && (g_h1_frame == &frame);
    ok = ok && (moos_irq_get(40) == test_handler_a);

    /* 3) duplicate registration overwrites (only B runs afterwards) */
    moos_irq_register(40, test_handler_b);
    g_h1_calls = 0;
    g_h2_calls = 0;
    moos_irq_dispatch(40, &frame);
    ok = ok && (g_h2_calls == 1);
    ok = ok && (g_h1_calls == 0);
    ok = ok && (moos_irq_get(40) == test_handler_b);

    /*    NULL registration clears the slot */
    moos_irq_register(40, NULL);
    g_h2_calls = 0;
    moos_irq_dispatch(40, &frame);
    ok = ok && (g_h2_calls == 0);
    ok = ok && (moos_irq_get(40) == NULL);

    /*    out-of-range register/dispatch/get are ignored, no crash */
    moos_irq_register(-1, test_handler_a);
    moos_irq_register(256, test_handler_a);
    moos_irq_dispatch(-1, &frame);
    moos_irq_dispatch(256, &frame);
    ok = ok && (moos_irq_get(-1) == NULL);
    ok = ok && (moos_irq_get(256) == NULL);

    /* 4) moos_intr_handler: counts the vector, records it, does not die.
     *    vec 200 is >= 32 => no warning path. Registered handler (if any)
     *    also fires, which is the MOOS HandleInterrupt behaviour. */
    before = moos_intr_count(200);
    frame.vector = 200;
    moos_intr_handler(200, &frame);
    ok = ok && (moos_intr_count(200) == before + 1u);
    ok = ok && (moos_intr_last_vector() == 200);
    moos_intr_handler(200, &frame);
    ok = ok && (moos_intr_count(200) == before + 2u);

    /*    unexpected CPU exception (vec 13): warning only, count still taken,
     *    no panic - exercising that path is the point of this assertion. */
    before = moos_intr_count(13);
    frame.vector = 13;
    frame.error_code = 0xBADull;
    moos_intr_handler(13, &frame);
    ok = ok && (moos_intr_count(13) == before + 1u);
    ok = ok && (moos_intr_last_vector() == 13);

    /*    a registered handler is invoked by moos_intr_handler as well */
    frame.vector = 41;
    moos_irq_register(41, test_handler_a);
    g_h1_calls = 0;
    g_h1_vec = 0;
    moos_intr_handler(41, &frame);
    ok = ok && (g_h1_calls == 1);
    ok = ok && (g_h1_vec == 41);
    moos_irq_register(41, NULL);

    st_run("moos_intr", ok);
}
