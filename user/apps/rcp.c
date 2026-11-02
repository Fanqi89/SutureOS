/* rcp.c - copy a file from ring 3 (the ring-3 half of `cp`).
 *
 * Usage: rcp <source> <destination>
 *
 * The destination is created if it does not exist and overwritten when it does
 * (O_CREAT|O_TRUNC, exactly what cp does). A failed open/read/write is reported
 * with the real -errno and the tool exits 1; a partial copy says how many bytes
 * made it, so nothing "looks successful" when it was not.
 *
 * License: GPL-3.0, part of SutureOS (see user/apps/r3tool.h).
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "r3tool.h"

#define RCP_BUF 1024

int main(int argc, char **argv)
{
    char buf[RCP_BUF];
    struct stat st;
    long in;
    long out;
    unsigned long total = 0;
    int failed = 0;

    if (argc != 3) {
        printf("usage: rcp <source> <destination>\n");
        return 1;
    }
    /* Copying a file onto itself would truncate the source before reading it. */
    if (strcmp(argv[1], argv[2]) == 0) {
        printf("rcp: %s: source and destination are the same path\n", argv[1]);
        return 1;
    }
    if (r3_stat(argv[2], &st) == 0 && S_ISDIR(st.st_mode)) {
        printf("rcp: %s: is a directory (this copy takes a file name)\n", argv[2]);
        return 1;
    }

    in = r3_open(argv[1], O_RDONLY, 0);
    if (in < 0) {
        r3_fail("rcp", argv[1], in);
        return 1;
    }
    out = r3_open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        r3_fail("rcp", argv[2], out);
        (void)r3_close((int)in);
        return 1;
    }

    for (;;) {
        long n;
        long off = 0;

        n = r3_read((int)in, buf, sizeof(buf));
        if (n < 0) {
            r3_fail("rcp", argv[1], n);
            failed = 1;
            break;
        }
        if (n == 0) break;
        while (off < n) {
            const long w = r3_write((int)out, buf + off, (unsigned long)(n - off));
            if (w <= 0) {
                r3_fail("rcp", argv[2], (w < 0) ? w : -5);
                failed = 1;
                break;
            }
            off += w;
            total += (unsigned long)w;
        }
        if (failed) break;
    }
    if (r3_close((int)out) < 0) failed = 1;
    (void)r3_close((int)in);
    /* Flush every mount so the FAT32 free-cluster counter matches the FAT. */
    (void)r3_sync();

    if (failed) {
        printf("rcp: FAILED after %lu byte(s): %s -> %s\n", total, argv[1], argv[2]);
        return 1;
    }
    printf("rcp: %s -> %s (%lu bytes)\n", argv[1], argv[2], total);
    return 0;
}
