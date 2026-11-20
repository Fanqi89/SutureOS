/* SutureOS module: VimtuFS2 read-only filesystem driver (kernel/fs/vimtufs2.c)
 *
 * WHAT THIS IS
 *   A driver for VimtuFS2, the on-disk filesystem of VimtuOS (vfs64).  It
 *   mounts a volume, validates the superblock and every inode it uses, resolves
 *   paths, reads files through the direct / single-indirect / double-indirect
 *   block tree, lists directories whose entries are "inodes whose parent is
 *   this directory" (VimtuFS2 has no directory data blocks at all), and reports
 *   metadata.  Read-only: the VFS adapter (kernel/fs/vimtufs2_vfs.c) refuses
 *   every write path with EPERM.
 *
 * WHERE THE FORMAT COMES FROM (provenance / licence)
 *   Specification + reference implementation: VimtuOS
 *       kernel/vfs64.h    file header: "磁盘布局", the v4/v5 inode table, the
 *                         block-chain table, limits, the mtime packing
 *       kernel/vfs64.cpp  VFS_O_* / VFS_I_* offsets, sb_verify(), inode_ok(),
 *                         the rmap_* block mapping, vfs64_format()
 *   VimtuOS is GPL-3.0; SutureOS is GPL-3.0.  The format may be implemented
 *   here, but this file is an independent re-implementation written from that
 *   specification - no VimtuOS source was copied.  Every offset below carries
 *   the reference line so a reader can check it against the spec without
 *   opening the reference tree.
 *
 * SUPPORTED / NOT SUPPORTED (all stated honestly, nothing faked)
 *   v5  current VimtuOS output: v4 layout + a feature extension header at
 *       superblock [64,88).  **feature_bits must be 0.**  Journal (bit0),
 *       quota (bit1), ACL (bit2) and snapshot (bit3) are format-level features
 *       this driver does not implement, so a volume that asks for them is
 *       rejected with the exact bits in the log instead of being mounted
 *       half-validated (a journaled volume must be replayed before any read,
 *       which we cannot do; ACL needs an xattr block we cannot interpret).
 *   v4  feature-free v4 volumes: identical 128 B inode layout, uid/gid/mode
 *       present, reserved run [81,124) must be zero.
 *   v3  the legacy 128 B inode layout with no permission fields: read as
 *       root-owned with the documented default modes (directory 0755, file
 *       0644), exactly like the reference does for legacy volumes.
 *   v2  (64 B inodes, no double indirect, 27 B names) is NOT supported: it is a
 *       different layout and this port has no fixture for it.  The mount is
 *       refused with reason=version - we do not pretend to read it.
 *   Symlinks (inode type 3) are structurally validated but opening one is
 *   refused (ENOTSUP): resolving them needs the target string, which is not
 *   part of this port's scope.
 *   Timestamps are validated and exposed by vimtufs2_stat() as the packed
 *   32-bit value; the VFS vfs_stat_t wants epoch seconds, and rather than
 *   inventing a calendar conversion the adapter reports mtime = 0 (documented
 *   in kernel/fs/vimtufs2_vfs.c).
 */

#include "vimtufs2.h"

#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "block.h"

/* ==================== on-disk offsets (spec-referenced) ====================
 * Superblock: VimtuOS kernel/vfs64.cpp:39-65 (VFS_O_*), all little endian.
 * The main CRC32 covers [0,60); the v5 feature extension header lives at
 * [64,88) and has its own CRC over [64,84).  0xAA55 sits at offset 510.
 */
enum {
    SB_MAGIC = 0,        /* 8B  "VIMTUFS2"                                    */
    SB_VERSION = 8,      /* u32 2/3/4/5                                       */
    SB_SECTOR = 12,      /* u32 512                                           */
    SB_BLOCK = 16,       /* u32 512                                           */
    SB_TOTAL = 20,       /* u32 volume size in blocks                         */
    SB_ROOT = 24,        /* u32 root inode (0)                                */
    SB_BITMAP = 28,      /* u32 bitmap start block                            */
    SB_BITMAPN = 32,     /* u32 bitmap block count                            */
    SB_INODE = 36,       /* u32 inode table start block                       */
    SB_INODEN = 40,      /* u32 inode slot count                              */
    SB_INOSZ = 44,       /* u32 bytes per inode                               */
    SB_DATA = 48,        /* u32 data area start block                         */
    SB_DATAN = 52,       /* u32 data block count                              */
    SB_FLAGS = 56,       /* u32 reserved (must be 0)                          */
    SB_CRC = 60,         /* u32 CRC32 over [0,60)                             */
    SB_CRC_LEN = 60,
    SB_FEATBITS = 64,    /* u32 feature bits (v5 only; 0 = feature free)      */
    SB_JX_START = 68,    /* u32 journal area start block                      */
    SB_JX_NBLK = 72,     /* u32 journal block count                           */
    SB_QUOTA = 76,       /* u32 quota table block                             */
    SB_SNAP = 80,        /* u32 snapshot table block                          */
    SB_FEATCRC = 84,     /* u32 CRC32 over [64,84)                            */
    SB_FEAT_CRC_LEN = 20,
    SB_EXT_END = 88,     /* end of the v5 extension header                    */
    SB_SIG = 510         /* u16 0xAA55                                        */
};

/* Inode: VimtuOS kernel/vfs64.cpp:69-97 (VFS_I_* / VFS_I3_* / VFS_I4_*).
 * v3/v4/v5 all use the 128 B layout; the tail differs:
 *   v3 reserved [75,124)   v4 uid/gid/mode at 75/77/79 + reserved [81,124)
 *   v5 same as v4, but [81,85) = xattr block and [85,89) = snapshot flags
 */
enum {
    I_TYPE = 0,          /* u8  0 free / 1 file / 2 dir / 3 symlink          */
    I_NAMELEN = 1,       /* u8  0..31 (0 only for the root directory)        */
    I_RSVD = 2,          /* u16 must be 0                                    */
    I_SIZE = 4,          /* u32 byte count (directories: always 0)           */
    I_D0 = 8,            /* u32 direct blocks 0..3 at 8/12/16/20             */
    I_IND = 24,          /* u32 single indirect block                        */
    I_PARENT = 28,       /* u32 parent inode (root points at itself)         */
    I_MTIME = 32,        /* u32 packed timestamp (0 = unknown)               */
    I_NLINK = 36,        /* u16 file 1 / dir 1 + direct subdirs              */
    I_KIND = 38,         /* u8  cached content kind 0..6                     */
    I_RSVD2 = 39,        /* u8  must be 0                                    */
    I_NAME = 40,         /* 31 B name, not NUL terminated on disk            */
    I_DIND = 71,         /* u32 double indirect block (v3/v4/v5 only)        */
    I_UID = 75, I_GID = 77, I_MODE = 79,
    I_XATTR = 81, I_SNAPF = 85,
    I_CRC = 124          /* u32 CRC32 over [0,124)                           */
};

/* vf_layout mirrors VimtuOS' Vfs64Layout table (vfs64.cpp:119-137). */
struct vf_layout {
    uint32_t version;
    uint32_t inode_bytes;
    uint32_t inodes_per_blk;
    uint32_t name_max;
    uint32_t crc_off;
    uint32_t dind_off;                 /* 0 = this version has no dind      */
    uint32_t uid_off, gid_off, mode_off;   /* 0 = no permission fields     */
    uint32_t rsvd_from;               /* first byte of the must-be-0 run   */
};

static const struct vf_layout VF_LAYOUT_V3 = {
    VIMTUFS2_VERSION_V3, 128u, 4u, 31u, I_CRC, I_DIND, 0u, 0u, 0u, I_UID
};
static const struct vf_layout VF_LAYOUT_V4 = {
    VIMTUFS2_VERSION_V4, 128u, 4u, 31u, I_CRC, I_DIND,
    I_UID, I_GID, I_MODE, I_XATTR
};
static const struct vf_layout VF_LAYOUT_V5 = {
    VIMTUFS2_VERSION_V5, 128u, 4u, 31u, I_CRC, I_DIND,
    I_UID, I_GID, I_MODE, I_SNAPF + 4u
};

