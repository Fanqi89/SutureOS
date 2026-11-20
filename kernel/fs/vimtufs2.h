/* SutureOS module: VimtuFS2 (vimtufs2_) - read-only filesystem driver interface
 *
 * The on-disk format is VimtuOS' VimtuFS2.  Format specification and reference
 * implementation: VimtuOS kernel/vfs64.h + kernel/vfs64.cpp (GPL-3.0).  This
 * driver is an independent re-implementation written from that specification -
 * no VimtuOS source code was copied.  See kernel/fs/vimtufs2.c for the layout
 * table with a spec line reference for every offset.
 *
 * Supported: v5 (current VimtuOS output, feature_bits must be 0), v4 and v3
 * (same 128 B inode layout).  Not supported: v2 (64 B inodes) and any volume
 * with journal/quota/ACL/snapshot feature bits - those are refused with the
 * exact bits in the log.  Every write operation is refused (read-only driver).
 */
#ifndef STITCH_VIMTUFS2_H
#define STITCH_VIMTUFS2_H

#include <stitch/types.h>

/* ---- format constants (VimtuOS vfs64.h:166-204) ---- */
#define VIMTUFS2_SECTOR_BYTES    512u
#define VIMTUFS2_INODE_BYTES     128u
#define VIMTUFS2_INODES_PER_BLK  4u
#define VIMTUFS2_NAME_MAX        31u
#define VIMTUFS2_DIRECT_BLOCKS   4u
#define VIMTUFS2_INDIRECT_PTRS   128u
#define VIMTUFS2_DIND_CHILDREN   128u
/* first filesystem block that lives behind the double indirect block */
#define VIMTUFS2_L2_FIRST_BLOCK  (VIMTUFS2_DIRECT_BLOCKS + VIMTUFS2_INDIRECT_PTRS)
#define VIMTUFS2_MAX_MAP_BLOCKS  (VIMTUFS2_L2_FIRST_BLOCK + \
                                  VIMTUFS2_DIND_CHILDREN * VIMTUFS2_INDIRECT_PTRS)
#define VIMTUFS2_MAX_FILE_BLOCKS 16384u
#define VIMTUFS2_MAX_FILE_BYTES  (VIMTUFS2_MAX_FILE_BLOCKS * VIMTUFS2_SECTOR_BYTES)
#define VIMTUFS2_MAX_INODES      2048u
#define VIMTUFS2_MIN_BLOCKS      32u
#define VIMTUFS2_PATH_MAX        128u
#define VIMTUFS2_PATH_DEPTH_MAX  16u

#define VIMTUFS2_VERSION_V3      3u
#define VIMTUFS2_VERSION_V4      4u
#define VIMTUFS2_VERSION_V5      5u

/* ---- driver status codes ---- */
#define VIMTUFS2_OK            0
#define VIMTUFS2_ENOENT       -1
#define VIMTUFS2_EIO          -2
#define VIMTUFS2_EINVAL       -3
#define VIMTUFS2_ENOTDIR      -4
#define VIMTUFS2_EISDIR       -5
#define VIMTUFS2_ECORRUPT     -6
#define VIMTUFS2_ENAMETOOLONG -7
#define VIMTUFS2_ENOTSUP      -8
#define VIMTUFS2_EBUSY        -9
#define VIMTUFS2_ENOSPC      -10

/* inode types on disk (VimtuOS vfs64.h:351-357) */
#define VIMTUFS2_T_FREE   0u
#define VIMTUFS2_T_FILE   1u
#define VIMTUFS2_T_DIR    2u
#define VIMTUFS2_T_LINK   3u

/* Number of volumes that can be mounted at the same time. */
#define VIMTUFS2_MAX_MOUNTS 4

/* Mounted-volume geometry, as verified against the on-disk superblock. */
struct vimtufs2_vol_info {
    int      drive;
    uint32_t start_lba;
    uint32_t version;
    uint32_t blocks;            /* total blocks in the volume            */
    uint32_t inodes;            /* inode slots                           */
    uint32_t inode_bytes;       /* 128 on v3/v4/v5                       */
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t inode_start;
    uint32_t inode_blocks;
    uint32_t data_start;
    uint32_t data_blocks;
    uint32_t free_blocks;       /* measured from the bitmap at probe time */
};

/* Metadata of one inode (VimtuOS Vfs64Info64, trimmed to what the VFS needs). */
struct vimtufs2_stat {
    uint32_t index;             /* inode number                          */
    uint32_t type;              /* VIMTUFS2_T_*                          */
    uint32_t size;              /* bytes (directories are always 0)      */
    uint32_t mtime;             /* packed (year-2000)<<26|mon<<22|...    */
    uint32_t parent;
    uint32_t nlink;
    uint32_t uid;               /* 0 on v3 (no permission fields)        */
    uint32_t gid;
    uint32_t mode;              /* S_IF*|0777, v3 default modes          */
    char     name[VIMTUFS2_NAME_MAX + 1];
    uint32_t name_len;
};

struct vimtufs2_file;
struct vimtufs2_dir;

/* ---- volume lifecycle ---- */

/* Read-only probe: validates the superblock and the root inode, measures the
 * free-block count, and leaves **no** driver state behind (the boot probe uses
 * this).  `out` may be NULL.  Returns VIMTUFS2_OK or a negative code. */
int vimtufs2_probe(int drive, uint32_t start_lba, struct vimtufs2_vol_info *out);

/* Boot-time probe: prints exactly one log line - either the volume geometry or
 * "no volume on drive N (reason=...)".  Never mounts anything. */
int vimtufs2_boot_probe(int drive);

/* Mount / unmount.  vimtufs2_mount() returns the volume slot (>= 0) or a
 * negative code; it also logs the geometry line. */
int vimtufs2_mount(int drive, uint32_t start_lba);
int vimtufs2_unmount(int drive, uint32_t start_lba);

/* Volume selection: every call below works on the selected volume. */
int vimtufs2_volume_count(void);
int vimtufs2_select(int slot);
int vimtufs2_selected(void);
int vimtufs2_is_mounted(int slot);
int vimtufs2_volume_info(int slot, struct vimtufs2_vol_info *out);

/* ---- read-only operations (paths are volume-relative: "/HELLO.TXT") ---- */

int vimtufs2_open(const char *path, struct vimtufs2_file **out);
int vimtufs2_read(struct vimtufs2_file *f, void *buf, uint32_t len, uint32_t off);
int vimtufs2_close(struct vimtufs2_file *f);

int vimtufs2_stat(const char *path, struct vimtufs2_stat *out);

int vimtufs2_opendir(const char *path, struct vimtufs2_dir **out);
/* Returns VIMTUFS2_OK and one entry, or VIMTUFS2_ENOENT at end of directory.
 * Only inodes whose parent is this directory are reported (the root directory
 * and free slots are skipped). */
int vimtufs2_readdir(struct vimtufs2_dir *d, char *name, uint32_t cap,
                     uint32_t *name_len, int *is_dir);
int vimtufs2_closedir(struct vimtufs2_dir *d);

#endif /* STITCH_VIMTUFS2_H */
