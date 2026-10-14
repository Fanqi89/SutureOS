/* Host-harness shadow of include/stitch/types.h.
 *
 * The harness compiles the REAL kernel/fs/squashfs.c and kernel/pkg/inflate.c
 * with -I tests/squashfs_harness/include first, so this file (and the other
 * shadows next to it) replaces the kernel headers. The host standard library
 * already defines every fixed-width type, so the kernel typedefs are simply
 * mapped onto <stdint.h>/<stddef.h>/<stdbool.h>. Only the handful of extra
 * names the driver uses are added here.
 */
#ifndef STITCH_TYPES_H
#define STITCH_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef EPERM
#define EPERM 1
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef EROFS
#define EROFS 30
#endif

#endif /* STITCH_TYPES_H */