/* mode bits (VimtuOS vfs64.h:293-308) - Linux layout, permission bits only */
#define VF_S_IFREG 0100000u
#define VF_S_IFDIR 0040000u
#define VF_S_IFLNK 0120000u
#define VF_TYPE_MASK 0170000u
#define VF_PERM_MASK 0777u
#define VF_DEF_DIR_MODE  (VF_S_IFDIR | 0755u)
#define VF_DEF_FILE_MODE (VF_S_IFREG | 0644u)
#define VF_DEF_LINK_MODE (VF_S_IFLNK | 0777u)

/* feature bits (VimtuOS vfs64.h:220-227) */
#define VF_FEAT_JOURNAL  1u
#define VF_FEAT_QUOTA    2u
#define VF_FEAT_ACL      4u
#define VF_FEAT_SNAPSHOT 8u
#define VF_FEAT_ALL      (VF_FEAT_JOURNAL | VF_FEAT_QUOTA | VF_FEAT_ACL | VF_FEAT_SNAPSHOT)

/* ==================== buffers / state ==================== */
/* The kernel VFS serialises filesystem calls and this driver is not
 * re-entrant; the block buffers below are file-scope so no kernel stack (which
 * is small) is used by a directory scan.  Buffers are only used inside a single
 * public entry point, never across one. */
#define VF_INODE_SCAN_SECTORS 16u        /* 8 KiB = 64 inodes per disk read */
static uint8_t g_scan_buf[VF_INODE_SCAN_SECTORS * VIMTUFS2_SECTOR_BYTES];
#define VF_BITMAP_CHUNK 64u              /* 32 KiB of bitmap per disk read */
static uint8_t g_bitmap_buf[VF_BITMAP_CHUNK * VIMTUFS2_SECTOR_BYTES];
static uint8_t g_data_buf[VIMTUFS2_SECTOR_BYTES];
static uint8_t g_ptr_buf[VIMTUFS2_SECTOR_BYTES];

/* One-sector inode window: four inodes share a 512 B sector, so repeated
 * lookups (open -> stat -> read) do not hit the disk again. */
static uint8_t g_ino_win[VIMTUFS2_SECTOR_BYTES];
static int g_ino_win_valid;
static uint32_t g_ino_win_gen;
static uint32_t g_ino_win_rel;

/* Every probe/mount gets a new generation id, so a window filled while probing
 * one volume can never be reused for another volume (the slot number alone is
 * not unique for probes). */
static uint32_t g_vol_gen;

struct vimtufs2_vol {
    int      used;
    int      slot;
    int      drive;
    uint32_t start_lba;
    uint32_t gen;
    const struct vf_layout *lay;
    uint32_t blocks, inodes, inode_bytes;
    uint32_t bitmap_start, bitmap_blocks;
    uint32_t inode_start, inode_blocks;
    uint32_t data_start, data_blocks;
    uint32_t free_blocks;
};

static struct vimtufs2_vol g_vols[VIMTUFS2_MAX_MOUNTS];
static int g_cur_vol = -1;

#define VF_MAX_OPEN_FILES 8
struct vimtufs2_file {
    int      used;
    int      vol;
    uint32_t ino_index;
    uint32_t size;
    uint32_t mode;
};
static struct vimtufs2_file g_files[VF_MAX_OPEN_FILES];

#define VF_MAX_OPEN_DIRS 4
struct vimtufs2_dir {
    int      used;
    int      vol;
    uint32_t dir_ino;
    uint32_t cursor;
};
static struct vimtufs2_dir g_dirs[VF_MAX_OPEN_DIRS];

/* Failure description: "why did the mount fail" travels as data so the mount
 * path and the boot probe can word it differently without duplicating the
 * validation code. */
struct vf_fail {
    const char *reason;
    const char *field;
    uint32_t got;
    uint32_t want;
    int      has_values;
    uint32_t feature_bits;
};

/* ==================== warnings (rate limited) ==================== */
static int g_warn_left = 12;
static void vf_warn(const char *msg, uint32_t a, uint32_t b) {
    if (g_warn_left > 0) {
        console_printf("[vimtufs2] %s (blk=%u count=%u)\n", msg,
                       (uint64_t)a, (uint64_t)b);
        if (--g_warn_left == 0) {
            console_puts("[vimtufs2] further block-layer warnings suppressed\n");
        }
    }
}

/* ==================== small helpers ==================== */
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Standard CRC-32 (reflected 0xEDB88320, init/xorout 0xFFFFFFFF), the same
 * function VimtuOS uses (vfs64.cpp:206-213) and the same one Python's
 * zlib.crc32() / the host generators compute.
 * Known answer: crc32("123456789") == 0xCBF43926.
 */
static uint32_t vf_crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static int vf_blk_in_data(const struct vimtufs2_vol *vol, uint32_t blk) {
    return blk >= vol->data_start && blk < vol->blocks;
}

/* ==================== block I/O ==================== */
static int vf_read_blocks(const struct vimtufs2_vol *vol, uint32_t blk,
                          uint32_t count, void *buf) {
    if (!vol || !buf || count == 0) return VIMTUFS2_EINVAL;
    if (blk >= vol->blocks || count > vol->blocks - blk) {
        vf_warn("read outside the volume", blk, count);
        return VIMTUFS2_EINVAL;
    }
    if (blk_read(vol->drive, (uint64_t)vol->start_lba + blk, count, buf) != 0) {
        vf_warn("block read failed", blk, count);
        return VIMTUFS2_EIO;
    }
    return VIMTUFS2_OK;
}

static int vf_read_block(const struct vimtufs2_vol *vol, uint32_t blk, void *buf) {
    return vf_read_blocks(vol, blk, 1u, buf);
}

/* ==================== inode access ==================== */
/* Returns a pointer into the one-sector inode window (valid until the next
 * vf_inode_raw() call that has to read a different sector).  Callers that keep
 * the inode across another inode read must copy it first. */
static int vf_inode_raw(struct vimtufs2_vol *vol, uint32_t idx, const uint8_t **out) {
    if (!vol || !out) return VIMTUFS2_EINVAL;
    if (idx >= vol->inodes) return VIMTUFS2_EINVAL;
    const uint32_t rel = idx / VIMTUFS2_INODES_PER_BLK;
    const uint32_t off = (idx % VIMTUFS2_INODES_PER_BLK) * VIMTUFS2_INODE_BYTES;
    if (!g_ino_win_valid || g_ino_win_gen != vol->gen || g_ino_win_rel != rel) {
        const int rc = vf_read_block(vol, vol->inode_start + rel, g_ino_win);
        if (rc != VIMTUFS2_OK) {
            g_ino_win_valid = 0;
            return rc;
        }
        g_ino_win_valid = 1;
        g_ino_win_gen = vol->gen;
        g_ino_win_rel = rel;
    }
    *out = g_ino_win + off;
    return VIMTUFS2_OK;
}

/* Structural validation of one inode, mirroring VimtuOS inode_ok()
 * (vfs64.cpp:2419-2494).  Empty slots are valid and are not CRC checked (a
 * formatted volume leaves them all zero). */
