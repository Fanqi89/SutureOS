/* SutureOS: terminal system installer (see installer.c for the design notes).
 *
 * `install` in the shell calls installer_run(): it lists the writable disks,
 * asks which one to use, asks for the root password (empty allowed = locked
 * account) and an optional normal user, then really writes an MBR partition
 * table, the Limine BIOS boot code, a FAT32 system partition and the system
 * files. Nothing is written before the explicit "yes" confirmation.
 */
#ifndef STITCH_INSTALLER_H
#define STITCH_INSTALLER_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Interactive installer. Returns 0 when a system was installed, -1 when the user
 * cancelled or an error was reported (each failure prints its reason). */
int installer_run(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_INSTALLER_H */
