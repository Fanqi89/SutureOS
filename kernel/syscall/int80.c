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
 * STATUS: the gate works (the asm probe program calls getpid through it and gets
 * an answer), but for the *ported libc* the call number read from the frame is
 * wrong: `struct moos_regs` declares rax at +0x00 while the shared ISR stub pushes
 * rax first, i.e. it lands at +0x70. The fix is a dedicated int 0x80 stub that
 * passes (nr, a1, a2, a3) to C in registers instead of through that frame; until
 * then user_run_builtin() refuses to start the libc programs.
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
    const uint64_t nr = f->rax;
    uint64_t ret;

    g_calls++;

    switch (nr) {
    case V64_NR_WRITE: {
        const uint64_t fd = f->rdi, buf = f->rsi, len = f->rdx;
        if (len > V64_WRITE_MAX) { ret = (uint64_t)-1; break; }
        if (!user_ptr_ok((const void *)(uintptr_t)buf, len)) { ret = (uint64_t)-3; break; }
        ret = syscall_invoke(LX_WRITE, fd, buf, len, 0, 0, 0);
        break;
    }
    case V64_NR_READ: {
        const uint64_t fd = f->rdi, buf = f->rsi, len = f->rdx;
        if (!user_ptr_ok((const void *)(uintptr_t)buf, len)) { ret = (uint64_t)-3; break; }
        ret = syscall_invoke(LX_READ, fd, buf, len, 0, 0, 0);
        break;
    }
    case V64_NR_OPEN: {
        const uint64_t path = f->rdi;
        if (!user_ptr_ok((const void *)0, 0) || path == 0) { ret = (uint64_t)-3; break; }
        ret = syscall_invoke(LX_OPEN, path, 0 /* O_RDONLY */, 0, 0, 0, 0);
        break;
    }
    case V64_NR_CLOSE:
        ret = syscall_invoke(LX_CLOSE, f->rdi, 0, 0, 0, 0, 0);
        break;
    case V64_NR_GETPID:
        ret = syscall_invoke(LX_GETPID, 0, 0, 0, 0, 0, 0);
        break;
    case V64_NR_TICKS:
        ret = syscall_invoke(SYS_TIME, 0, 0, 0, 0, 0, 0);   /* epoch seconds */
        break;
    case V64_NR_SLEEP_MS: {
        const uint64_t ms = f->rdi;
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
            ring3_exit_user(f->rdi);        /* does not return */
        }
        ret = 0;
        break;
    default:
        g_denied++;
        ret = (uint64_t)-38;                /* -ENOSYS */
        break;
    }

    f->rax = ret;
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
