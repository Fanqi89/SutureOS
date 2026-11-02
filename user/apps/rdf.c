/* rdf.c - report free/total space of a mounted filesystem from ring 3.
 *
 * Usage: rdf [path]        (no argument: "/")
 *
 * This is statfs(2) in the SutureOS dialect: the kernel's sys2_statfs() fills two
 * 64-bit words - free bytes, then total bytes - for the mount that owns `path`
 * (kernel/syscall/syscalls2.c). If the mount cannot report space the call fails
 * with -ENOENT and this tool prints that failure: it never invents numbers.
 *
 * License: GPL-3.0, part of SutureOS (see user/apps/r3tool.h).
 */
#include <stdio.h>
#include <string.h>

#include "r3tool.h"

static unsigned long pct(unsigned long used, unsigned long total)
{
    if (total == 0) return 0;
    /* 0.5% rounding without floating point */
    return (used * 100ul + total / 2ul) / total;
}

int main(int argc, char **argv)
{
    uint64_t space[2];
    const char *path = (argc > 1) ? argv[1] : "/";
    long r;

    memset(space, 0, sizeof(space));
    r = r3_statfs(path, space);
    if (r < 0) {
        if (-r == 38) {
            printf("rdf: %s: statfs is not available in this kernel\n", path);
        } else {
            r3_fail("rdf", path, r);
        }
        printf("rdf: no space numbers reported (nothing was guessed)\n");
        return 1;
    }
    {
        const unsigned long total = (unsigned long)space[1];
        const unsigned long freeb = (unsigned long)space[0];
        const unsigned long used = (total >= freeb) ? (total - freeb) : 0;

        printf("Filesystem: %s\n", path);
        printf("total: %lu bytes (%lu KiB)\n", total, total / 1024ul);
        printf("free : %lu bytes (%lu KiB)\n", freeb, freeb / 1024ul);
        printf("used : %lu bytes (%lu%%)\n", used, pct(used, total));
    }
    return 0;
}