static int vf_inode_ok(const struct vimtufs2_vol *vol, const uint8_t *b,
                       const char **why) {
    const struct vf_layout *lay = vol->lay;
    const uint32_t t = b[I_TYPE];

    if (t > VIMTUFS2_T_LINK) { *why = "type"; return 0; }
    if (b[I_NAMELEN] > lay->name_max) { *why = "namelen"; return 0; }
    if (rd16(b + I_RSVD) != 0) { *why = "reserved"; return 0; }
    if (t == VIMTUFS2_T_FREE) return 1;
    if (rd32(b + lay->crc_off) != vf_crc32(b, lay->crc_off)) { *why = "crc"; return 0; }

    const uint32_t size = rd32(b + I_SIZE);
    if (t == VIMTUFS2_T_FILE && size > VIMTUFS2_MAX_FILE_BYTES) {
        *why = "size";
        return 0;
    }
    if (t == VIMTUFS2_T_LINK && size > 511u) { *why = "link-size"; return 0; }

    if (b[I_RSVD2] != 0) { *why = "rsvd2"; return 0; }
    for (uint32_t i = lay->rsvd_from; i < lay->crc_off; i++) {
        if (b[i] != 0) { *why = "reserved-run"; return 0; }
    }
    if (b[I_KIND] > 6u) { *why = "kind"; return 0; }
    if (rd16(b + I_NLINK) == 0) { *why = "nlink"; return 0; }

    if (lay->mode_off != 0) {
        const uint32_t mode = rd16(b + lay->mode_off);
        const uint32_t want = (t == VIMTUFS2_T_DIR) ? VF_S_IFDIR
                            : (t == VIMTUFS2_T_LINK) ? VF_S_IFLNK : VF_S_IFREG;
        if ((mode & VF_TYPE_MASK) != want) { *why = "mode-type"; return 0; }
        if ((mode & ~(VF_TYPE_MASK | VF_PERM_MASK)) != 0) { *why = "mode-bits"; return 0; }
    }

    const uint32_t mt = rd32(b + I_MTIME);
    if (mt != 0) {
        const uint32_t mo = (mt >> 22) & 0xFu;
        const uint32_t dy = (mt >> 17) & 0x1Fu;
        const uint32_t hh = (mt >> 12) & 0x1Fu;
        const uint32_t mi = (mt >> 6) & 0x3Fu;
        const uint32_t ss = mt & 0x3Fu;
        if (mo < 1u || mo > 12u || dy < 1u || dy > 31u ||
            hh > 23u || mi > 59u || ss > 59u) {
            *why = "mtime";
            return 0;
        }
    }

    /* Names are printable ASCII without '/', exactly what the spec allows for a
     * path segment (VimtuOS vfs64.h:101). */
    const uint32_t nlen = b[I_NAMELEN];
    for (uint32_t i = 0; i < nlen; i++) {
        const uint8_t c = b[I_NAME + i];
        if (c < 0x21u || c > 0x7Eu || c == '/') { *why = "name-chars"; return 0; }
    }

    if (t == VIMTUFS2_T_DIR) {
        /* Directories have no data blocks at all (VimtuOS vfs64.h:85-92). */
        if (size != 0) { *why = "dir-size"; return 0; }
        for (uint32_t d = 0; d < VIMTUFS2_DIRECT_BLOCKS; d++) {
            if (rd32(b + I_D0 + 4u * d) != 0) { *why = "dir-blocks"; return 0; }
        }
        if (rd32(b + I_IND) != 0) { *why = "dir-ind"; return 0; }
        if (lay->dind_off != 0 && rd32(b + lay->dind_off) != 0) {
            *why = "dir-dind";
            return 0;
        }
    } else if (t == VIMTUFS2_T_FILE) {
        const uint32_t ind = rd32(b + I_IND);
        const uint32_t dind = (lay->dind_off != 0) ? rd32(b + lay->dind_off) : 0u;
        if (ind != 0 && !vf_blk_in_data(vol, ind)) { *why = "ind-range"; return 0; }
        if (dind != 0 && !vf_blk_in_data(vol, dind)) { *why = "dind-range"; return 0; }
        if (dind != 0 && ind == 0) { *why = "dind-without-ind"; return 0; }
        const uint32_t nblk = (size + VIMTUFS2_SECTOR_BYTES - 1u) / VIMTUFS2_SECTOR_BYTES;
        if (nblk > VIMTUFS2_DIRECT_BLOCKS && ind == 0) { *why = "missing-ind"; return 0; }
        if (nblk > VIMTUFS2_L2_FIRST_BLOCK && dind == 0) { *why = "missing-dind"; return 0; }
        if (nblk > VIMTUFS2_MAX_MAP_BLOCKS) { *why = "too-many-blocks"; return 0; }
    }
    return 1;
}

/* ==================== volume geometry ==================== */
static void vf_fill_geometry(struct vimtufs2_vol *vol) {
    vol->inode_blocks = (vol->inodes + (vol->lay->inodes_per_blk) - 1u) /
                        (vol->lay->inodes_per_blk);
}

/* Validate the superblock and fill the geometry.  Mirrors VimtuOS sb_verify()
 * (vfs64.cpp:953-1039): magic -> CRC -> version -> signature -> "the layout
 * must add up exactly" (every derived number is recomputed, the on-disk copies
 * are only used as the claim being checked). */
