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

/* ---- B5 file tools ---------------------------------------------------------
 * Each tool is its own built-in blob (user/apps/r*.c), so it has to run as a
 * *separate process* and not by execve(): execve replaces this shell's image and
 * the prompt would be gone after one command. fork() gives us a copy-on-write
 * child; execve() (syscall 59, whose path may carry the arguments - the kernel
 * builds the child's argc/argv frame) replaces only the child; wait4() collects
 * its exit status and the shell keeps running. */
static const char *const g_tools[] = {
    "rls", "rcat", "rcp", "rmv", "rrm", "rmkdir", "rtouch", "rdf", NULL
};

/* "rls /mnt/fat" -> "rls"; "rlsx" -> NULL. */
static const char *match_tool(const char *line)
{
    int i;

    for (i = 0; g_tools[i] != NULL; i++) {
        const size_t n = strlen(g_tools[i]);
        if (strncmp(line, g_tools[i], n) == 0 &&
            (line[n] == '\0' || line[n] == ' ')) {
            return g_tools[i];
        }
    }
    return NULL;
}

/* Run `line` (tool name + arguments) in a child process. Returns the tool's exit
 * code, or -1 when the child could not be started or reaped. */
static int run_tool(const char *tool, const char *line)
{
    extern long __v64_syscall(long nr, long a1, long a2, long a3, long a4, long a5);
    const int cpid = fork();

    if (cpid == 0) {
        const long r = __v64_syscall(59, (long)line, 0, 0, 0, 0);
        printf("ushell: exec '%s' failed (rc=%ld)\n", tool, r);
        _exit(127);
    }
    if (cpid < 0) {
        printf("ushell: fork failed (%d)\n", cpid);
        return -1;
    }
    {
        int st = -1;
        const int w = wait4(cpid, &st, 0, 0);
        if (w < 0) {
            printf("ushell: wait4(%d) failed (%d)\n", cpid, w);
            return -1;
        }
        return st & 0xFF;
    }
}

int main(int argc, char **argv)
{
    char line[128];

    printf("SutureOS ring-3 shell (pid=%d, ppid=%d, argc=%d)\n",
           getpid(), getppid(), argc);
    printf("commands: help hello probe fork crash pid ticks root exec exit\n");
    printf("tools   : rls rcat rcp rmv rrm rmkdir rtouch rdf (e.g. 'rls /mnt/fat')\n");

    for (;;) {
        printf("user$ ");
        if (read_line(line, (int)sizeof(line)) <= 0) continue;
        trim(line);

        if (line[0] == '\0') continue;
        {
            const char *tool = match_tool(line);
            if (tool != NULL) {
                const int rc = run_tool(tool, line);
                if (rc > 0) printf("[ushell] %s exited with %d\n", tool, rc);
                continue;
            }
        }
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            puts("bye");
            return 0;
        }
        if (strcmp(line, "help") == 0) {
            puts("help   - this list");
            puts("hello  - execve the libc hello program");
            puts("probe  - execve the staged acceptance program");
            puts("fork   - fork a second process and wait4() for it");
            puts("crash  - write to kernel memory (must kill only this process)");
            puts("pid    - getpid()/getppid()");
            puts("ticks  - uptime ticks");
            puts("root   - write to kernel memory (same as crash)");
            puts("exec   - execve the ring-3 shell again");
            puts("exit   - leave the shell");
            puts("--- ring-3 file tools (run in a child process, arguments allowed) ---");
            puts("rls [path]              - list a directory ('name/' for directories)");
            puts("rcat <file>...          - print file contents");
            puts("rcp <src> <dst>         - copy a file");
            puts("rmv <src> <dst>         - rename inside one directory");
            puts("rrm [-r] <path>...      - remove files / empty directories");
            puts("rmkdir [-p] <path>...   - create directories");
            puts("rtouch <file>...        - create empty files");
            puts("rdf [path]              - free/total space of a mount");
            continue;
        }
        if (strcmp(line, "fork") == 0) {
            const int cpid = fork();
            if (cpid == 0) {
                /* The child is a real second process: its own address space (COW
                 * from here), its own pid, this shell as its parent. It must leave
                 * immediately, otherwise two shells would read the console. */
                printf("[shell] child: pid=%d ppid=%d, exiting with 7\n",
                       getpid(), getppid());
                _exit(7);
            }
            if (cpid < 0) {
                printf("fork failed: %d\n", cpid);
                continue;
            }
            {
                int st = -1;
                const int w = wait4(cpid, &st, 0, 0);
                printf("[shell] waited: wait4=%d status=0x%x (exit code %d)\n",
                       w, st, st & 0xFF);
            }
            continue;
        }
        if (strcmp(line, "hello") == 0) { do_exec("hello"); continue; }
        if (strcmp(line, "probe") == 0) { do_exec("probe"); continue; }
        if (strcmp(line, "exec") == 0 || strcmp(line, "shell") == 0) {
            do_exec("shell");
            continue;
        }
        if (strcmp(line, "pid") == 0) {
            printf("pid=%d ppid=%d\n", getpid(), getppid());
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
