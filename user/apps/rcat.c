/* rcat.c - print file contents from ring 3 (the ring-3 half of `cat`).
 *
 * Usage: rcat <file> [file...]
 *
 * Reads each file through the Linux-numbered syscalls (open/read/close) and
 * writes the bytes to stdout (fd 1, the console). A failure on one file does not
 * stop the others, and every failure prints the real -errno. The exit code is 1
 * when at least one argument failed, 0 only when every file was printed.
 *
 * License: GPL-3.0, part of SutureOS (see user/apps/r3tool.h).
 */
#include <stdio.h>
#include <string.h>

#include "r3tool.h"

#define RCAT_BUF 1024

static int cat_one(const char *path)
{
    char buf[RCAT_BUF];
    long fd;
    long n;

    fd = r3_open(path, O_RDONLY, 0);
    if (fd < 0) {
        r3_fail("rcat", path, fd);
        return 1;
    }
    for (;;) {
        long off = 0;

        n = r3_read((int)fd, buf, sizeof(buf));
        if (n < 0) {
            r3_fail("rcat", path, n);
            (void)r3_close((int)fd);
            return 1;
        }
        if (n == 0) break;
        while (off < n) {
            const long w = r3_write(1, buf + off, (unsigned long)(n - off));
            if (w <= 0) {
                r3_fail("rcat", "write to stdout", (w < 0) ? w : -5);
                (void)r3_close((int)fd);
                return 1;
            }
            off += w;
        }
    }
    (void)r3_close((int)fd);
    return 0;
}

int main(int argc, char **argv)
{
    int failed = 0;
    int i;

    if (argc < 2) {
        printf("usage: rcat <file> [file...]\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (cat_one(argv[i]) != 0) failed = 1;
    }
    return failed;
}