static int vf_load_geometry(const uint8_t *sb, struct vimtufs2_vol *vol,
                            struct vf_fail *f) {
    const struct vf_layout *lay = NULL;

    if (memcmp(sb + SB_MAGIC, "VIMTUFS2", 8) != 0) {
        f->reason = "magic";
        return VIMTUFS2_ENOTSUP;
    }
    {
        const uint32_t stored = rd32(sb + SB_CRC);
        const uint32_t want = vf_crc32(sb, SB_CRC_LEN);
        if (stored != want) {
            f->reason = "crc";
            f->field = "superblock";
            f->got = stored;
            f->want = want;
            f->has_values = 1;
            return VIMTUFS2_ECORRUPT;
        }
    }
    {
        const uint32_t ver = rd32(sb + SB_VERSION);
        if (ver == VIMTUFS2_VERSION_V5)      lay = &VF_LAYOUT_V5;
        else if (ver == VIMTUFS2_VERSION_V4) lay = &VF_LAYOUT_V4;
        else if (ver == VIMTUFS2_VERSION_V3) lay = &VF_LAYOUT_V3;
        else {
            /* v2 (64 B inodes) is a different layout this port does not read. */
            f->reason = "version";
            f->got = ver;
            f->want = VIMTUFS2_VERSION_V5;
            f->has_values = 1;
            return VIMTUFS2_ENOTSUP;
        }
    }
    if (rd16(sb + SB_SIG) != 0xAA55u) {
        f->reason = "signature";
        f->got = rd16(sb + SB_SIG);
        f->want = 0xAA55u;
        f->has_values = 1;
        return VIMTUFS2_ENOTSUP;
    }
    if (rd32(sb + SB_SECTOR) != VIMTUFS2_SECTOR_BYTES) {
        f->reason = "layout"; f->field = "sector";
        f->got = rd32(sb + SB_SECTOR); f->want = VIMTUFS2_SECTOR_BYTES;
        f->has_values = 1;
        return VIMTUFS2_ENOTSUP;
    }
    if (rd32(sb + SB_BLOCK) != VIMTUFS2_SECTOR_BYTES) {
        f->reason = "layout"; f->field = "block";
        f->got = rd32(sb + SB_BLOCK); f->want = VIMTUFS2_SECTOR_BYTES;
        f->has_values = 1;
        return VIMTUFS2_ENOTSUP;
    }
    if (rd32(sb + SB_INOSZ) != lay->inode_bytes) {
        f->reason = "layout"; f->field = "inode-size";
        f->got = rd32(sb + SB_INOSZ); f->want = lay->inode_bytes;
        f->has_values = 1;
        return VIMTUFS2_ENOTSUP;
    }

    vol->lay = lay;
    vol->blocks = rd32(sb + SB_TOTAL);
    vol->inodes = rd32(sb + SB_INODEN);
    vol->inode_bytes = lay->inode_bytes;
    vol->bitmap_start = rd32(sb + SB_BITMAP);
    vol->bitmap_blocks = rd32(sb + SB_BITMAPN);
    vol->inode_start = rd32(sb + SB_INODE);
    vol->data_start = rd32(sb + SB_DATA);
    vol->data_blocks = rd32(sb + SB_DATAN);

    if (vol->blocks < VIMTUFS2_MIN_BLOCKS) {
        f->reason = "layout"; f->field = "total-blocks";
        f->got = vol->blocks; f->want = VIMTUFS2_MIN_BLOCKS;
        f->has_values = 1;
        return VIMTUFS2_ENOTSUP;
    }
    if (vol->inodes == 0 || vol->inodes > VIMTUFS2_MAX_INODES) {
        f->reason = "layout"; f->field = "inode-count";
        f->got = vol->inodes; f->want = VIMTUFS2_MAX_INODES;
        f->has_values = 1;
        return VIMTUFS2_ENOTSUP;
    }
    if (rd32(sb + SB_ROOT) != 0) {
        /* The spec fixes the root at inode 0 (vfs64_format writes 0). */
        f->reason = "layout"; f->field = "root";
        f->got = rd32(sb + SB_ROOT); f->want = 0;
        f->has_values = 1;
        return VIMTUFS2_ENOTSUP;
    }

    /* Every derived number is recomputed: the volume must be laid out exactly
     * as the spec says (bitmap right after the superblock, inode table right
     * after the bitmap, data area right after the inode table). */
    {
        const uint32_t bmn_calc = (vol->blocks + (VIMTUFS2_SECTOR_BYTES * 8u) - 1u) /
                                  (VIMTUFS2_SECTOR_BYTES * 8u);
        if (vol->bitmap_start != 1u) {
            f->reason = "layout"; f->field = "bitmap-start";
            f->got = vol->bitmap_start; f->want = 1u;
            f->has_values = 1;
            return VIMTUFS2_ENOTSUP;
        }
        if (vol->bitmap_blocks != bmn_calc) {
            f->reason = "layout"; f->field = "bitmap-blocks";
            f->got = vol->bitmap_blocks; f->want = bmn_calc;
            f->has_values = 1;
            return VIMTUFS2_ENOTSUP;
        }
        vf_fill_geometry(vol);
        if (vol->inode_start != vol->bitmap_start + vol->bitmap_blocks) {
            f->reason = "layout"; f->field = "inode-start";
            f->got = vol->inode_start;
            f->want = vol->bitmap_start + vol->bitmap_blocks;
            f->has_values = 1;
            return VIMTUFS2_ENOTSUP;
        }
        if (vol->data_start != vol->inode_start + vol->inode_blocks) {
            f->reason = "layout"; f->field = "data-start";
            f->got = vol->data_start;
            f->want = vol->inode_start + vol->inode_blocks;
            f->has_values = 1;
            return VIMTUFS2_ENOTSUP;
        }
        if (vol->data_start >= vol->blocks) {
            f->reason = "layout"; f->field = "data-start-vs-total";
            f->got = vol->data_start; f->want = vol->blocks;
            f->has_values = 1;
            return VIMTUFS2_ENOTSUP;
        }
        if (vol->data_blocks != vol->blocks - vol->data_start) {
            f->reason = "layout"; f->field = "data-blocks";
            f->got = vol->data_blocks; f->want = vol->blocks - vol->data_start;
            f->has_values = 1;
            return VIMTUFS2_ENOTSUP;
        }
    }

    /* v5 feature extension header (VimtuOS vfs64.h:209-228, vfs64.cpp:995-1027).
     * feature_bits != 0 means the volume carries journal / quota / ACL /
     * snapshot state we cannot honour - refuse with the exact bits. */
    {
        const uint32_t feat = rd32(sb + SB_FEATBITS);
        if (feat != 0) {
            if (lay->version != VIMTUFS2_VERSION_V5) {
                f->reason = "feature";
                f->field = "bits-on-legacy-version";
                f->got = feat;
                f->want = 0;
                f->has_values = 1;
                return VIMTUFS2_ENOTSUP;
            }
            if ((feat & ~VF_FEAT_ALL) != 0) {
                f->reason = "feature";
                f->field = "unknown-bits";
                f->got = feat & ~VF_FEAT_ALL;
                f->want = VF_FEAT_ALL;
                f->has_values = 1;
                return VIMTUFS2_ENOTSUP;
            }
            if (rd32(sb + SB_FEATCRC) != vf_crc32(sb + SB_FEATBITS, SB_FEAT_CRC_LEN)) {
                f->reason = "feature";
                f->field = "crc";
                f->got = rd32(sb + SB_FEATCRC);
                f->want = vf_crc32(sb + SB_FEATBITS, SB_FEAT_CRC_LEN);
                f->has_values = 1;
                return VIMTUFS2_ECORRUPT;
            }
            /* Coherent feature header, features we do not implement. */
            f->reason = "feature";
            f->field = NULL;
            f->feature_bits = feat;
            f->has_values = 1;
            return VIMTUFS2_ENOTSUP;
        }
        /* Feature-free volume: the journal/quota/snapshot block numbers must be
         * zero as well (the reference checks exactly this when feat == 0). */
        for (uint32_t i = SB_FEATBITS; i < SB_FEATCRC; i++) {
            if (sb[i] != 0) {
                f->reason = "feature";
                f->field = "extension-not-zero";
                f->got = i;
                f->want = 0;
                f->has_values = 1;
                return VIMTUFS2_ECORRUPT;
            }
        }
    }
    return VIMTUFS2_OK;
}

/* Count free blocks in the data area from the on-disk bitmap. */
static int vf_count_free(struct vimtufs2_vol *vol, uint32_t *out_free) {
    uint32_t free_n = 0;
    for (uint32_t m = 0; m < vol->bitmap_blocks; ) {
        uint32_t c = vol->bitmap_blocks - m;
        if (c > VF_BITMAP_CHUNK) c = VF_BITMAP_CHUNK;
        int rc = vf_read_blocks(vol, vol->bitmap_start + m, c, g_bitmap_buf);
        if (rc != VIMTUFS2_OK) return rc;
        for (uint32_t s = 0; s < c; s++) {
            const uint8_t *p = g_bitmap_buf + (uint64_t)s * VIMTUFS2_SECTOR_BYTES;
            for (uint32_t k = 0; k < VIMTUFS2_SECTOR_BYTES * 8u; k++) {
                const uint32_t blk = (m + s) * (VIMTUFS2_SECTOR_BYTES * 8u) + k;
                if (blk >= vol->blocks) break;
                if (blk < vol->data_start) continue;
                if (((p[k >> 3] >> (k & 7u)) & 1u) == 0) free_n++;
            }
        }
        m += c;
    }
    *out_free = free_n;
    return VIMTUFS2_OK;
}

/* Open (validate) a volume into a caller-supplied struct; no driver state is
 * published and nothing is logged, so the boot probe can reuse it. */
static int vf_open_volume(int drive, uint32_t start_lba, struct vimtufs2_vol *vol,
                          struct vf_fail *f) {
    memset(vol, 0, sizeof(*vol));
    memset(f, 0, sizeof(*f));
    f->reason = "read";

    if (!blk_find_by_drive(drive)) {
        /* Do not call blk_read() here: it would print its own
         * "[blk] Read failed" line and pollute the boot log of a machine that
         * simply has no second disk. */
        f->reason = "no-device";
        return VIMTUFS2_ENOTSUP;
    }
    vol->gen = ++g_vol_gen;
    vol->drive = drive;
    vol->start_lba = start_lba;

    {
        uint8_t sb[VIMTUFS2_SECTOR_BYTES];
        if (blk_read(drive, (uint64_t)start_lba, 1u, sb) != 0) {
            f->reason = "read";
            return VIMTUFS2_EIO;
        }
        int rc = vf_load_geometry(sb, vol, f);
        if (rc != VIMTUFS2_OK) return rc;
    }

    /* Root inode must be a structurally valid directory. */
    {
        const uint8_t *raw = NULL;
        int rc = vf_inode_raw(vol, 0, &raw);
        if (rc != VIMTUFS2_OK) return rc;
        const char *why = NULL;
        if (!vf_inode_ok(vol, raw, &why)) {
            f->reason = "root-inode";
            f->field = why;
            return VIMTUFS2_ECORRUPT;
        }
        if (raw[I_TYPE] != VIMTUFS2_T_DIR) {
            f->reason = "root-inode";
            f->field = "not-a-directory";
            return VIMTUFS2_ECORRUPT;
        }
    }

    return vf_count_free(vol, &vol->free_blocks);
}

