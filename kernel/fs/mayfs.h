/* SutureOS: MayFS driver - MayOS' snapshot filesystem, mounted for real.
 *
 * PROVENANCE / LICENCE
 *   Format and algorithms come from MayOS kernel/fs.c (MayOS_0.3.0-pre1,
 *   https://github.com/... / local collection "MayOS").  The MayOS repository
 *   root LICENSE was read during this port: it is the **MIT License,
 *   Copyright (c) 2024 MayOS Project** - NOT GPL-3.0, which the porting list
 *   had assumed.  Because MIT is compatible with SutureOS' GPL-3.0-or-later
 *   tree this port is fine, but the file header records the real licence.
 *   The implementation below is written from scratch against that format
 *   (structure layout and the FNV-1a checksum semantics are identical, the code
 *   is not a copy); see docs/mayfs-format-notes.md for the byte-level spec.
 *
 * WHAT MAYFS IS
 *   A tiny fixed-size filesystem that stores ONE whole snapshot of a flat
 *   record table (48 fixed 4160-byte records, each holding a 4 KiB file body)
 *   in a 256 KiB slot, plus a second identical slot as a rollback copy.  There
 *   is no allocation bitmap, no indirect blocks and no journal: every commit
 *   rewrites both 256 KiB slots, backup first, so a power cut can never destroy
 *   the last good copy.
 *
 * SCOPE OF THIS DRIVER
 *   read + write (create/truncate/write/unlink/mkdir/rmdir/rename/commit),
 *   one mounted MayFS volume at a time (like the ext4 driver), VFS name
 *   "mayfs".  All symbols are prefixed mayfs_.
 */
#ifndef STITCH_MAYFS_H
#define STITCH_MAYFS_H

#include <stitch/types.h>

/* ==================== on-disk geometry ==================== */
#define MAYFS_SECTOR_SIZE      512u
#define MAYFS_SLOT_SECTORS     512u                                  /* FS_SECTORS */
#define MAYFS_SLOT_BYTES       (MAYFS_SLOT_SECTORS * MAYFS_SECTOR_SIZE) /* 262144 */
#define MAYFS_PRIMARY_LBA      0u
#define MAYFS_BACKUP_LBA       MAYFS_SLOT_SECTORS                     /* 512 */
#define MAYFS_TOTAL_SECTORS    (MAYFS_SLOT_SECTORS * 2u)              /* 1024 */

#define MAYFS_MAX_FILES        48u        /* FS_MAX: record slots incl. dirs */
#define MAYFS_NAME_BYTES       48u        /* File.name[] */
#define MAYFS_NAME_MAX         47u        /* longest legal name (NUL kept) */
#define MAYFS_FILE_CAP         4096u      /* File.data[] */
#define MAYFS_FILE_MAX         4095u      /* size must stay < FILE_CAP */

#define MAYFS_TABLE_OFFSET     512u       /* File table starts in sector 1 */
#define MAYFS_ENTRY_BYTES      4160u      /* 4*4 + 48 + 4096 */
#define MAYFS_TABLE_BYTES      (MAYFS_MAX_FILES * MAYFS_ENTRY_BYTES)  /* 199680 */
#define MAYFS_TABLE_SECTORS    (MAYFS_TABLE_BYTES / MAYFS_SECTOR_SIZE) /* 390 */

#define MAYFS_PREFS_OFFSET     (500u * MAYFS_SECTOR_SIZE)             /* 256000 */
#define MAYFS_PREFS_LENGTH     32u
#define MAYFS_PREFS_MAGIC      "MAYPREF1"
#define MAYFS_MAGIC            "MAYFS001"
#define MAYFS_GEN_TAG          "MAY2"

/* Longest volume-relative path the driver accepts (VFS_MAX_PATH is 256). */
#define MAYFS_PATH_MAX         256u

/* Records 0..3 must be root-child directories (MayOS snapshot_valid rule). */
#define MAYFS_ROOT_INDEX       0
#define MAYFS_FIXED_DIRS       4u

/* ==================== error codes ==================== */
#define MAYFS_OK         0
#define MAYFS_ENOENT    -1
#define MAYFS_ENOTDIR   -2
#define MAYFS_EISDIR    -3
#define MAYFS_EINVAL    -4
#define MAYFS_EEXIST    -5
#define MAYFS_ENOSPC    -6
#define MAYFS_EIO       -7
#define MAYFS_ENOTEMPTY -8
#define MAYFS_EBUSY     -9
#define MAYFS_ENOTSUP  -10

/* ==================== in-memory record ==================== */
/* Byte-identical to MayOS' File (core.h): 4 x u32, char[48], u8[4096]. */
struct mayfs_entry {
    uint32_t used;
    uint32_t dir;
    uint32_t parent;
    uint32_t size;
    char     name[MAYFS_NAME_BYTES];
    uint8_t  data[MAYFS_FILE_CAP];
};

struct mayfs_stat {
    int      index;      /* record slot (0..MAYFS_MAX_FILES-1) */
    uint32_t is_dir;
    uint32_t size;
};

/* ==================== handles ==================== */
#define MAYFS_MAX_OPEN_FILES 8
#define MAYFS_MAX_OPEN_DIRS  8

struct mayfs_file {
    int used;
    int index;
};
struct mayfs_dir {
    int used;
    int index;      /* directory record being listed */
    uint32_t next;  /* next record slot to inspect */
};

/* ==================== core API (kernel/fs/mayfs.c) ==================== */

/* FNV-1a 32 used by MayFS for both the table and the prefs record. */
uint32_t mayfs_checksum(const uint8_t *p, uint32_t n);

/* Read-only boot probe: never writes, prints exactly one line. */
int mayfs_boot_probe(int drive);

/* Mount/unmount (single volume).  mount() also recovers the newer of the two
 * snapshots and initialises a blank disk. */
int mayfs_mount(int drive, uint32_t start_lba);
int mayfs_unmount(void);
int mayfs_is_mounted(void);
int mayfs_drive(void);
uint32_t mayfs_generation(void);
int mayfs_active_slot(void);
int mayfs_used_count(void);

/* Commit both slots (backup first, then primary). */
int mayfs_commit(void);

/* Commit only when the in-memory table changed since the last commit. */
int mayfs_sync(void);

/* Path operations (paths are volume-relative and start with '/'). */
int mayfs_resolve(const char *path, int *out_index);
int mayfs_stat(const char *path, struct mayfs_stat *st);
int mayfs_create(const char *path, int dir);
int mayfs_remove(const char *path);          /* file -> unlink, dir -> must be empty */
int mayfs_rename(const char *oldpath, const char *newpath);
int mayfs_count_free(uint32_t *free_records, uint32_t *used_records);

/* File handles. */
int mayfs_file_open(const char *path, int create, int trunc, struct mayfs_file **out);
int mayfs_file_read(struct mayfs_file *f, void *buf, uint32_t size, uint32_t offset);
int mayfs_file_write(struct mayfs_file *f, const void *buf, uint32_t size, uint32_t offset);
int mayfs_file_size(struct mayfs_file *f);
int mayfs_file_close(struct mayfs_file *f);

/* Directory handles. */
int mayfs_opendir(const char *path, struct mayfs_dir **out);
int mayfs_readdir(struct mayfs_dir *d, char *name, uint32_t cap,
                  uint32_t *out_len, int *is_dir);
int mayfs_closedir(struct mayfs_dir *d);

#endif /* STITCH_MAYFS_H */
