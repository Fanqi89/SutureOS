/* rrm.c - remove files and empty directories from ring 3 (the ring-3 half of `rm`).
 *
 * Usage: rrm <path> [path...]
 *        rrm -r ...   -> refused: recursive delete is not implemented
 *
 * A plain file goes through unlink(2), a directory through rmdir(2) (which only
 * succeeds on an empty one). The kind is decided with stat() first so the error
 * message names the operation that actually failed. Recursive delete is *not*
 * silently approximated: -r is reported as unsupported and nothing is removed.
 *
 * License: GPL-3.0, part of SutureOS (see user/apps/r3tool.h).
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "r3tool.h"

static int remove_one(const char *path)
{
    struct stat st;
    long r;

    r = r3_stat(path, &st);
    if (r < 0) {
        r3_fail("rrm", path, r);
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        r = r3_rmdir(path);
        (void)r3_sync();
        if (r < 0) {
            if (-r == 39 || -r == 17) {
                printf("rrm: %s: directory is not empty (errno=%ld %s)\n",
                       path, -r, r3_errname(-r));
            } else {
                r3_fail("rrm", path, r);
            }
            return 1;
        }
        printf("rrm: removed directory %s\n", path);
        return 0;
    }
    r = r3_unlink(path);
    (void)r3_sync();
    if (r < 0) {
        r3_fail("rrm", path, r);
        return 1;
    }
    printf("rrm: removed file %s\n", path);
    return 0;
}

int main(int argc, char **argv)
{
    int failed = 0;
    int i;

    if (argc < 2) {
        printf("usage: rrm [-r] <path> [path...]\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "-rf") == 0 ||
            strcmp(argv[i], "-fr") == 0) {
            printf("rrm: %s: recursive delete is not supported by this VFS; "
                   "nothing was removed\n", argv[i]);
            return 1;
        }
    }
    for (i = 1; i < argc; i++) {
        if (remove_one(argv[i]) != 0) failed = 1;
    }
    return failed;
}