/* ==================== failure text ==================== */
static void vf_feature_names(uint32_t bits, char *out, uint32_t cap) {
    struct { uint32_t bit; const char *name; } names[] = {
        { VF_FEAT_JOURNAL,  "journal"  },
        { VF_FEAT_QUOTA,    "quota"    },
        { VF_FEAT_ACL,      "acl"      },
        { VF_FEAT_SNAPSHOT, "snapshot" }
    };
    uint32_t used = 0;
    out[0] = '\0';
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if ((bits & names[i].bit) == 0) continue;
        int n = snprintf(out + used, cap - used, "%s%s", used ? "|" : "",
                         names[i].name);
        if (n <= 0) break;
        used += (uint32_t)n;
        if (used >= cap) break;
    }
    if (bits & ~VF_FEAT_ALL) {
        snprintf(out + used, cap - used, "%sunknown", used ? "|" : "");
    }
    if (out[0] == '\0') snprintf(out, cap, "none");
}

/* Render a mount failure as one line of text (used by both the mount path and
 * the boot probe, so the wording can never drift between them). */
static void vf_fail_text(const struct vf_fail *f, char *out, uint32_t cap) {
    int n = snprintf(out, cap, "reason=%s", f->reason ? f->reason : "?");
    if (n < 0 || (uint32_t)n >= cap) return;
    if (f->field != NULL) {
        n += snprintf(out + n, cap - (uint32_t)n, " field=%s", f->field);
        if ((uint32_t)n >= cap) return;
    }
    if (!f->has_values) return;
    if (f->feature_bits != 0) {
        char names[48];
        vf_feature_names(f->feature_bits, names, sizeof(names));
        n += snprintf(out + n, cap - (uint32_t)n, " feature_bits=0x%x (%s) unsupported",
                      (uint64_t)f->feature_bits, names);
    } else {
        n += snprintf(out + n, cap - (uint32_t)n, " got=0x%x want=0x%x",
                      (uint64_t)f->got, (uint64_t)f->want);
    }
}

/* ==================== volume lifecycle ==================== */
int vimtufs2_probe(int drive, uint32_t start_lba, struct vimtufs2_vol_info *out) {
    struct vimtufs2_vol vol;
    struct vf_fail f;
    int rc = vf_open_volume(drive, start_lba, &vol, &f);
    if (rc != VIMTUFS2_OK) return rc;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->drive = vol.drive;
        out->start_lba = vol.start_lba;
        out->version = vol.lay->version;
        out->blocks = vol.blocks;
        out->inodes = vol.inodes;
        out->inode_bytes = vol.inode_bytes;
        out->bitmap_start = vol.bitmap_start;
        out->bitmap_blocks = vol.bitmap_blocks;
        out->inode_start = vol.inode_start;
        out->inode_blocks = vol.inode_blocks;
        out->data_start = vol.data_start;
        out->data_blocks = vol.data_blocks;
        out->free_blocks = vol.free_blocks;
    }
    return VIMTUFS2_OK;
}

int vimtufs2_boot_probe(int drive) {
    struct vimtufs2_vol vol;
    struct vf_fail f;
    int rc = vf_open_volume(drive, 0u, &vol, &f);
    if (rc == VIMTUFS2_OK) {
        console_printf("[vimtufs2] volume ready on drive %d: version=%u blocks=%u "
                       "inodes=%u free=%u (read-only; mount with "
                       "\"mount vimtufs2 %d /mnt/v2\")\n",
                       (int64_t)drive, (uint64_t)vol.lay->version,
                       (uint64_t)vol.blocks, (uint64_t)vol.inodes,
                       (uint64_t)vol.free_blocks, (int64_t)drive);
        return 0;
    }
    {
        char why[160];
        vf_fail_text(&f, why, sizeof(why));
        console_printf("[vimtufs2] no volume on drive %d (%s)\n",
                       (int64_t)drive, why);
    }
    return rc;
}

int vimtufs2_mount(int drive, uint32_t start_lba) {
    struct vf_fail f;
    int slot = -1;
    for (int i = 0; i < VIMTUFS2_MAX_MOUNTS; i++) {
        if (!g_vols[i].used) { slot = i; break; }
        if (g_vols[i].drive == drive && g_vols[i].start_lba == start_lba) {
            /* Already mounted: report the slot instead of mounting twice. */
            return i;
        }
    }
    if (slot < 0) {
        console_puts("[vimtufs2] mount FAILED: no free volume slot\n");
        return VIMTUFS2_ENOSPC;
    }
    int rc = vf_open_volume(drive, start_lba, &g_vols[slot], &f);
    if (rc != VIMTUFS2_OK) {
        char why[160];
        vf_fail_text(&f, why, sizeof(why));
        console_printf("[vimtufs2] mount FAILED drive=%d lba=%u (%s)\n",
                       (int64_t)drive, (uint64_t)start_lba, why);
        memset(&g_vols[slot], 0, sizeof(g_vols[slot]));
        return rc;
    }
    g_vols[slot].used = 1;
    g_vols[slot].slot = slot;
    if (g_cur_vol < 0) g_cur_vol = slot;
    console_printf("[vimtufs2] mount drive=%d version=%u blocks=%u inodes=%u "
                   "inode=%uB data=%u+%u free=%u%s\n",
                   (int64_t)drive, (uint64_t)g_vols[slot].lay->version,
                   (uint64_t)g_vols[slot].blocks, (uint64_t)g_vols[slot].inodes,
                   (uint64_t)g_vols[slot].inode_bytes,
                   (uint64_t)g_vols[slot].data_start,
                   (uint64_t)g_vols[slot].data_blocks,
                   (uint64_t)g_vols[slot].free_blocks,
                   (g_vols[slot].lay->version >= VIMTUFS2_VERSION_V4)
                       ? "" : " (legacy v3: uid/gid/mode read as root/0755/0644)");
    return slot;
}

int vimtufs2_unmount(int drive, uint32_t start_lba) {
    for (int i = 0; i < VIMTUFS2_MAX_MOUNTS; i++) {
        if (!g_vols[i].used) continue;
        if (g_vols[i].drive != drive || g_vols[i].start_lba != start_lba) continue;
        /* Close the handles that belong to this volume: the adapter cannot
         * reach them after the slot is gone. */
        for (int k = 0; k < VF_MAX_OPEN_FILES; k++)
            if (g_files[k].used && g_files[k].vol == i) g_files[k].used = 0;
        for (int k = 0; k < VF_MAX_OPEN_DIRS; k++)
            if (g_dirs[k].used && g_dirs[k].vol == i) g_dirs[k].used = 0;
        if (g_cur_vol == i) {
            g_cur_vol = -1;
            for (int k = 0; k < VIMTUFS2_MAX_MOUNTS; k++)
                if (g_vols[k].used && k != i) { g_cur_vol = k; break; }
        }
        memset(&g_vols[i], 0, sizeof(g_vols[i]));
        return VIMTUFS2_OK;
    }
    return VIMTUFS2_ENOENT;
}

int vimtufs2_volume_count(void) {
    int n = 0;
    for (int i = 0; i < VIMTUFS2_MAX_MOUNTS; i++) if (g_vols[i].used) n++;
    return n;
}

