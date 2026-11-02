/* rls.c - list a directory from ring 3 (the ring-3 half of `ls`).
 *
 * Usage: rls [path]        (no argument: the current directory)
 *
 * Output: one entry per line, stable and grep-friendly. A directory is printed
 * with a trailing '/', a regular file as its plain name. A path that names a
 * file prints "<path>  <size> bytes" (same shape as the kernel shell's ls).
 *
 * The listing goes through the Linux-numbered syscalls: open(path, O_DIRECTORY)
 * gives a directory cursor (the VFS keeps directory handles in their own table)
 * and getdents64 drains it. Every failure prints the real -errno.
 *
 * License: GPL-3.0, part of SutureOS. See user/apps/r3tool.h for the reference
 * notice (VimtuOS user/busybox/vimtu_dirent.c informed the approach; no code
 * copied).
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "r3tool.h"

#define RLS_BUF 1024

static int list_dir(const char *path)
{
    char buf[RLS_BUF];
    struct stat st;
    long fd;
    long n;
    int count = 0;

    fd = r3_open(path, O_RDONLY | R3_O_DIRECTORY, 0);
    if (fd < 0) {
        r3_fail("rls", path, fd);
        return 1;
    }
    for (;;) {
        long off = 0;

        n = r3_getdents64((int)fd, buf, sizeof(buf));
        if (n < 0) {
            r3_fail("rls", "getdents64", n);
            (void)r3_close((int)fd);
            return 1;
        }
        if (n == 0) break;
        while (off < n) {
            const struct r3_dirent64 *de =
                (const struct r3_dirent64 *)(const void *)(buf + off);
            const char *name = de->d_name;

            if (de->d_reclen == 0 || off + (long)de->d_reclen > n) break;
            /* The kernel already filters "." and ".."; keep the guard anyway so a
             * future driver cannot make rls print them as entries. */
            if (!(name[0] == '.' && (name[1] == '\0' ||
                                     (name[1] == '.' && name[2] == '\0')))) {
                if (de->d_type == R3_DT_DIR) {
                    printf("%s/\n", name);
                } else {
                    printf("%s\n", name);
                }
                count++;
            }
            off += (long)de->d_reclen;
        }
    }
    (void)r3_close((int)fd);
    if (count == 0) {
        printf("%s: empty directory\n", path);
    } else if (r3_stat(path, &st) == 0) {
        /* one summary line so a human can tell a full listing from a truncated
         * one; the entry lines above stay the machine-readable part */
        printf("%d entr%s in %s\n", count, (count == 1) ? "y" : "ies", path);
    }
    return 0;
}

int main(int argc, char **argv)
{
    char cwd[256];
    struct stat st;
    const char *path;

    if (argc > 1) {
        path = argv[1];
    } else {
        const long r = r3_getcwd(cwd, sizeof(cwd));
        if (r < 0) {
            r3_fail("rls", "getcwd", r);
            return 1;
        }
        path = cwd;
    }

    /* A plain file: report it like ls does instead of failing with ENOTDIR. */
    if (r3_stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
        printf("%s  %lu bytes\n", path, (unsigned long)st.st_size);
        return 0;
    }
    return list_dir(path);
}
