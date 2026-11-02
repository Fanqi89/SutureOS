/* rtouch.c - create empty files from ring 3 (the ring-3 half of `touch`).
 *
 * Usage: rtouch <file> [file...]
 *
 * touch(1) has two meanings: create a missing file, and update the mtime of an
 * existing one. This kernel's VFS does not carry a settable mtime, so the tool
 * does the first and says clearly what it did with the second: an existing file is
 * opened and left byte-for-byte alone (it is NOT truncated) and the output states
 * that the timestamp was not updated. Nothing pretends to be more than it is.
 *
 * License: GPL-3.0, part of SutureOS (see user/apps/r3tool.h).
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "r3tool.h"

static int touch_one(const char *path)
{
    struct stat st;
    long r;

    r = r3_stat(path, &st);
    if (r == 0) {
        const long fd = r3_open(path, O_WRONLY, 0);
        if (fd < 0) {
            r3_fail("rtouch", path, fd);
            return 1;
        }
        (void)r3_close((int)fd);
        printf("rtouch: %s: already exists, left untouched "
               "(this VFS cannot set mtime)\n", path);
        return 0;
    }
    /* O_CREAT without O_TRUNC: creating is all this does. */
    r = r3_open(path, O_WRONLY | O_CREAT, 0644);
    if (r < 0) {
        r3_fail("rtouch", path, r);
        return 1;
    }
    (void)r3_close((int)r);
    (void)r3_sync();
    printf("rtouch: created %s\n", path);
    return 0;
}

int main(int argc, char **argv)
{
    int failed = 0;
    int i;

    if (argc < 2) {
        printf("usage: rtouch <file> [file...]\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (touch_one(argv[i]) != 0) failed = 1;
    }
    return failed;
}