int vimtufs2_select(int slot) {
    if (slot < 0 || slot >= VIMTUFS2_MAX_MOUNTS || !g_vols[slot].used) {
        return VIMTUFS2_EINVAL;
    }
    g_cur_vol = slot;
    return VIMTUFS2_OK;
}

int vimtufs2_selected(void) { return g_cur_vol; }

int vimtufs2_is_mounted(int slot) {
    return slot >= 0 && slot < VIMTUFS2_MAX_MOUNTS && g_vols[slot].used;
}

int vimtufs2_volume_info(int slot, struct vimtufs2_vol_info *out) {
    if (!out || !vimtufs2_is_mounted(slot)) return VIMTUFS2_EINVAL;
    struct vimtufs2_vol *vol = &g_vols[slot];
    memset(out, 0, sizeof(*out));
    out->drive = vol->drive;
    out->start_lba = vol->start_lba;
    out->version = vol->lay->version;
    out->blocks = vol->blocks;
    out->inodes = vol->inodes;
    out->inode_bytes = vol->inode_bytes;
    out->bitmap_start = vol->bitmap_start;
    out->bitmap_blocks = vol->bitmap_blocks;
    out->inode_start = vol->inode_start;
    out->inode_blocks = vol->inode_blocks;
    out->data_start = vol->data_start;
    out->data_blocks = vol->data_blocks;
    out->free_blocks = vol->free_blocks;
    return VIMTUFS2_OK;
}

/* ==================== directory lookup ==================== */
/* Find the child of `dir` whose name is exactly `name_len` bytes.  VimtuFS2 has
 * no directory entries: the directory is "every inode whose parent is this
 * inode" (VimtuOS vfs64.h:85-92), so this is a linear scan of the inode table.
 * The table is read 16 sectors (64 inodes) at a time to keep the disk
 * transactions down; a matching inode is fully validated before it is used. */
static int vf_lookup_child(struct vimtufs2_vol *vol, uint32_t dir,
                           const char *name, uint32_t name_len, uint32_t *out_idx) {
    const uint32_t per_chunk = VF_INODE_SCAN_SECTORS * VIMTUFS2_INODES_PER_BLK;
    for (uint32_t base = 0; base < vol->inodes; base += per_chunk) {
        uint32_t n = vol->inodes - base;
        if (n > per_chunk) n = per_chunk;
        uint32_t sectors = (n + VIMTUFS2_INODES_PER_BLK - 1u) / VIMTUFS2_INODES_PER_BLK;
        int rc = vf_read_blocks(vol, vol->inode_start + base / VIMTUFS2_INODES_PER_BLK,
                                sectors, g_scan_buf);
        if (rc != VIMTUFS2_OK) return rc;
        for (uint32_t k = 0; k < n; k++) {
            const uint32_t idx = base + k;
            const uint8_t *raw = g_scan_buf + k * VIMTUFS2_INODE_BYTES;
            if (idx == 0) continue;                  /* root has no name entry */
            if (raw[I_TYPE] == VIMTUFS2_T_FREE) continue;
            if (rd32(raw + I_PARENT) != dir) continue;
            if (raw[I_NAMELEN] != name_len) continue;
            if (memcmp(raw + I_NAME, name, name_len) != 0) continue;
            {
                const char *why = NULL;
                if (!vf_inode_ok(vol, raw, &why)) {
                    console_printf("[vimtufs2] inode %u matches but is invalid "
                                   "(reason=%s) - refusing to use it\n",
                                   (uint64_t)idx, why ? why : "?");
                    return VIMTUFS2_ECORRUPT;
                }
            }
            *out_idx = idx;
            return VIMTUFS2_OK;
        }
    }
    return VIMTUFS2_ENOENT;
}

/* ==================== path resolution ==================== */
/* Absolute or relative-to-root path; '//' collapses, '.' is skipped, '..' walks
 * the parent chain (the root's parent is the root, POSIX semantics - exactly
 * what VimtuOS vfs64.h:94-102 documents). */
static int vf_resolve(struct vimtufs2_vol *vol, const char *path, uint32_t *out_idx) {
    uint32_t cur = 0;
    uint32_t depth = 0;
    const char *p = path;

    if (!path || !out_idx) return VIMTUFS2_EINVAL;
    if (path[0] == '\0') return VIMTUFS2_EINVAL;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') p++;
        const uint32_t len = (uint32_t)(p - seg);

        if (len > VIMTUFS2_NAME_MAX) {
            /* A VimtuFS2 name is at most 31 bytes; this cannot match anything,
             * so say why instead of reporting a plain "not found". */
            console_printf("[vimtufs2] name too long: %u bytes (limit %u) in "
                           "path \"%s\"\n",
                           (uint64_t)len, (uint64_t)VIMTUFS2_NAME_MAX, path);
            return VIMTUFS2_ENAMETOOLONG;
        }
        if (++depth > VIMTUFS2_PATH_DEPTH_MAX) {
            console_printf("[vimtufs2] path too deep (limit %u) in \"%s\"\n",
                           (uint64_t)VIMTUFS2_PATH_DEPTH_MAX, path);
            return VIMTUFS2_EINVAL;
        }

        {
            const uint8_t *raw = NULL;
            int rc = vf_inode_raw(vol, cur, &raw);
            if (rc != VIMTUFS2_OK) return rc;
            if (raw[I_TYPE] != VIMTUFS2_T_DIR) return VIMTUFS2_ENOTDIR;

            if (len == 1 && seg[0] == '.') continue;
            if (len == 2 && seg[0] == '.' && seg[1] == '.') {
                const uint32_t parent = rd32(raw + I_PARENT);
                if (parent >= vol->inodes) {
                    console_printf("[vimtufs2] inode %u: parent %u out of range\n",
                                   (uint64_t)cur, (uint64_t)parent);
                    return VIMTUFS2_ECORRUPT;
                }
                cur = parent;
                continue;
            }
        }

        for (uint32_t i = 0; i < len; i++) {
            const uint8_t c = (uint8_t)seg[i];
            if (c < 0x21u || c > 0x7Eu) {
                console_printf("[vimtufs2] invalid name character 0x%x in \"%s\"\n",
                               (uint64_t)c, path);
                return VIMTUFS2_EINVAL;
            }
        }
        {
            uint32_t child = 0;
            int rc = vf_lookup_child(vol, cur, seg, len, &child);
            if (rc == VIMTUFS2_ENOENT) return VIMTUFS2_ENOENT;
            if (rc != VIMTUFS2_OK) return rc;
            cur = child;
        }
    }
    *out_idx = cur;
    return VIMTUFS2_OK;
}

/* ==================== block mapping (read side) ==================== */
/* One read cursor per call: the pointer blocks are read once each even when the
 * caller walks through all 128 entries they cover (VimtuOS does the same in
 * rmap_get(), vfs64.cpp:2785-2827). */
struct vf_rmap {
    int      ind_loaded;
    uint32_t ind[VIMTUFS2_INDIRECT_PTRS];
    int      dind_loaded;
    uint32_t dind[VIMTUFS2_DIND_CHILDREN];
    uint32_t child_idx;              /* 0xFFFFFFFF = no child loaded */
    uint32_t child[VIMTUFS2_INDIRECT_PTRS];
};
static struct vf_rmap g_rmap;

static void vf_rmap_init(struct vf_rmap *r) {
    memset(r, 0, sizeof(*r));
    r->child_idx = 0xFFFFFFFFu;
}

/* Load one 512 B pointer block (128 little-endian u32) into a table. */
static int vf_load_ptrs(struct vimtufs2_vol *vol, uint32_t blk, uint32_t *out,
                        const char *what) {
    if (!vf_blk_in_data(vol, blk)) {
        console_printf("[vimtufs2] %s block %u is outside the data area "
                       "[%u,%u)\n", what, (uint64_t)blk,
                       (uint64_t)vol->data_start, (uint64_t)vol->blocks);
        return VIMTUFS2_ECORRUPT;
    }
    int rc = vf_read_block(vol, blk, g_ptr_buf);
    if (rc != VIMTUFS2_OK) return rc;
    for (uint32_t i = 0; i < VIMTUFS2_INDIRECT_PTRS; i++) {
        out[i] = rd32(g_ptr_buf + 4u * i);
    }
    return VIMTUFS2_OK;
}

