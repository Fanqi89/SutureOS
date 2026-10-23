/* probe.c - SutureOS ring-3 acceptance program for the ported VimtuOS libc.
 *
 * It walks the runtime in stages and prints after each one, so a failure points
 * at exactly one layer instead of "the program printed nothing":
 *   1. raw int 0x80 write (own ABI, no libc)
 *   2. puts()      -> stdio line buffer -> write(1)
 *   3. printf()    -> stdio formatting (%d/%s/%lu)
 *   4. malloc()/free() -> stdlib free-list arena + string.h
 *   5. getpid()/vimtu64_ticks() -> more own-ABI calls
 *   6. exit code 7 back to the kernel
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* the own-ABI trampoline from user/lib/syscall.S */
extern long __v64_int80(long nr, long a1, long a2, long a3, long a4);

static void raw_write(const char *s, unsigned n)
{
    (void)__v64_int80(1 /* V64_NR_WRITE */, 1, (long)(const void *)s, (long)n, 0);
}

int main(int argc, char **argv)
{
    static const char stage1[] = "[probe] 1 int80 write ok\n";
    char *heap;
    int i;

    raw_write(stage1, sizeof(stage1) - 1);

    puts("[probe] 2 puts ok");
    printf("[probe] 3 printf ok argc=%d argv0=%s\n", argc,
           (argc > 0 && argv && argv[0]) ? argv[0] : "(null)");

    heap = (char *)malloc(256);
    if (heap == NULL) {
        printf("[probe] 4 malloc FAILED\n");
        return 3;
    }
    for (i = 0; i < 32; i++) heap[i] = (char)('a' + (i % 26));
    heap[32] = '\0';
    printf("[probe] 4 malloc ok len=%lu str=%s\n", (unsigned long)strlen(heap), heap);
    free(heap);

    printf("[probe] 5 pid=%d ticks=%lu\n", getpid(), vimtu64_ticks());
    printf("[probe] 6 exiting with 7\n");
    return 7;
}
