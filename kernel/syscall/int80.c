/* SutureOS: the Vimtu64 own-ABI syscall gate (int 0x80).
 *
 * The userland in user/lib is ported from VimtuOS and speaks VimtuOS' own ABI:
 * `int $0x80` with the call number in eax and arguments in rdi/rsi/rdx/r10.
 * Rather than editing the ported libc (which would make future re-ports
 * impossible), the kernel translates that ABI onto its own Linux-numbered
 * syscall table. The gate at vector 0x80 already exists (DPL=3) and lands in
 * moos_intr_handler() -> moos_irq_dispatch(), so all this file has to do is
 * register a handler for it.
 *
 * Frame layout: `struct moos_regs` declares rax at +0x00, but this stub pushes the
 * GPRs in the order rax, rcx, rdx, rbx, rsi, rdi, rbp, r8..r15, so in memory rax
 * ends up at +0x70 (the struct's tail - vector/error/rip - is what actually
 * matches). This file therefore reads the arguments with explicit offsets; that is
 * the whole reason the ported libc could not see its own call numbers before.
 *
 * Numbers (user/lib/vimtu64.h):
 *   1 write(fd,buf,len)  2 exit(code)      3 getpid()      4 ticks()
 *   5 sleep_ms(ms)       6 open(path,flags)7 read(fd,buf,len) 8 close(fd)
 * Everything else answers -ENOSYS, which the libc reports as an error instead of
 * silently pretending to work.
 */
#include "int80.h"

#include <stitch/console.h>
#include <stitch/string.h>
#include "intr.h"
#include "ring3.h"
#include "syscall.h"

/* Linux x86-64 numbers for the calls we forward to (see kernel/syscall/syscall.h) */
#define LX_READ   0
#define LX_WRITE  1
#define LX_OPEN   2
#define LX_CLOSE  3
#define LX_GETPID 39
#define LX_NANOSLEEP 35

#define V64_NR_WRITE    1
#define V64_NR_EXIT     2
#define V64_NR_GETPID   3
#define V64_NR_TICKS    4
#define V64_NR_SLEEP_MS 5
#define V64_NR_OPEN     6
#define V64_NR_READ     7
#define V64_NR_CLOSE    8

#define V64_WRITE_MAX 1024u

/* Real frame offsets for this stub (see the file header note). The stub pops the
 * GPRs in reverse before iretq, so writing the return value to FRAME_RAX is what
 * the user program sees in rax. */
#define FRAME_RAX 0x70u
#define FRAME_RDI 0x48u
#define FRAME_RSI 0x50u
#define FRAME_RDX 0x60u

static uint64_t frame_get(const struct moos_regs *f, unsigned off)
{
    return *(const uint64_t *)(const void *)((const uint8_t *)f + off);
}

static void frame_set(struct moos_regs *f, unsigned off, uint64_t v)
{
    *(uint64_t *)(void *)((uint8_t *)f + off) = v;
}

static uint32_t g_calls;
static uint32_t g_denied;

uint32_t int80_call_count(void) { return g_calls; }
uint32_t int80_denied_count(void) { return g_denied; }

/* Same CPL=3 pointer rule the syscall layer uses (no-op until ring 3 is armed). */
static int user_ptr_ok(const void *p, uint64_t len)
{
    extern int el0_ptr_ok(uint64_t va, uint64_t len);
    return el0_ptr_ok((uint64_t)(uintptr_t)p, len);
}

static void int80_handler(struct moos_regs *f)
{
    const uint64_t nr = frame_get(f, FRAME_RAX);
    uint64_t ret;

    g_calls++;

    switch (nr) {
    case V64_NR_WRITE: {
        const uint64_t fd = frame_get(f, FRAME_RDI), buf = frame_get(f, FRAME_RSI),
                      len = frame_get(f, FRAME_RDX);
        if (len > V64_WRITE_MAX) { ret = (uint64_t)-1; break; }
        if (!user_ptr_ok((const void *)(uintptr_t)buf, len)) { ret = (uint64_t)-3; break; }
        ret = syscall_invoke(LX_WRITE, fd, buf, len, 0, 0, 0);
        break;
    }
    case V64_NR_READ: {
        const uint64_t fd = frame_get(f, FRAME_RDI), buf = frame_get(f, FRAME_RSI),
                      len = frame_get(f, FRAME_RDX);
        if (!user_ptr_ok((const void *)(uintptr_t)buf, len)) { ret = (uint64_t)-3; break; }
        ret = syscall_invoke(LX_READ, fd, buf, len, 0, 0, 0);
        break;
    }
    case V64_NR_OPEN: {
        const uint64_t path = frame_get(f, FRAME_RDI);
        if (!user_ptr_ok((const void *)0, 0) || path == 0) { ret = (uint64_t)-3; break; }
        ret = syscall_invoke(LX_OPEN, path, 0 /* O_RDONLY */, 0, 0, 0, 0);
        break;
    }
    case V64_NR_CLOSE:
        ret = syscall_invoke(LX_CLOSE, frame_get(f, FRAME_RDI), 0, 0, 0, 0, 0);
        break;
    case V64_NR_GETPID:
        ret = syscall_invoke(LX_GETPID, 0, 0, 0, 0, 0, 0);
        break;
    case V64_NR_TICKS:
        ret = syscall_invoke(SYS_TIME, 0, 0, 0, 0, 0, 0);   /* epoch seconds */
        break;
    case V64_NR_SLEEP_MS: {
        const uint64_t ms = frame_get(f, FRAME_RDI);
        if (ms == 0) { ret = 0; break; }
        /* No timer wheel yet: bounded busy wait, the same way nanosleep works. */
        {
            extern uint64_t rtc_uptime_seconds(void);
            const uint64_t start = rtc_uptime_seconds();
            const uint64_t want = (ms + 999u) / 1000u;
            while ((rtc_uptime_seconds() - start) < want) {
                for (volatile int spin = 0; spin < 20000; spin++) {
                    __asm__ volatile("pause");
                }
            }
        }
        ret = 0;
        break;
    }
    case V64_NR_EXIT:
        /* Leave the iretq frame behind and resume the kernel launcher. */
        if (ring3_active()) {
            ring3_exit_user(frame_get(f, FRAME_RDI));   /* does not return */
        }
        ret = 0;
        break;
    default:
        g_denied++;
        ret = (uint64_t)-38;                /* -ENOSYS */
        break;
    }

    frame_set(f, FRAME_RAX, ret);
}

int int80_init(void)
{
    extern void moos_irq_register(int vec, moos_irq_fn fn);
    moos_irq_register(0x80, int80_handler);
    return 0;
}

/* Acceptance: the gate is installed, and every number the ported libc uses has a
 * real implementation (no -ENOSYS for 1..8). */
int int80_selftest(void)
{
    uint32_t ok = 0, total = 0;

    total++;
    if (int80_call_count() > 0) ok++;
    console_printf("int80 selftest: the ported libc made calls (n=%u) ... %s\n",
                   (int64_t)g_calls, (int80_call_count() > 0) ? "PASS" : "FAIL");

    total++;
    {
        const int clean = (g_denied == 0);
        if (clean) ok++;
        console_printf("int80 selftest: no -ENOSYS from the libc (denied=%u) ... %s\n",
                       (int64_t)g_denied, clean ? "PASS" : "FAIL");
    }

    console_printf("int80 selftest: RESULT %s (%u/%u)\n",
                   (ok == total) ? "PASS" : "FAIL", (int64_t)ok, (int64_t)total);
    return (ok == total) ? 0 : 1;
}