/* Filesystem block i of a file -> volume block (0 = not mapped). */
static int vf_map_get(struct vimtufs2_vol *vol, const uint8_t *ino, uint32_t i,
                      struct vf_rmap *r, uint32_t *out_blk) {
    *out_blk = 0;
    if (i < VIMTUFS2_DIRECT_BLOCKS) {
        *out_blk = rd32(ino + I_D0 + 4u * i);
        return VIMTUFS2_OK;
    }
    if (i < VIMTUFS2_L2_FIRST_BLOCK) {
        if (!r->ind_loaded) {
            const uint32_t b = rd32(ino + I_IND);
            memset(r->ind, 0, sizeof(r->ind));
            if (b != 0) {
                int rc = vf_load_ptrs(vol, b, r->ind, "single-indirect");
                if (rc != VIMTUFS2_OK) return rc;
            }
            r->ind_loaded = 1;
        }
        *out_blk = r->ind[i - VIMTUFS2_DIRECT_BLOCKS];
        return VIMTUFS2_OK;
    }
    if (i >= VIMTUFS2_MAX_MAP_BLOCKS) {
        console_printf("[vimtufs2] fs block index %u beyond the mapping "
                       "capacity %u\n", (uint64_t)i,
                       (uint64_t)VIMTUFS2_MAX_MAP_BLOCKS);
        return VIMTUFS2_ECORRUPT;
    }
    if (vol->lay->dind_off == 0) {
        console_printf("[vimtufs2] double indirect used on a volume without "
                       "that field\n");
        return VIMTUFS2_ECORRUPT;
    }
    {
        const uint32_t child = (i - VIMTUFS2_L2_FIRST_BLOCK) /
                               VIMTUFS2_INDIRECT_PTRS;
        if (!r->dind_loaded) {
            const uint32_t b = rd32(ino + vol->lay->dind_off);
            memset(r->dind, 0, sizeof(r->dind));
            if (b != 0) {
                int rc = vf_load_ptrs(vol, b, r->dind, "double-indirect");
                if (rc != VIMTUFS2_OK) return rc;
            }
            r->dind_loaded = 1;
        }
        if (r->child_idx != child) {
            const uint32_t cb = r->dind[child];
            memset(r->child, 0, sizeof(r->child));
            if (cb != 0) {
                int rc = vf_load_ptrs(vol, cb, r->child, "second-level-indirect");
                if (rc != VIMTUFS2_OK) return rc;
            }
            r->child_idx = child;
        }
        *out_blk = r->child[(i - VIMTUFS2_L2_FIRST_BLOCK) % VIMTUFS2_INDIRECT_PTRS];
    }
    return VIMTUFS2_OK;
}

/* ==================== stat / open / read ==================== */
static void vf_inode_mode(const struct vimtufs2_vol *vol, const uint8_t *raw,
                          uint32_t *out_mode) {
    if (vol->lay->mode_off != 0) {
        *out_mode = rd16(raw + vol->lay->mode_off);
        return;
    }
    /* v3 volumes have no permission fields: the reference reads them as
     * root-owned with the documented default modes. */
    switch (raw[I_TYPE]) {
        case VIMTUFS2_T_DIR:  *out_mode = VF_DEF_DIR_MODE;  break;
        case VIMTUFS2_T_LINK: *out_mode = VF_DEF_LINK_MODE; break;
        default:              *out_mode = VF_DEF_FILE_MODE; break;
    }
}

static void vf_fill_stat(const struct vimtufs2_vol *vol, uint32_t idx,
                         const uint8_t *raw, struct vimtufs2_stat *out) {
    memset(out, 0, sizeof(*out));
    out->index = idx;
    out->type = raw[I_TYPE];
    out->size = (raw[I_TYPE] == VIMTUFS2_T_DIR) ? 0u : rd32(raw + I_SIZE);
    out->mtime = rd32(raw + I_MTIME);
    out->parent = rd32(raw + I_PARENT);
    out->nlink = rd16(raw + I_NLINK);
    out->uid = (vol->lay->uid_off != 0) ? rd16(raw + vol->lay->uid_off) : 0u;
    out->gid = (vol->lay->gid_off != 0) ? rd16(raw + vol->lay->gid_off) : 0u;
    vf_inode_mode(vol, raw, &out->mode);
    out->name_len = raw[I_NAMELEN];
    if (out->name_len > VIMTUFS2_NAME_MAX) out->name_len = VIMTUFS2_NAME_MAX;
    memcpy(out->name, raw + I_NAME, out->name_len);
    out->name[out->name_len] = '\0';
}

static struct vimtufs2_vol *vf_cur(void) {
    if (g_cur_vol < 0 || g_cur_vol >= VIMTUFS2_MAX_MOUNTS) return NULL;
    if (!g_vols[g_cur_vol].used) return NULL;
    return &g_vols[g_cur_vol];
}

int vimtufs2_stat(const char *path, struct vimtufs2_stat *out) {
    struct vimtufs2_vol *vol = vf_cur();
    uint32_t idx = 0;
    const uint8_t *raw = NULL;
    const char *why = NULL;

    if (!out) return VIMTUFS2_EINVAL;
    if (!vol) return VIMTUFS2_EINVAL;
    int rc = vf_resolve(vol, path, &idx);
    if (rc != VIMTUFS2_OK) return rc;
    rc = vf_inode_raw(vol, idx, &raw);
    if (rc != VIMTUFS2_OK) return rc;
    if (!vf_inode_ok(vol, raw, &why)) {
        console_printf("[vimtufs2] stat %s: inode %u is invalid (reason=%s)\n",
                       path, (uint64_t)idx, why ? why : "?");
        return VIMTUFS2_ECORRUPT;
    }
    vf_fill_stat(vol, idx, raw, out);
    return VIMTUFS2_OK;
}

