/* shell.c - the ring-3 user shell.
 *
 * This is the "usable userspace" milestone: it runs at CPL=3 on the ported libc,
 * reads commands from the console through `int 0x80` read(fd=0) (which the kernel
 * routes to the keyboard ring), and can replace its own image with another program
 * through execve (syscall 59), which the kernel implements by rebuilding the
 * current address space and rewriting the syscall return frame.
 *
 * It also has a deliberate fault command: `crash` writes to kernel memory, which
 * must kill only this process (exit code 128+SIGSEGV) and leave the kernel alive.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Linux-numbered syscall trampoline from user/lib/syscall.S */
extern long __v64_syscall(long nr, long a1, long a2, long a3, long a4, long a5);

#define SYS_EXECVE 59

/* read(0,...) delivers whatever is available (often a single character), so keep
 * reading until the kernel echoes a newline. */
static int read_line(char *buf, int cap)
{
    int got = 0;
    while (got < cap - 1) {
        const int n = (int)read(0, buf + got, 1);
        if (n <= 0) continue;              /* -EAGAIN: nothing typed yet */
        if (buf[got] == '\n') {
            buf[got] = '\0';
            return got;
        }
        got++;
    }
    buf[got] = '\0';
    return got;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) {
        s[--n] = '\0';
    }
}

static void do_exec(const char *name)
{
    /* own-ABI number 9 = execve; the kernel rewrites our interrupt frame */
    extern long __v64_int80(long nr, long a1, long a2, long a3, long a4);
    const long r = __v64_int80(9, (long)name, 0, 0, 0);
    printf("exec '%s' failed: %ld\n", name, r);
}

int main(int argc, char **argv)
{
    char line[128];

    printf("SutureOS ring-3 shell (pid=%d, argc=%d)\n", getpid(), argc);
    printf("commands: help hello probe crash pid ticks root exec exit\n");

    for (;;) {
        printf("user$ ");
        if (read_line(line, (int)sizeof(line)) <= 0) continue;
        trim(line);

        if (line[0] == '\0') continue;
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            puts("bye");
            return 0;
        }
        if (strcmp(line, "help") == 0) {
            puts("help   - this list");
            puts("hello  - execve the libc hello program");
            puts("probe  - execve the staged acceptance program");
            puts("crash  - write to kernel memory (must kill only this process)");
            puts("pid    - getpid()");
            puts("ticks  - uptime ticks");
            puts("root   - write to kernel memory (same as crash)");
            puts("exec   - execve the ring-3 shell again");
            puts("exit   - leave the shell");
            continue;
        }
        if (strcmp(line, "hello") == 0) { do_exec("hello"); continue; }
        if (strcmp(line, "probe") == 0) { do_exec("probe"); continue; }
        if (strcmp(line, "exec") == 0 || strcmp(line, "shell") == 0) {
            do_exec("shell");
            continue;
        }
        if (strcmp(line, "pid") == 0) {
            printf("pid=%d\n", getpid());
            continue;
        }
        if (strcmp(line, "ticks") == 0) {
            printf("ticks=%lu\n", vimtu64_ticks());
            continue;
        }
        if (strcmp(line, "crash") == 0 || strcmp(line, "root") == 0) {
            puts("writing to kernel memory at 0x100000 ...");
            {
                volatile unsigned char *kernel = (volatile unsigned char *)0x100000;
                *kernel = 0x41;
            }
            puts("NOT KILLED - isolation is broken");
            continue;
        }
        printf("%s: unknown command (try 'help')\n", line);
    }
}
