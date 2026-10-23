/* forktest.c - SutureOS ring-3 acceptance for real multi-process support.
 *
 * Run it from the kernel shell with `run forktest`. It proves the whole chain:
 *
 *   1. getpid/getppid describe the parent before the fork.
 *   2. fork() returns the child's pid in the parent and 0 in the child, and the
 *      child really is a separate process (different pid, parent's pid as ppid).
 *   3. A page that fork() shared copy-on-write diverges: the child writes 'C' into
 *      it (that write is what takes the COW #PF and copies the 4 KiB frame), the
 *      parent still reads 'P' afterwards.
 *   4. The child exits with 42; the parent's wait4() returns the child's pid and a
 *      status whose low byte is 42 (the kernel encodes status as sig<<8 | code).
 *   5. A second child exits with 7 and WNOHANG before it is ready answers 0.
 *
 * Exit code 0 means every check passed; each failure prints the reason and returns
 * the number of the failing check.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* A page-sized global that lives in the user window (.data/.bss are inside the
 * 16-page window the kernel maps for the process), so fork() shares it COW. */
static char g_shared[64];

#define WNOHANG 1

static int fail(int code, const char *what)
{
    printf("[forktest] FAIL %d: %s\n", code, what);
    return code;
}

int main(int argc, char **argv)
{
    int pid, wpid, status, rc, mypid, myppid;

    (void)argc; (void)argv;

    for (int i = 0; i < (int)sizeof(g_shared); i++) g_shared[i] = '.';
    g_shared[0] = 'P';

    mypid = getpid();
    myppid = getppid();
    printf("[forktest] parent: pid=%d ppid=%d marker=%c\n", mypid, myppid, g_shared[0]);
    if (mypid <= 0) return fail(1, "getpid() returned 0 inside a process");

    /* ---------------- 1. fork ---------------- */
    pid = fork();
    if (pid < 0) return fail(2, "fork() returned an error");

    if (pid == 0) {
        /* ---- the child: a different process, whose parent is the one above ---- */
        printf("[forktest] child: fork returned 0, pid=%d ppid=%d\n",
               getpid(), getppid());
        if (getpid() == mypid) return fail(3, "the child has the parent's pid");
        if (getppid() != mypid) return fail(4, "the child's getppid is not the parent's pid");

        /* This store is the copy-on-write test: the page is still read-only and
         * shared with the parent, so the kernel must copy the frame here. */
        g_shared[0] = 'C';
        if (g_shared[0] != 'C') return fail(5, "the child cannot write its own COW page");
        printf("[forktest] child: wrote 'C' into the shared page, exiting with 42\n");
        return 42;
    }

    /* ---- the parent ---- */
    printf("[forktest] parent: fork returned child pid %d\n", pid);
    if (pid <= 0) return fail(5, "fork() did not return a pid in the parent");
    if (pid == getpid()) return fail(6, "fork() returned the parent's own pid");

    status = -1;
    wpid = wait4(pid, &status, 0, 0);
    if (wpid != pid) return fail(7, "wait4() did not return the child's pid");
    printf("[forktest] parent: wait4 -> pid=%d status=0x%x (code=%d)\n",
           wpid, status, status & 0xFF);
    if ((status & 0xFF) != 42) return fail(8, "wait4 status does not carry the exit code 42");
    if (((status >> 8) & 0xFF) != 0) return fail(9, "a normal exit must not report a signal");

    /* ---------------- 3. the parent's copy is untouched ---------------- */
    if (g_shared[0] != 'P')
        return fail(10, "the child's write reached the parent's page (COW is broken)");
    /* ... and the parent can still write it (the last reference became writable) */
    g_shared[0] = 'Q';
    if (g_shared[0] != 'Q') return fail(11, "the parent cannot write its own page");
    printf("[forktest] parent: its own copy still reads 'Q' after the child wrote 'C'\n");

    /* ---------------- 4. no child left: -ECHILD ---------------- */
    rc = wait4(-1, &status, WNOHANG, 0);
    if (rc != -1) return fail(12, "wait4 with no child left must fail with -ECHILD");
    printf("[forktest] parent: wait4 with no child left -> -1 (ECHILD)\n");

    /* ---------------- 5. WNOHANG with a live child ---------------- */
    pid = fork();
    if (pid < 0) return fail(14, "second fork() returned an error");
    if (pid == 0) {
        printf("[forktest] child2: pid=%d ppid=%d, exiting with 7\n", getpid(), getppid());
        return 7;
    }
    rc = wait4(pid, &status, WNOHANG, 0);
    if (rc != 0) return fail(15, "wait4(WNOHANG) must answer 0 while the child is alive");
    status = -1;
    wpid = wait4(pid, &status, 0, 0);
    if (wpid != pid) return fail(16, "wait4() did not reap the second child");
    printf("[forktest] parent: child2 reaped, status=0x%x (code=%d)\n", status, status & 0xFF);
    if ((status & 0xFF) != 7) return fail(17, "second child's exit code is not 7");

    printf("[forktest] RESULT PASS: fork/COW/wait4/getppid all behave\n");
    return 0;
}
