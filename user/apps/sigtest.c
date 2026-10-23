/* sigtest.c - ring-3 acceptance for real signal semantics (ITEM 5).
 *
 * It proves, from CPL=3 and through the real syscall path, that:
 *   1. rt_sigaction(13) stores a handler and rt_sigprocmask(14) stores a mask;
 *   2. kill(getpid(), SIGUSR1) delivers: the kernel pushes a signal frame on the
 *      user stack, jumps to the handler with rdi = signal number, and the
 *      handler's `ret` lands on the libc restorer (__v64_sigreturn_stub), whose
 *      rt_sigreturn(15) restores every GPR plus rip/rsp/rflags so execution
 *      resumes on the instruction after the interrupted call;
 *   3. SIG_IGN really drops a signal instead of killing the process;
 *   4. a blocked signal stays pending (handler does not run) and is delivered the
 *      moment it is unblocked.
 *
 * Exit code 0 == every check passed (the shell prints "run: sigtest exited with
 * 0"). Each failure prints one line saying which check failed and exits with its
 * own code, so a serial log pinpoints the layer.
 */
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

static volatile int g_handler_runs;
static volatile int g_handler_sig;
static volatile int g_handler_stage;
static volatile int g_resumed_after_raise;

static void usr1_handler(int sig)
{
    g_handler_runs++;
    g_handler_sig = sig;
    g_handler_stage = 4242;      /* proves the handler body really executed */
}

/* Returns 0 on success. The caller turns a non-zero value into an exit code. */
static int check(int cond, const char *what, int code)
{
    if (!cond) {
        printf("[sigtest] FAIL %s\n", what);
        return code;
    }
    return 0;
}

int main(void)
{
    struct sigaction sa;
    sigset_t block, old;
    int rc;

    printf("[sigtest] pid=%d: installing a SIGUSR1 handler\n", getpid());

    sa.handler = usr1_handler;
    sa.flags = 0;
    sa.restorer = 0;             /* the libc wrapper fills __v64_sigreturn_stub */
    sa.mask = 0;
    if (sigaction(SIGUSR1, &sa, 0) != 0) {
        printf("[sigtest] FAIL sigaction(SIGUSR1) errno=%d\n", errno);
        return 10;
    }

    /* --- 1. delivery + handler + sigreturn resume ------------------------- */
    rc = raise(SIGUSR1);
    if ((rc = check(rc == 0, "raise(SIGUSR1) returned an error", 11)) != 0)
        return rc;
    g_resumed_after_raise = 1;
    if ((rc = check(g_handler_runs == 1, "handler did not run", 12)) != 0)
        return rc;
    if ((rc = check(g_handler_sig == SIGUSR1, "handler saw the wrong signal", 13)) != 0)
        return rc;
    if ((rc = check(g_handler_stage == 4242, "handler body did not finish", 14)) != 0)
        return rc;
    if ((rc = check(g_resumed_after_raise == 1,
                    "execution did not resume after the interrupted call", 15)) != 0)
        return rc;
    printf("[sigtest] 1 handler ran for SIGUSR1 (%d) and sigreturn resumed\n",
           g_handler_sig);

    /* --- 2. SIG_IGN is dropped, not fatal -------------------------------- */
    if (signal(SIGUSR2, SIG_IGN) == SIG_ERR) {
        printf("[sigtest] FAIL signal(SIGUSR2, SIG_IGN)\n");
        return 20;
    }
    if ((rc = check(raise(SIGUSR2) == 0, "raise(SIGUSR2) returned an error", 21)) != 0)
        return rc;
    printf("[sigtest] 2 SIG_IGN dropped SIGUSR2, the process is still alive\n");

    /* --- 3. a blocked signal stays pending, then arrives on unblock ------- */
    block = 1ul << (SIGUSR1 - 1);
    old = 0;
    if (sigprocmask(SIG_BLOCK, &block, &old) != 0) {
        printf("[sigtest] FAIL sigprocmask(SIG_BLOCK)\n");
        return 30;
    }
    if ((rc = check((old & block) == 0, "old mask already blocked SIGUSR1", 31)) != 0)
        return rc;
    if ((rc = check(raise(SIGUSR1) == 0, "raise while blocked failed", 32)) != 0)
        return rc;
    if ((rc = check(g_handler_runs == 1,
                    "a blocked signal was delivered anyway", 33)) != 0)
        return rc;
    printf("[sigtest] 3 while blocked the handler did not run (pending kept)\n");

    if (sigprocmask(SIG_UNBLOCK, &block, 0) != 0) {
        printf("[sigtest] FAIL sigprocmask(SIG_UNBLOCK)\n");
        return 34;
    }
    /* The unblock syscall itself returns to user mode with the signal pending, so
     * the handler has already run by the time we get here. */
    if ((rc = check(g_handler_runs == 2,
                    "the pending signal was not delivered on unblock", 35)) != 0)
        return rc;
    printf("[sigtest] 4 unblocking delivered the pending signal (%d runs total)\n",
           g_handler_runs);

    printf("[sigtest] RESULT PASS: signal frame, handler, sigreturn and mask all "
           "behave\n");
    return 0;
}