int vimtufs2_open(const char *path, struct vimtufs2_file **out) {
    struct vimtufs2_vol *vol = vf_cur();
    uint32_t idx = 0;
    const uint8_t *raw = NULL;
    const char *why = NULL;
    int slot = -1;

    if (!out) return VIMTUFS2_EINVAL;
    *out = NULL;
    if (!vol) return VIMTUFS2_EINVAL;

    int rc = vf_resolve(vol, path, &idx);
    if (rc != VIMTUFS2_OK) return rc;
    rc = vf_inode_raw(vol, idx, &raw);
    if (rc != VIMTUFS2_OK) return rc;
    if (!vf_inode_ok(vol, raw, &why)) {
        console_printf("[vimtufs2] open %s: inode %u is invalid (reason=%s)\n",
                       path, (uint64_t)idx, why ? why : "?");
        return VIMTUFS2_ECORRUPT;
    }
    if (raw[I_TYPE] == VIMTUFS2_T_DIR) return VIMTUFS2_EISDIR;
    if (raw[I_TYPE] == VIMTUFS2_T_LINK) {
        console_printf("[vimtufs2] open %s: symlinks are not supported by this "
                       "read-only driver\n", path);
        return VIMTUFS2_ENOTSUP;
    }

    for (int i = 0; i < VF_MAX_OPEN_FILES; i++) {
        if (!g_files[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        console_puts("[vimtufs2] open failed: no free file handle\n");
        return VIMTUFS2_ENOSPC;
    }

    g_files[slot].used = 1;
    g_files[slot].vol = g_cur_vol;
    g_files[slot].ino_index = idx;
    g_files[slot].size = rd32(raw + I_SIZE);
    vf_inode_mode(vol, raw, &g_files[slot].mode);

    /* The per-file geometry line this port's spec asks for:
     *   [vimtufs2] file HELLO.TXT inode=1 size=21 blocks=1 */
    {
        char fname[VIMTUFS2_NAME_MAX + 1];
        const uint32_t nlen = raw[I_NAMELEN];
        memcpy(fname, raw + I_NAME, nlen);
        fname[nlen] = '\0';
        console_printf("[vimtufs2] file %s inode=%u size=%u blocks=%u\n",
                       fname, (uint64_t)idx, (uint64_t)g_files[slot].size,
                       (uint64_t)((g_files[slot].size + VIMTUFS2_SECTOR_BYTES - 1u) /
                                  VIMTUFS2_SECTOR_BYTES));
    }
    *out = &g_files[slot];
    return VIMTUFS2_OK;
}

int vimtufs2_read(struct vimtufs2_file *f, void *buf, uint32_t len, uint32_t off) {
    if (!f || !f->used) return VIMTUFS2_EINVAL;
    if (!buf && len != 0) return VIMTUFS2_EINVAL;
    if (len == 0) return 0;
    if (off >= f->size) return 0;
    if (len > f->size - off) len = f->size - off;

    {
        struct vimtufs2_vol *vol = &g_vols[f->vol];
        /* Copy the inode: the pointer returned above lives in the one-sector
         * window and the block reads below must not invalidate it. */
        uint8_t ino[VIMTUFS2_INODE_BYTES];
        const uint8_t *raw = NULL;
        int rc = vf_inode_raw(vol, f->ino_index, &raw);
        if (rc != VIMTUFS2_OK) return rc;
        memcpy(ino, raw, sizeof(ino));

        vf_rmap_init(&g_rmap);
        uint32_t done = 0;
        while (done < len) {
            const uint32_t pos = off + done;
            const uint32_t fs_blk = pos / VIMTUFS2_SECTOR_BYTES;
            const uint32_t in_blk = pos % VIMTUFS2_SECTOR_BYTES;
            uint32_t want = VIMTUFS2_SECTOR_BYTES - in_blk;
            if (want > len - done) want = len - done;

            uint32_t vol_blk = 0;
            rc = vf_map_get(vol, ino, fs_blk, &g_rmap, &vol_blk);
            if (rc != VIMTUFS2_OK) return (done != 0) ? (int)done : rc;
            if (vol_blk == 0) {
                console_printf("[vimtufs2] inode %u: fs block %u is not mapped "
                               "(file size %u) - volume is corrupt\n",
                               (uint64_t)f->ino_index, (uint64_t)fs_blk,
                               (uint64_t)f->size);
                return (done != 0) ? (int)done : VIMTUFS2_ECORRUPT;
            }
            if (in_blk == 0 && want == VIMTUFS2_SECTOR_BYTES) {
                rc = vf_read_block(vol, vol_blk, (uint8_t *)buf + done);
            } else {
                rc = vf_read_block(vol, vol_blk, g_data_buf);
                if (rc == VIMTUFS2_OK) {
                    memcpy((uint8_t *)buf + done, g_data_buf + in_blk, want);
                }
            }
            if (rc != VIMTUFS2_OK) return (done != 0) ? (int)done : rc;
            done += want;
        }
        return (int)done;
    }
}

int vimtufs2_close(struct vimtufs2_file *f) {
    if (!f || !f->used) return VIMTUFS2_EINVAL;
    f->used = 0;
    return VIMTUFS2_OK;
}

/* ==================== directories ==================== */
int vimtufs2_opendir(const char *path, struct vimtufs2_dir **out) {
    struct vimtufs2_vol *vol = vf_cur();
    uint32_t idx = 0;
    const uint8_t *raw = NULL;
    const char *why = NULL;
    int slot = -1;

    if (!out) return VIMTUFS2_EINVAL;
    *out = NULL;
    if (!vol) return VIMTUFS2_EINVAL;

    int rc = vf_resolve(vol, path, &idx);
    if (rc != VIMTUFS2_OK) return rc;
    rc = vf_inode_raw(vol, idx, &raw);
    if (rc != VIMTUFS2_OK) return rc;
    if (!vf_inode_ok(vol, raw, &why)) {
        console_printf("[vimtufs2] opendir %s: inode %u is invalid (reason=%s)\n",
                       path, (uint64_t)idx, why ? why : "?");
        return VIMTUFS2_ECORRUPT;
    }
    if (raw[I_TYPE] != VIMTUFS2_T_DIR) return VIMTUFS2_ENOTDIR;

    for (int i = 0; i < VF_MAX_OPEN_DIRS; i++) {
        if (!g_dirs[i].used) { slot = i; break; }
    }
    if (slot < 0) return VIMTUFS2_ENOSPC;

    g_dirs[slot].used = 1;
    g_dirs[slot].vol = g_cur_vol;
    g_dirs[slot].dir_ino = idx;
    g_dirs[slot].cursor = 0;
    *out = &g_dirs[slot];
    return VIMTUFS2_OK;
}

int vimtufs2_readdir(struct vimtufs2_dir *d, char *name, uint32_t cap,
                     uint32_t *name_len, int *is_dir) {
    const uint32_t per_chunk = VF_INODE_SCAN_SECTORS * VIMTUFS2_INODES_PER_BLK;

    if (!d || !d->used || !name) return VIMTUFS2_EINVAL;
    {
        struct vimtufs2_vol *vol = &g_vols[d->vol];
        uint32_t idx = d->cursor;

        while (idx < vol->inodes) {
            const uint32_t base = (idx / per_chunk) * per_chunk;
            uint32_t n = vol->inodes - base;
            if (n > per_chunk) n = per_chunk;
            const uint32_t sectors =
                (n + VIMTUFS2_INODES_PER_BLK - 1u) / VIMTUFS2_INODES_PER_BLK;
            int rc = vf_read_blocks(vol, vol->inode_start + base / VIMTUFS2_INODES_PER_BLK,
                                    sectors, g_scan_buf);
            if (rc != VIMTUFS2_OK) { d->cursor = vol->inodes; return rc; }
            while (idx < base + n) {
                const uint8_t *raw = g_scan_buf + (idx - base) * VIMTUFS2_INODE_BYTES;
                const uint32_t this_idx = idx;
                idx++;
                if (this_idx == d->dir_ino) continue;         /* never list itself */
                if (raw[I_TYPE] == VIMTUFS2_T_FREE) continue;
                if (rd32(raw + I_PARENT) != d->dir_ino) continue;
                {
                    const uint32_t nl = raw[I_NAMELEN];
                    const char *why = NULL;
                    if (nl == 0) continue;                    /* root has no name */
                    if (!vf_inode_ok(vol, raw, &why)) {
                        console_printf("[vimtufs2] readdir inode %u: invalid "
                                       "(reason=%s) - skipped\n",
                                       (uint64_t)this_idx, why ? why : "?");
                        continue;
                    }
                    if (cap < nl + 1u) {
                        console_printf("[vimtufs2] readdir: name buffer too small "
                                       "(%u < %u)\n", (uint64_t)cap, (uint64_t)(nl + 1u));
                        d->cursor = this_idx;
                        return VIMTUFS2_ENOSPC;
                    }
                    memcpy(name, raw + I_NAME, nl);
                    name[nl] = '\0';
                    if (name_len) *name_len = nl;
                    if (is_dir) *is_dir = (raw[I_TYPE] == VIMTUFS2_T_DIR) ? 1 : 0;
                    d->cursor = this_idx + 1u;
                    return VIMTUFS2_OK;
                }
            }
        }
        d->cursor = vol->inodes;
    }
    return VIMTUFS2_ENOENT;
}

int vimtufs2_closedir(struct vimtufs2_dir *d) {
    if (!d || !d->used) return VIMTUFS2_EINVAL;
    d->used = 0;
    return VIMTUFS2_OK;
}
