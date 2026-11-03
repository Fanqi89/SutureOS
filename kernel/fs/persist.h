/* SutureOS: persistent user data on the installed system volume (see persist.c)
 *
 * The ramfs stays `/`; this is a two-way mirror of /etc, /home and /root to
 * <system volume>/SUTUREOS/PERSIST, restored at boot and flushed on `sync`,
 * `persist flush`, and before `shutdown` / `reboot`.
 */
#ifndef STITCH_PERSIST_H
#define STITCH_PERSIST_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Detect the persistence volume (/mnt/sys/SUTUREOS/PERSIST or an installed
 * system volume that will get the skeleton on first flush). 1 = available. */
int persist_detect(void);

/* 1 when a flush/restore can run (installed system boot). 0 on a live ISO. */
int persist_present(void);

/* Boot: copy PERSIST/{etc,home,root} into the ramfs. Returns the file count. */
int persist_restore(void);

/* Copy ramfs /etc, /home, /root back to the volume and vfs_sync() it.
 * Returns the number of files written, -1 on a write error, 0 when there is no
 * persistence volume (nothing to do). */
int persist_flush(void);

/* Format version read from PERSIST/VERSION (0 when absent). */
int persist_format_version(void);

/* Diagnostics for the `persist` shell command and the boot log. */
int persist_restored_files(void);
int persist_flushed_files(void);
int persist_flush_count(void);
int persist_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_PERSIST_H */
