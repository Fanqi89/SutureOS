/* rmkdir.c - create directories from ring 3 (the ring-3 half of `mkdir`).
 *
 * Usage: rmkdir <path> [path...]
 *        rmkdir -p <path> [path...]     create missing parents too
 *
 * -p is implemented in user space (the kernel's mkdir(2) has no parent-creation
 * mode): every prefix of the path is created and EEXIST is tolerated, which is
 * exactly mkdir -p semantics. Without -p an existing path is an error (EEXIST) and
 * the directory is left untouched.
 *
 * License: GPL-3.0, part of SutureOS (see user/apps/r3tool.h).
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "r3tool.h"

/* mkdir -p: create every component of `path`, tolerating EEXIST. */
static int mkdir_p(const char *path)
{
    char buf[256];
    size_t i;
    size_t n = strlen(path);
    int made = 0;
    int failed = 0;

    if (n == 0 || n >= sizeof(buf)) {
        printf("rmkdir: %s: path too long\n", path);
        return 1;
    }
    memcpy(buf, path, n + 1);
    for (i = 1; i <= n; i++) {
        if (buf[i] != '/' && buf[i] != '\0') continue;
        {
            const char saved = buf[i];
            struct stat st;
            long r;
            int exists;

            buf[i] = '\0';
            /* Check first: a component that already exists is left alone instead of
             * being re-created as a ramfs shadow. */
            exists = (r3_stat(buf, &st) == 0);
            if (!exists && saved == '/') {
                /* A directory this VFS holds only as a *mount parent* has no node of
                 * its own: /mnt is not a directory here, /mnt/fat is the mount. If the
                 * next component already resolves, this prefix is such a parent -
                 * skip it rather than creating a ramfs shadow of it. */
                char probe[256];
                size_t plen = strlen(buf);
                size_t k = 0;

                if (plen + 1 < sizeof(probe)) {
                    memcpy(probe, buf, plen);
                    probe[plen++] = '/';
                    while (path[i + 1 + k] != '\0' && path[i + 1 + k] != '/' &&
                           plen + 1 < sizeof(probe)) {
                        probe[plen++] = path[i + 1 + k++];
                    }
                    probe[plen] = '\0';
                    if (r3_stat(probe, &st) == 0) exists = 1;
                }
            }
            if (exists) {
                buf[i] = saved;
                continue;
            }
            r = r3_mkdir(buf, 0755);
            if (r == 0) {
                printf("rmkdir: created %s\n", buf);
                made++;
            } else if (-r != 17) {             /* anything but EEXIST is fatal */
                r3_fail("rmkdir", buf, r);
                failed = 1;
            }
            buf[i] = saved;
        }
        if (failed) break;
    }
    if (!failed && made == 0) {
        printf("rmkdir: %s: already exists (nothing to do)\n", path);
    }
    (void)r3_sync();
    return failed;
}

int main(int argc, char **argv)
{
    int parents = 0;
    int start = 1;
    int failed = 0;
    int i;

    if (argc < 2) {
        printf("usage: rmkdir [-p] <path> [path...]\n");
        return 1;
    }
    if (strcmp(argv[1], "-p") == 0) {
        parents = 1;
        start = 2;
        if (argc < 3) {
            printf("usage: rmkdir -p <path> [path...]\n");
            return 1;
        }
    }
    for (i = start; i < argc; i++) {
        if (parents) {
            if (mkdir_p(argv[i]) != 0) failed = 1;
        } else {
            const long r = r3_mkdir(argv[i], 0755);
            (void)r3_sync();
            if (r < 0) {
                if (-r == 17) {
                    printf("rmkdir: %s: already exists (errno=17 EEXIST)\n", argv[i]);
                } else {
                    r3_fail("rmkdir", argv[i], r);
                }
                failed = 1;
            } else {
                printf("rmkdir: created %s\n", argv[i]);
            }
        }
    }
    return failed;
}
