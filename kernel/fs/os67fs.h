/* SutureOS OS67 inode-filesystem port (Minix v1 layout) - core.
 *
 * Ported from OS67's inode layer.  Format and algorithm reference (read-only):
 *   OS67 inc/minix.h   - superblock / d_inode / dir_entry / IBLK / IMAP_BLK /
 *                        ZMAP_BLK / NDIRECT / NINDIRECT definitions
 *   OS67 inc/inode.h   - inode cache API (iget / ialloc / idup / iput / ilock /
 *                        iupdate / iread / iwrite / istat), NINODE = 500
 *   OS67 fs/inode.c    - inode cache, reference counting, lock flags, inode
 *                        read/write, direct + single-indirect block mapping
 *   OS67 fs/bitmap.c   - inode/zone bitmap numbering (bit i >= 1, zone bits
 *                        relative to fst_data_zone)
 *   OS67 fs/dir.c      - 16-byte directory entries {u16 ino, char name[14]}
 * OS67 is redistributed under GPL-3.0-compatible terms as recorded in
 * docs/licensing-audit.md; this file is an independent re-implementation
 * written from that format specification - no OS67 source code was copied.
 *
 * Deliberately READ-ONLY for this port: the inode cache, reference counting,
 * lock flags and block mapping are the algorithms being ported; the on-disk
 * write path (ialloc/itrunc/iwrite) is not enabled and reports VFS_EPERM, so a
 * bug here can never corrupt a user volume.
 */
#ifndef OS67FS_H
#define OS67FS_H

#include <stitch/types.h>
#include "vfs.h"

#define OS67FS_BSIZE        1024u
#define OS67FS_INODE_SIZE   32u
#define OS67FS_IPB          (OS67FS_BSIZE / OS67FS_INODE_SIZE)   /* 32 */
#define OS67FS_NDIRECT      7
#define OS67FS_NINDIRECT    (OS67FS_BSIZE / 2u)                  /* 512 */
#define OS67FS_NAME_LEN     14
#define OS67FS_NINODE_CACHE 32

#define OS67FS_MAGIC_MINIX1 0x137Fu   /* classic Minix v1 */
#define OS67FS_MAGIC_OS67   0x138Fu   /* Minix v1 with 30-char names (accepted) */

#define OS67FS_S_IFMT   0xF000u
#define OS67FS_S_IFDIR  0x4000u
#define OS67FS_S_IFREG  0x8000u

struct os67fs_super {
    uint16_t ninodes;
    uint16_t nzones;
    uint16_t imap_blk;
    uint16_t zmap_blk;
    uint16_t fst_data_zone;
    uint16_t log_zone_size;
    uint32_t max_size;
    uint16_t magic;
    uint16_t state;
} __attribute__((packed));

struct os67fs_inode {          /* in-memory inode cache entry (OS67 struct inode) */
    int      used;
    int      valid;            /* I_VALID: fields below were read from disk   */
    int      busy;             /* I_BUSY:  locked by a user                   */
    int      ref;              /* reference count (OS67 ip->ref)             */
    uint32_t ino;
    uint16_t mode;
    uint32_t size;
    uint8_t  nlinks;
    uint16_t zone[9];
};

/* Mount one volume. Returns 0 on success, negative VFS error otherwise. */
int os67fs_mount_drive(int drive);
/* Read-only boot probe: one geometry line or one no-volume line. */
int os67fs_boot_probe(int drive);
/* True when at least one volume is mounted. */
int os67fs_ready(void);

/* Core operations used by the VFS adapter (paths are relative to the mount
 * point, i.e. "" is the root, "SUBDIR/INNER.TXT" is a nested lookup). */
int os67fs_resolve(int vol, const char *path, uint32_t *out_ino);
int os67fs_inode_stat(int vol, uint32_t ino, struct vfs_stat *st);
int os67fs_read(int vol, uint32_t ino, void *buf, uint32_t size, uint32_t offset);
int os67fs_dir_entry(int vol, uint32_t dir_ino, int index, char *name,
                     uint32_t cap, uint32_t *out_ino, vfs_node_type_t *type);
const char *os67fs_volume_name(int vol);
int os67fs_volume_drive(int vol);

#endif
