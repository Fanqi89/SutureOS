/* rmv.c - rename a file from ring 3 (the ring-3 half of `mv`).
 *
 * Usage: rmv <source> <destination>
 *
 * This is rename(2), not copy+delete. The VFS/FAT32 rename is honest about its
 * scope: it only moves a name inside one directory and refuses a cross-directory
 * move. A refusal prints the real -errno (EXDEV for a cross-directory move,
 * EEXIST/ENOTEMPTY for a taken target, EPERM for "not implemented") and exits 1 -
 * it never reports success for a move that did not happen.
 *
 * License: GPL-3.0, part of SutureOS (see user/apps/r3tool.h).
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "r3tool.h"

int main(int argc, char **argv)
{
    char src_dir[256];
    char dst_dir[256];
    struct stat st;
    long r;

    if (argc != 3) {
        printf("usage: rmv <source> <destination>\n");
        return 1;
    }
    if (strcmp(argv[1], argv[2]) == 0) {
        printf("rmv: %s: source and destination are the same path\n", argv[1]);
        return 1;
    }
    {
        const long sr = r3_stat(argv[1], &st);
        if (sr < 0) {
            r3_fail("rmv", argv[1], sr);
            return 1;
        }
    }

    /* Say up front when the VFS will refuse anyway (cross-directory move); the
     * kernel still makes the decision, this only turns a bare EPERM into an
     * explanation. */
    r3_dirname(src_dir, sizeof(src_dir), argv[1]);
    r3_dirname(dst_dir, sizeof(dst_dir), argv[2]);
    if (strcmp(src_dir, dst_dir) != 0) {
        printf("rmv: %s -> %s: cross-directory move is not supported by this VFS\n",
               argv[1], argv[2]);
        return 1;
    }
    if (r3_stat(argv[2], &st) == 0) {
        printf("rmv: %s: destination already exists (this VFS refuses to replace)\n",
               argv[2]);
        return 1;
    }

    r = r3_rename(argv[1], argv[2]);
    (void)r3_sync();
    if (r < 0) {
        r3_fail("rmv", argv[1], r);
        return 1;
    }
    printf("rmv: renamed %s -> %s\n", argv[1], argv[2]);
    return 0;
}
