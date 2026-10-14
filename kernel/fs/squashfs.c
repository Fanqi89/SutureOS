/* SutureOS: real SquashFS 4.0 READ-ONLY filesystem driver (COMPRESSOR_GZIP)
 *
 * Upstream references consulted (all read-only; see kernel/fs/squashfs.h for
 * the full provenance list):
 *   - squashfs-tools-ng headers/sources bundled as a reference implementation in
 *       C:\Users\fanqi\Desktop\自研操作系统\CoolPotOS\module\squashfs\
 *         include\sqfs\{super,inode,dir,compressor}.h
 *         src\{read_super,read_inode,read_table,dir_reader,data_reader,meta_reader}.c
 *     (LGPL-3.0-or-later, David Oberhollenzer - used for the on-disk layout facts)
 *   - CoolPotOS VFS wrapper: module\squashfs\squashfs.{c,h} (MIT)
 *   - The pre-existing SutureOS stub this file replaces, and kernel/fs/ext2.c
 *     (the shape this driver follows: static context, heap scratch, [prefix]
 *     logging, built-in guest selftest).
 *
 * gzip decompression is delegated to the SHARED RFC 1951 inflate in
 * kernel/pkg/inflate.c (extracted from kernel/pkg/deb.c) - there is exactly one
 * inflate implementation in this tree.
 *
 * READ-ONLY: there is no write path. Mutating entry points fail honestly; the
 * VFS adapter maps SQUASHFS_EROFS onto VFS_EPERM (kernel/vfs/vfs.h has no
 * dedicated EROFS code).
 *
 * ---------------------------------------------------------------------------
 * TEST IMAGE CONTENT RULES (must stay in sync with
 * scripts/mkfs_squashfs_testimg.py and scripts/verify_squashfs_read.py;
 * squashfs_selftest() re-derives them here):
 *
 *   /HELLO.TXT     : "Hello from SutureOS SquashFS!\nSecond line: gzip metadata and data.\n" (67 B)
 *   /README        : "SquashFS 4.0 read-only driver test volume.\n"                          (43 B)
 *   /LARGE.BIN     : byte i == (i * 31 + 7) & 0xFF                 (100 KiB == 25 x 4096)
 *   /UNCOMP.BIN    : byte i == lcg(i) & 0xFF, 8 KiB INCOMPRESSIBLE
 *                    (every 4096-byte block is stored UNCOMPRESSED on disk)
 *   /FRAG.TXT      : byte i == (i * 5 + 1) & 0xFF, 1000 B -> one fragment
 *   /ZERO.BIN      : empty file (0 bytes, no blocks at all)
 *   /LINK.TXT      : symlink to "LARGE.BIN"
 *   /SUB/NESTED.TXT: "Nested file: /SUB/NESTED.TXT via the SquashFS directory table.\n" (63 B)
 *   /SUB/DEEP/A/B/C/DEEP.TXT
 *                  : "Deep directory chain reached through SquashFS.\n"                     (47 B)
 * ---------------------------------------------------------------------------
 */
#include "squashfs.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"
#include "block.h"
#include "inflate.h"

/* ==================== volume state ==================== */

struct squashfs_fs g_squashfs_fs = {0};
struct squashfs_mount g_squashfs_mount = {0};

/* The registered RAM image survives mount/unmount; it belongs to the caller. */
static const uint8_t *g_ram_image = NULL;
static uint32_t g_ram_image_size = 0;

/* Warnings are budgeted per operation-class so a corrupt image cannot flood the
 * serial log. */
static uint32_t g_warn_left = SQUASHFS_WARN_BUDGET;

static struct squashfs_file g_files[SQUASHFS_MAX_OPEN_FILES];
static struct squashfs_dir  g_dirs[SQUASHFS_MAX_OPEN_DIRS];

static int  squashfs_mount_current_device(void);
static void squashfs_teardown(void);
static int  squashfs_resolve(const char *path, uint64_t *out_ref,
                             struct squashfs_stat *out_stat);
static int  squashfs_read_ref_impl(uint64_t ref, void *buf, uint32_t offset, uint32_t size);
static int  squashfs_meta_read(struct squashfs_meta_table *t, uint64_t pos,
                               uint8_t *dst, uint32_t len);

/* ==================== small helpers ==================== */

static uint16_t sq_rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t sq_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t sq_rd64(const uint8_t *p) {
    return (uint64_t)sq_rd32(p) | ((uint64_t)sq_rd32(p + 4) << 32);
}

static uint32_t sq_min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* squashfs_vfs.c assigns raw squashfs_err_t values (like the ext2 adapter does
 * with ext2_err_t), so keep this in sync with the enum in squashfs.h. */
_Static_assert(SQUASHFS_ENOENT == -2, "vfs adapter maps -2 to VFS_ENOTFOUND");
_Static_assert(SQUASHFS_ENOTDIR == -3, "vfs adapter maps -3 to VFS_ENOTDIR");
_Static_assert(SQUASHFS_EISDIR == -4, "vfs adapter maps -4 to VFS_EISDIR");
_Static_assert(SQUASHFS_EINVAL == -5, "vfs adapter maps -5 to VFS_EINVAL");
_Static_assert(SQUASHFS_EIO == -6, "vfs adapter maps -6 to VFS_EIO");
_Static_assert(SQUASHFS_EROFS == -7, "vfs adapter maps -7 to VFS_EPERM");
_Static_assert(SQUASHFS_ENOMEM == -8, "vfs adapter maps -8 to VFS_ENOSPC");
_Static_assert(SQUASHFS_ENOTSUP == -9, "vfs adapter maps -9 to VFS_EPERM");
_Static_assert(SQUASHFS_EBUSY == -10, "vfs adapter maps -10 to VFS_EBUSY");

static void sq_drop_warn(void) {
    if (g_warn_left == 0) return;
    g_warn_left--;
    if (g_warn_left == 0) {
        console_puts("[squashfs] WARNING: further warnings suppressed\n");
    }
}

static void sq_warn1(const char *msg, uint64_t a) {
    if (g_warn_left == 0) return;
    console_printf("[squashfs] WARNING: %s: %u\n", msg, (uint64_t)a);
    sq_drop_warn();
}

static void sq_warn2(const char *msg, uint64_t a, uint64_t b) {
    if (g_warn_left == 0) return;
    console_printf("[squashfs] WARNING: %s: %u %u\n", msg, (uint64_t)a, (uint64_t)b);
    sq_drop_warn();
}

/* ==================== CRC-32 (IEEE 802.3, == Python zlib.crc32) ==================== */

static uint32_t sq_crc_update(uint32_t crc, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc;
}

uint32_t squashfs_crc32(const void *data, uint32_t len) {
    return sq_crc_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

/* ==================== block device ==================== */

/* Read `len` bytes at absolute image byte offset `off` into buf.
 *
 * The RAM path ignores start_lba (the image IS the volume); the block path goes
 * through blk_read(), which transfers WHOLE 512-byte sectors - a short read
 * therefore needs a bounce buffer or it would overrun the caller's buffer.
 * Every access is bounds-checked against image_size. */
static int sq_dev_read(uint64_t off, void *buf, uint32_t len) {
    struct squashfs_fs *fs = &g_squashfs_fs;

    if (!buf || len == 0) return SQUASHFS_EINVAL;
    if (off > fs->image_size || len > fs->image_size - off) {
        sq_warn2("read past the end of the image (offset, len)", off, len);
        return SQUASHFS_ERANGE;
    }
    if (fs->ram) {
        memcpy(buf, fs->ram + off, len);
        return SQUASHFS_OK;
    }
    {
        uint64_t lba = (uint64_t)fs->start_lba + off / 512u;
        uint32_t delta = (uint32_t)(off % 512u);
        uint32_t sectors = (delta + len + 511u) / 512u;

        /* Fast path: a whole number of sectors exactly on a sector boundary
         * (this is what the metadata and data block reads use). */
        if (delta == 0 && (len % 512u) == 0) {
            if (blk_read(fs->drive, lba, sectors, buf) != 0) {
                sq_warn1("block read failed at lba", lba);
                return SQUASHFS_EIO;
            }
            return SQUASHFS_OK;
        }
        /* Everything else bounces through meta_raw, which is at least 4 KiB -
         * but the FULL sector count is what blk_read transfers, so check the
         * bounce buffer against the sector-aligned length, not `len`. */
        {
            uint8_t *tmp = fs->meta_raw;
            uint32_t need = sectors * 512u;
            if (!tmp || fs->meta_buf == NULL) return SQUASHFS_EINVAL;
            if (need > SQUASHFS_METADATA_SIZE) {
                sq_warn2("unaligned read larger than the bounce buffer", off, need);
                return SQUASHFS_EINVAL;
            }
            if (blk_read(fs->drive, lba, sectors, tmp) != 0) {
                sq_warn1("block read failed at lba", lba);
                return SQUASHFS_EIO;
            }
            memcpy(buf, tmp + delta, len);
        }
    }
    return SQUASHFS_OK;
}

/* ==================== decompression ==================== */

int squashfs_decompress_block(uint16_t algo, int compressed,
                              const uint8_t *src, uint32_t src_len,
                              uint8_t *dst, uint32_t dst_cap) {
    if (!compressed) {
        if (src_len > dst_cap) return SQUASHFS_ERANGE;
        memcpy(dst, src, src_len);
        return (int)src_len;
    }
    if (algo != SQUASHFS_COMP_GZIP) {
        /* Already rejected at mount; this is defence in depth. */
        return SQUASHFS_ENOTSUP;
    }
    /* SquashFS gzip blocks are raw deflate, but some producers wrap them in a
     * zlib header. Try raw first, then the zlib wrapper. */
    {
        int n = inflate_raw(src, src_len, dst, dst_cap);
        if (n >= 0) return n;
    }
    {
        int n = inflate_zlib(src, src_len, dst, dst_cap);
        if (n >= 0) return n;
    }
    return SQUASHFS_EIO;
}

/* ==================== metadata blocks / tables ==================== */

/* Load (and decompress) metadata block #index of `t` into dst (dst_cap bytes).
 * A metadata block is: 2-byte header, bit 15 set == stored uncompressed,
 * bits 14:0 == on-disk size. The decompressed block is at most 8192 bytes. */
static int sq_meta_block_load(struct squashfs_meta_table *t, uint32_t index,
                              uint8_t *dst, uint32_t dst_cap, uint32_t *out_len) {
    struct squashfs_fs *fs = &g_squashfs_fs;
    uint64_t off;
    uint8_t hdr[2];
    uint16_t h;
    uint32_t on_disk;
    int compressed, n;

    if (!t || !t->block_offsets || index >= t->block_count) {
        sq_warn2("metadata block index out of range", index,
                 t ? t->block_count : 0);
        return SQUASHFS_ERANGE;
    }
    off = t->block_offsets[index];
    if (sq_dev_read(off, hdr, 2) != SQUASHFS_OK) return SQUASHFS_EIO;
    h = sq_rd16(hdr);
    compressed = (h & SQUASHFS_COMPRESSED_BIT) ? 0 : 1;
    on_disk = (uint32_t)(h & ~SQUASHFS_COMPRESSED_BIT);
    if (on_disk == 0 || on_disk > SQUASHFS_METADATA_SIZE) {
        sq_warn2("bad metadata block size", on_disk, off);
        return SQUASHFS_ERANGE;
    }
    if (sq_dev_read(off + 2, fs->meta_raw, on_disk) != SQUASHFS_OK) return SQUASHFS_EIO;

    if (compressed) {
        n = squashfs_decompress_block(fs->compression, 1, fs->meta_raw, on_disk,
                                      dst, dst_cap);
        if (n < 0) {
            sq_warn2("metadata block failed to decompress at offset", off, (uint64_t)n);
            return SQUASHFS_EIO;
        }
    } else {
        if (on_disk > dst_cap) {
            sq_warn2("stored metadata block larger than the buffer", on_disk, dst_cap);
            return SQUASHFS_ERANGE;
        }
        memcpy(dst, fs->meta_raw, on_disk);
        n = (int)on_disk;
    }
    /* A metadata block is a logical unit of at most 8192 bytes; the last block
     * of a table may be shorter. */
    if (out_len) *out_len = (uint32_t)n;
    return SQUASHFS_OK;
}

/* Read `len` bytes at logical (uncompressed) offset `pos` out of a metadata
 * table. Walks at most ceil(len / 1) blocks because each iteration consumes at
 * least one byte and the loop is bounded by `len`. */
static int squashfs_meta_read(struct squashfs_meta_table *t, uint64_t pos,
                              uint8_t *dst, uint32_t len) {
    uint32_t done = 0;
    if (!dst && len) return SQUASHFS_EINVAL;

    while (done < len) {
        uint32_t index = (uint32_t)(pos / SQUASHFS_METADATA_SIZE);
        uint32_t inner = (uint32_t)(pos % SQUASHFS_METADATA_SIZE);
        uint32_t blen = 0;
        uint32_t chunk;
        int r;

        if (index >= t->block_count) return SQUASHFS_ERANGE;
        r = sq_meta_block_load(t, index, g_squashfs_fs.meta_buf,
                               SQUASHFS_METADATA_SIZE, &blen);
        if (r != SQUASHFS_OK) return r;
        if (inner >= blen) {
            /* Past the end of the last (short) block. */
            sq_warn2("metadata offset past the end of a block", pos, blen);
            return SQUASHFS_ERANGE;
        }
        chunk = sq_min_u32(blen - inner, len - done);
        memcpy(dst + done, g_squashfs_fs.meta_buf + inner, chunk);
        done += chunk;
        pos += chunk;
    }
    return SQUASHFS_OK;
}

/* Build the metadata-block offset stream for a table by WALKING it: each
 * metadata block stores its own on-disk length in its 2-byte header, so the
 * chain is followed from `first_block` until `span_hi` (or the cap) is reached.
 * The inode and directory tables have no lookup list in SquashFS 4.0 - only the
 * fragment table and the id table do (see sq_table_lookup). */
static int sq_table_walk(struct squashfs_meta_table *t, const char *what,
                         uint64_t first_block, uint64_t span_hi) {
    struct squashfs_fs *fs = &g_squashfs_fs;
    uint64_t span, cap;
    uint8_t *raw;
    uint64_t off = first_block;

    t->block_offsets = NULL;
    t->block_count = 0;
    t->capacity = 0;

    if (first_block == (uint64_t)-1 || first_block >= span_hi) {
        console_printf("[squashfs] mount failed: %s table is empty or has no room\n", what);
        return SQUASHFS_ERR;
    }
    span = span_hi - first_block;
    cap = span / 3u;                    /* a metadata block is >= 3 bytes on disk */
    if (cap == 0 || cap > SQUASHFS_MAX_TABLE_BLOCKS) {
        console_printf("[squashfs] mount failed: %s table span %u is out of range\n",
                       what, span);
        return SQUASHFS_ERANGE;
    }

    t->block_offsets = (uint64_t *)lemis_heap_alloc((uintptr_t)cap * sizeof(uint64_t));
    if (!t->block_offsets) {
        console_printf("[squashfs] mount failed: out of memory for the %s table\n", what);
        return SQUASHFS_ENOMEM;
    }
    t->capacity = (uint32_t)cap;
    raw = (uint8_t *)lemis_heap_alloc(2u);
    if (!raw) {
        console_printf("[squashfs] mount failed: out of memory for the %s header\n", what);
        return SQUASHFS_ENOMEM;
    }

    while (off < span_hi && t->block_count < t->capacity) {
        uint16_t hdr;
        uint32_t size;
        if (sq_dev_read(off, raw, 2) != SQUASHFS_OK) {
            lemis_heap_free(raw);
            console_printf("[squashfs] mount failed: cannot read the %s table header\n",
                           what);
            return SQUASHFS_EIO;
        }
        hdr = sq_rd16(raw);
        size = (uint32_t)(hdr & ~SQUASHFS_COMPRESSED_BIT);
        if (size == 0 || size > SQUASHFS_METADATA_SIZE ||
            off + 2u + size > span_hi) {
            lemis_heap_free(raw);
            console_printf("[squashfs] mount failed: %s metadata block at %u has a "
                           "bad size %u\n", what, off, size);
            return SQUASHFS_ERANGE;
        }
        t->block_offsets[t->block_count++] = off;
        off += 2u + size;
    }
    lemis_heap_free(raw);
    if (t->block_count == 0) {
        console_printf("[squashfs] mount failed: the %s table has no metadata blocks\n",
                       what);
        return SQUASHFS_ERANGE;
    }
    (void)fs;
    return SQUASHFS_OK;
}

/* Read the lookup list of `blocks` u64 image offsets that precedes a table's
 * metadata blocks (fragment table and id table only). */
static int sq_table_lookup(struct squashfs_meta_table *t, const char *what,
                           uint64_t lookup_at, uint32_t blocks,
                           uint64_t lower_bound, uint64_t upper_bound) {
    uint64_t span = (uint64_t)blocks * 8u;
    uint8_t *raw;

    t->block_offsets = NULL;
    t->block_count = 0;
    t->capacity = 0;

    if (blocks == 0) return SQUASHFS_OK;
    if (blocks > SQUASHFS_MAX_TABLE_BLOCKS) {
        console_printf("[squashfs] mount failed: %s table needs %u metadata blocks "
                       "(max %u)\n", what, (uint64_t)blocks,
                       (uint64_t)SQUASHFS_MAX_TABLE_BLOCKS);
        return SQUASHFS_ERANGE;
    }
    if (lookup_at == (uint64_t)-1 || lookup_at + span > g_squashfs_fs.image_size) {
        console_printf("[squashfs] mount failed: %s lookup list is outside the image\n",
                       what);
        return SQUASHFS_ERANGE;
    }
    raw = (uint8_t *)lemis_heap_alloc((uintptr_t)span);
    if (!raw) {
        console_printf("[squashfs] mount failed: out of memory for the %s lookup list\n",
                       what);
        return SQUASHFS_ENOMEM;
    }
    if (sq_dev_read(lookup_at, raw, (uint32_t)span) != SQUASHFS_OK) {
        lemis_heap_free(raw);
        console_printf("[squashfs] mount failed: cannot read the %s lookup list\n", what);
        return SQUASHFS_EIO;
    }
    t->block_offsets = (uint64_t *)lemis_heap_alloc((uintptr_t)span);
    if (!t->block_offsets) {
        lemis_heap_free(raw);
        return SQUASHFS_ENOMEM;
    }
    t->capacity = blocks;
    for (uint32_t i = 0; i < blocks; i++) {
        uint64_t off = sq_rd64(raw + i * 8u);
        if (off < lower_bound || off >= upper_bound) {
            lemis_heap_free(raw);
            console_printf("[squashfs] mount failed: %s table block %u at offset %u is "
                           "outside its range [%u,%u)\n", what, (uint64_t)i, off,
                           lower_bound, upper_bound);
            return SQUASHFS_ERANGE;
        }
        t->block_offsets[i] = off;
    }
    lemis_heap_free(raw);
    t->block_count = blocks;
    return SQUASHFS_OK;
}

static void sq_table_free(struct squashfs_meta_table *t) {
    if (t->block_offsets) {
        lemis_heap_free(t->block_offsets);
        t->block_offsets = NULL;
    }
    t->block_count = 0;
    t->capacity = 0;
}

/* ==================== inode parsing ==================== */

/* Read the inode that `ref` points at: the block part selects the metadata
 * block of the inode table, the low 16 bits are a byte offset into that
 * decompressed block. */
static int sq_inode_load(uint64_t ref, struct squashfs_base_inode *hdr,
                         uint8_t *buf, uint32_t buf_cap, uint32_t *out_len) {
    struct squashfs_fs *fs = &g_squashfs_fs;
    uint64_t block = ref >> 16;
    uint32_t offset = (uint32_t)(ref & 0xFFFFu);
    uint32_t blen = 0;
    uint32_t want;
    int r;

    if (block >= fs->inode_table.block_count) {
        sq_warn2("inode reference points past the inode table", ref, block);
        return SQUASHFS_ERANGE;
    }
    r = sq_meta_block_load(&fs->inode_table, (uint32_t)block, fs->meta_buf,
                           SQUASHFS_METADATA_SIZE, &blen);
    if (r != SQUASHFS_OK) return r;
    if (offset + sizeof(struct squashfs_base_inode) > blen) {
        sq_warn2("inode offset past the end of its metadata block", offset, blen);
        return SQUASHFS_ERANGE;
    }
    *hdr = *(const struct squashfs_base_inode *)(const void *)(fs->meta_buf + offset);

    /* The caller wants everything after the header, up to its own capacity. */
    want = blen - offset;
    if (want > buf_cap) want = buf_cap;
    memcpy(buf, fs->meta_buf + offset, want);
    if (out_len) *out_len = want;
    return SQUASHFS_OK;
}

/* `st` is filled from an inode reference. Only the common fields plus the
 * type-specific size are needed; the block list is loaded separately on open. */
static int sq_inode_stat(uint64_t ref, struct squashfs_stat *st) {
    struct squashfs_fs *fs = &g_squashfs_fs;
    struct squashfs_base_inode hdr;
    uint8_t buf[256];
    uint32_t len = 0;
    int r;

    if (!st) return SQUASHFS_EINVAL;
    r = sq_inode_load(ref, &hdr, buf, sizeof(buf), &len);
    if (r != SQUASHFS_OK) return r;

    memset(st, 0, sizeof(*st));
    st->inode_ref = ref;
    st->inode_number = hdr.inode_number;
    st->type = hdr.inode_type;
    st->mode = hdr.mode;
    st->mtime = hdr.mtime;
    st->uid = hdr.uid_idx;
    st->gid = hdr.gid_idx;

    switch (hdr.inode_type) {
        case SQUASHFS_INODE_DIR: {
            struct squashfs_reg_dir_inode in;
            if (len < sizeof(hdr) + sizeof(in)) return SQUASHFS_ERANGE;
            memcpy(&in, buf + sizeof(hdr), sizeof(in));
            st->is_dir = 1;
            st->nlink = in.nlink;
            st->size = in.size;
            break;
        }
        case SQUASHFS_INODE_EXT_DIR: {
            struct squashfs_lreg_dir_inode in;
            if (len < sizeof(hdr) + sizeof(in)) return SQUASHFS_ERANGE;
            memcpy(&in, buf + sizeof(hdr), sizeof(in));
            st->is_dir = 1;
            st->nlink = in.nlink;
            st->size = in.size;
            break;
        }
        case SQUASHFS_INODE_FILE: {
            struct squashfs_reg_file_inode in;
            if (len < sizeof(hdr) + sizeof(in)) return SQUASHFS_ERANGE;
            memcpy(&in, buf + sizeof(hdr), sizeof(in));
            st->is_file = 1;
            st->size = in.file_size;
            break;
        }
        case SQUASHFS_INODE_EXT_FILE: {
            struct squashfs_lreg_file_inode in;
            if (len < sizeof(hdr) + sizeof(in)) return SQUASHFS_ERANGE;
            memcpy(&in, buf + sizeof(hdr), sizeof(in));
            st->is_file = 1;
            st->size = in.file_size;
            break;
        }
        case SQUASHFS_INODE_SYMLINK: {
            struct squashfs_symlink_inode in;
            if (len < sizeof(hdr) + sizeof(in)) return SQUASHFS_ERANGE;
            memcpy(&in, buf + sizeof(hdr), sizeof(in));
            st->is_symlink = 1;
            st->nlink = in.nlink;
            st->size = in.target_size;
            break;
        }
        case SQUASHFS_INODE_EXT_SYMLINK: {
            /* SutureOS fix: an extended symlink inode is 8 bytes of payload PLUS
             * a 256-byte target/offset field (264 total), so copying the whole
             * `struct squashfs_lsymlink_inode` out of a 256-byte buffer both read
             * past the end and made the guard unsatisfiable (len could never
             * reach sizeof(hdr)+264), i.e. every extended symlink failed stat()
             * with ERANGE. stat() only needs nlink and target_size. */
            uint32_t nlink, tsize;
            if (len < sizeof(hdr) + 8u) return SQUASHFS_ERANGE;
            memcpy(&nlink, buf + sizeof(hdr), 4u);
            memcpy(&tsize, buf + sizeof(hdr) + 4u, 4u);
            st->is_symlink = 1;
            st->nlink = nlink;
            st->size = tsize;
            break;
        }
        case SQUASHFS_INODE_BLKDEV:
        case SQUASHFS_INODE_CHRDEV: {
            struct squashfs_dev_inode in;
            if (len < sizeof(hdr) + sizeof(in)) return SQUASHFS_ERANGE;
            memcpy(&in, buf + sizeof(hdr), sizeof(in));
            st->nlink = in.nlink;
            break;
        }
        case SQUASHFS_INODE_EXT_BLKDEV:
        case SQUASHFS_INODE_EXT_CHRDEV: {
            struct squashfs_ldev_inode in;
            if (len < sizeof(hdr) + sizeof(in)) return SQUASHFS_ERANGE;
            memcpy(&in, buf + sizeof(hdr), sizeof(in));
            st->nlink = in.nlink;
            break;
        }
        case SQUASHFS_INODE_FIFO:
        case SQUASHFS_INODE_SOCKET: {
            struct squashfs_ipc_inode in;
            if (len < sizeof(hdr) + sizeof(in)) return SQUASHFS_ERANGE;
            memcpy(&in, buf + sizeof(hdr), sizeof(in));
            st->nlink = in.nlink;
            break;
        }
        case SQUASHFS_INODE_EXT_FIFO:
        case SQUASHFS_INODE_EXT_SOCKET: {
            struct squashfs_lipc_inode in;
            if (len < sizeof(hdr) + sizeof(in)) return SQUASHFS_ERANGE;
            memcpy(&in, buf + sizeof(hdr), sizeof(in));
            st->nlink = in.nlink;
            break;
        }
        default:
            sq_warn1("unknown inode type", hdr.inode_type);
            return SQUASHFS_ENOTSUP;
    }
    (void)fs;
    return SQUASHFS_OK;
}

/* Load the data-block list of a regular file (basic or extended inode) into
 * f->block_sizes. Also records blocks_start / fragment info / size. */
static int sq_file_load_blocks(struct squashfs_file *f) {
    struct squashfs_fs *fs = &g_squashfs_fs;
    struct squashfs_base_inode hdr;
    uint8_t buf[64];
    uint32_t len = 0;
    uint64_t file_size = 0;
    uint32_t frag_idx = SQUASHFS_INVALID_FRAG, frag_off = SQUASHFS_INVALID_BLK;
    uint64_t blocks_start = 0;
    uint32_t blocks;
    uint8_t *list_raw = NULL;
    uint32_t list_len;
    int r;

    r = sq_inode_load(f->inode_ref, &hdr, buf, sizeof(buf), &len);
    if (r != SQUASHFS_OK) return r;
    if (hdr.inode_type == SQUASHFS_INODE_FILE) {
        struct squashfs_reg_file_inode in;
        if (len < sizeof(hdr) + sizeof(in)) return SQUASHFS_ERANGE;
        memcpy(&in, buf + sizeof(hdr), sizeof(in));
        blocks_start = in.blocks_start;
        frag_idx = in.fragment_idx;
        frag_off = in.fragment_offset;
        file_size = in.file_size;
    } else if (hdr.inode_type == SQUASHFS_INODE_EXT_FILE) {
        struct squashfs_lreg_file_inode in;
        if (len < sizeof(hdr) + sizeof(in)) return SQUASHFS_ERANGE;
        memcpy(&in, buf + sizeof(hdr), sizeof(in));
        blocks_start = in.blocks_start;
        frag_idx = in.fragment_idx;
        frag_off = in.fragment_offset;
        file_size = in.file_size;
    } else {
        return SQUASHFS_ENOTSUP;
    }

    if (file_size > 0xFFFFFFFFull) {
        sq_warn1("file larger than 4 GiB is not supported", f->inode_ref);
        return SQUASHFS_ENOTSUP;
    }
    if (frag_idx == SQUASHFS_INVALID_FRAG) frag_idx = SQUASHFS_INVALID_FRAG;
    if (frag_idx == SQUASHFS_INVALID_FRAG && frag_off != SQUASHFS_INVALID_BLK) {
        sq_warn2("fragment offset set without a fragment index", f->inode_ref, frag_off);
        return SQUASHFS_ERANGE;
    }
    if (frag_idx != SQUASHFS_INVALID_FRAG) {
        if (frag_idx >= fs->fragment_count) {
            sq_warn2("fragment index out of range", frag_idx, fs->fragment_count);
            return SQUASHFS_ERANGE;
        }
        if (frag_off > fs->block_size) {
            sq_warn2("fragment offset out of range", frag_off, fs->block_size);
            return SQUASHFS_ERANGE;
        }
    }

    {
        uint64_t full = file_size / fs->block_size;
        uint64_t count = full + ((frag_idx == SQUASHFS_INVALID_FRAG && (file_size % fs->block_size)) ? 1u : 0u);
        if (count > SQUASHFS_MAX_FILE_BLOCKS) {
            console_printf("[squashfs] file has %u data blocks (max %u supported)\n",
                           count, (uint64_t)SQUASHFS_MAX_FILE_BLOCKS);
            return SQUASHFS_ENOTSUP;
        }
        blocks = (uint32_t)count;
    }

    list_len = blocks * 4u;
    if (list_len > 0) {
        list_raw = (uint8_t *)lemis_heap_alloc(list_len);
        if (!list_raw) return SQUASHFS_ENOMEM;
        /* The block list lives in the inode itself, i.e. in the inode table's
         * metadata stream right after the inode header+payload. */
        r = squashfs_meta_read(&fs->inode_table,
                               (f->inode_ref >> 16) * (uint64_t)SQUASHFS_METADATA_SIZE +
                               (uint64_t)(f->inode_ref & 0xFFFFu) +
                               (uint64_t)sizeof(struct squashfs_base_inode) +
                               (uint64_t)((hdr.inode_type == SQUASHFS_INODE_FILE)
                                          ? sizeof(struct squashfs_reg_file_inode)
                                          : sizeof(struct squashfs_lreg_file_inode)),
                               list_raw, list_len);
        if (r != SQUASHFS_OK) {
            lemis_heap_free(list_raw);
            return r;
        }
        for (uint32_t i = 0; i < blocks; i++) {
            f->block_sizes[i] = sq_rd32(list_raw + i * 4u);
        }
        lemis_heap_free(list_raw);
    }

    f->size = file_size;
    f->block_count = blocks;
    f->blocks_start = blocks_start;
    f->fragment_idx = frag_idx;
    f->fragment_offset = frag_off;
    f->inode_number = hdr.inode_number;

    /* A fragment block list entry is (fragment size) for the fragment itself;
     * validate that the referenced fragment exists and is sane. */
    if (frag_idx != SQUASHFS_INVALID_FRAG) {
        struct squashfs_fragment_entry fe;
        uint8_t raw[sizeof(fe)];
        if (squashfs_meta_read(&fs->frag_table, (uint64_t)frag_idx * sizeof(fe), raw,
                               sizeof(fe)) != SQUASHFS_OK) {
            return SQUASHFS_ERANGE;
        }
        fe.start_block = sq_rd64(raw);
        fe.size = sq_rd32(raw + 8);
        fe.unused = sq_rd32(raw + 12);
        if (fe.size == 0) {
            /* an all-zero fragment block: legitimate, reads back as zeros */
        } else {
            uint32_t on_disk = fe.size & SQUASHFS_BLOCK_SIZE_MASK;
            if (on_disk == 0 || on_disk > fs->block_size) {
                sq_warn2("fragment block size out of range", on_disk, fs->block_size);
                return SQUASHFS_ERANGE;
            }
            if (fe.start_block > fs->image_size ||
                (uint64_t)on_disk > fs->image_size - fe.start_block) {
                sq_warn2("fragment block outside the image", fe.start_block, on_disk);
                return SQUASHFS_ERANGE;
            }
        }
    }
    return SQUASHFS_OK;
}

/* ==================== directory table ==================== */

/* Read a directory header at logical offset `pos` of the directory table. */
static int sq_dir_header(struct squashfs_fs *fs, uint64_t pos, uint32_t *out_count,
                         uint32_t *out_block, uint32_t *out_inode) {
    uint8_t raw[sizeof(struct squashfs_dir_header)];
    int r = squashfs_meta_read(&fs->dir_table, pos, raw, sizeof(raw));
    if (r != SQUASHFS_OK) return r;
    *out_count = sq_rd32(raw) + 1u;                /* stored off by one */
    *out_block = sq_rd32(raw + 4);
    *out_inode = sq_rd32(raw + 8);
    if (*out_count == 0 || *out_count > 256u) {
        sq_warn2("directory header count out of range", *out_count, pos);
        return SQUASHFS_ERANGE;
    }
    return SQUASHFS_OK;
}

/* Walk one directory looking for name_len bytes of `name`. On success
 * *out_ref is the child's inode reference. */
static int sq_dir_find(const struct squashfs_dir *dir, const char *name,
                       uint32_t name_len, uint64_t *out_ref) {
    struct squashfs_fs *fs = &g_squashfs_fs;
    /* A directory listing starts at (start_block, offset) inside the directory
     * table's *logical* byte stream: start_block selects the uncompressed
     * metadata block, offset is the byte position within it. */
    const uint64_t base = dir->start_block * (uint64_t)SQUASHFS_METADATA_SIZE +
                          (uint64_t)dir->start_offset;
    const uint64_t span = (uint64_t)dir->size;
    uint64_t pos = 0;
    uint32_t headers = 0;

    while (pos < span) {
        uint32_t count = 0, hblock = 0, hinode = 0;
        int r = sq_dir_header(fs, base + pos, &count, &hblock, &hinode);
        if (r != SQUASHFS_OK) return r;
        pos += sizeof(struct squashfs_dir_header);
        if (++headers > SQUASHFS_MAX_DIR_ENTRIES) return SQUASHFS_ERANGE;

        for (uint32_t i = 0; i < count; i++) {
            uint8_t raw[sizeof(struct squashfs_dir_entry)];
            uint32_t nlen;
            if (pos + sizeof(raw) > span) return SQUASHFS_ERANGE;
            r = squashfs_meta_read(&fs->dir_table, base + pos, raw, sizeof(raw));
            if (r != SQUASHFS_OK) return r;
            nlen = sq_rd16(raw + 6) + 1u;          /* stored off by one */
            if (nlen > SQUASHFS_NAME_MAX) return SQUASHFS_ERANGE;
            if (pos + sizeof(raw) + nlen > span) return SQUASHFS_ERANGE;
            if (nlen == name_len) {
                uint8_t nb[SQUASHFS_NAME_MAX];
                r = squashfs_meta_read(&fs->dir_table, base + pos + sizeof(raw),
                                       nb, nlen);
                if (r != SQUASHFS_OK) return r;
                if (memcmp(nb, name, name_len) == 0) {
                    if (out_ref) {
                        *out_ref = ((uint64_t)hblock << 16) |
                                   (uint64_t)sq_rd16(raw);
                    }
                    return SQUASHFS_OK;
                }
            }
            pos += sizeof(raw) + nlen;
        }
    }
    return SQUASHFS_ENOENT;
}

/* ==================== path resolution ==================== */

static int squashfs_resolve(const char *path, uint64_t *out_ref,
                            struct squashfs_stat *out_stat) {
    struct squashfs_fs *fs = &g_squashfs_fs;
    struct squashfs_stat st;
    uint64_t cur = fs->root_inode_ref;
    const char *p;
    int r;

    if (!path) return SQUASHFS_EINVAL;
    r = sq_inode_stat(cur, &st);
    if (r != SQUASHFS_OK) return SQUASHFS_EIO;

    p = path;
    while (*p == '/') p++;
    while (*p) {
        char comp[SQUASHFS_NAME_MAX + 1];
        uint32_t len = 0;
        uint64_t next = 0;

        while (*p && *p != '/') {
            if (len >= SQUASHFS_NAME_MAX) {
                sq_warn1("path component longer than the limit at offset",
                         (uint64_t)(p - path));
                return SQUASHFS_EINVAL;
            }
            comp[len++] = *p++;
        }
        while (*p == '/') p++;
        if (len == 0) continue;
        comp[len] = '\0';

        if (!st.is_dir) return SQUASHFS_ENOTDIR;
        /* Look the component up in the current directory: build a temporary
         * directory view (start/size/offset) straight from the directory inode,
         * then scan the directory table. */
        {
            struct squashfs_dir tmp;
            memset(&tmp, 0, sizeof(tmp));
            {
                struct squashfs_base_inode hdr;
                uint8_t buf[64];
                uint32_t blen = 0;
                r = sq_inode_load(cur, &hdr, buf, sizeof(buf), &blen);
                if (r != SQUASHFS_OK) return r;
                if (hdr.inode_type == SQUASHFS_INODE_DIR) {
                    struct squashfs_reg_dir_inode in;
                    if (blen < sizeof(hdr) + sizeof(in)) return SQUASHFS_ERANGE;
                    memcpy(&in, buf + sizeof(hdr), sizeof(in));
                    tmp.start_block = in.start_block;
                    tmp.start_offset = in.offset;
                    if (in.size < 3u) return SQUASHFS_ERANGE;
                    tmp.size = (uint32_t)in.size - 3u;   /* stored off by +3 */
                } else if (hdr.inode_type == SQUASHFS_INODE_EXT_DIR) {
                    struct squashfs_lreg_dir_inode in;
                    if (blen < sizeof(hdr) + sizeof(in)) return SQUASHFS_ERANGE;
                    memcpy(&in, buf + sizeof(hdr), sizeof(in));
                    tmp.start_block = in.start_block;
                    tmp.start_offset = in.offset;
                    if (in.size < 3u) return SQUASHFS_ERANGE;
                    tmp.size = in.size - 3u;
                } else {
                    return SQUASHFS_ENOTDIR;
                }
            }
            r = sq_dir_find(&tmp, comp, len, &next);
        }
        if (r != SQUASHFS_OK) return (r == SQUASHFS_ENOENT) ? SQUASHFS_ENOENT : r;

        r = sq_inode_stat(next, &st);
        if (r != SQUASHFS_OK) return SQUASHFS_EIO;
        cur = next;
    }

    if (out_ref) *out_ref = cur;
    if (out_stat) *out_stat = st;

    /* A trailing slash demands a directory ("/SUB/" ok, "/HELLO.TXT/" not). */
    {
        size_t plen = strlen(path);
        if (plen > 0 && path[plen - 1] == '/' && !st.is_dir) return SQUASHFS_ENOTDIR;
    }
    return SQUASHFS_OK;
}

/* ==================== data blocks ==================== */

/* Decompress data block `index` of `f` into fs->data_buf, setting cached_*.
 * Returns the number of valid bytes in the buffer. */
static int sq_data_block(struct squashfs_file *f, uint32_t index) {
    struct squashfs_fs *fs = &g_squashfs_fs;
    uint32_t on_disk;
    uint64_t off;
    int compressed, n;

    if (index >= f->block_count) return SQUASHFS_ERANGE;
    if (fs->cached_block == index && fs->cached_len > 0) {
        return (int)fs->cached_len;
    }
    on_disk = f->block_sizes[index];
    if (on_disk == 0) {
        /* sparse block: a whole block of zeros */
        memset(fs->data_buf, 0, fs->block_size);
        fs->cached_block = index;
        fs->cached_len = fs->block_size;
        return (int)fs->block_size;
    }
    compressed = (on_disk & SQUASHFS_COMPRESSED_BIT_BLOCK) ? 0 : 1;
    on_disk &= SQUASHFS_BLOCK_SIZE_MASK;
    if (on_disk == 0 || on_disk > fs->block_size) {
        sq_warn2("data block size out of range", on_disk, fs->block_size);
        return SQUASHFS_ERANGE;
    }
    off = f->blocks_start;
    for (uint32_t i = 0; i < index; i++) {
        uint32_t sz = f->block_sizes[i] & SQUASHFS_BLOCK_SIZE_MASK;
        off += sz;
    }
    if (off > fs->image_size || (uint64_t)on_disk > fs->image_size - off) {
        sq_warn2("data block outside the image at offset", off, on_disk);
        return SQUASHFS_ERANGE;
    }
    if (sq_dev_read(off, fs->data_raw, on_disk) != SQUASHFS_OK) return SQUASHFS_EIO;

    n = squashfs_decompress_block(fs->compression, compressed, fs->data_raw, on_disk,
                                  fs->data_buf, fs->block_size);
    if (n < 0) {
        sq_warn2("data block failed to decompress at offset", off, (uint64_t)n);
        return SQUASHFS_EIO;
    }
    if ((uint32_t)n > fs->block_size) n = (int)fs->block_size;
    if ((uint32_t)n < fs->block_size) {
        /* A full-size data block that decompresses short is unusual (only the
         * final non-fragment block may legitimately be short). Pad the logical
         * block with zeros rather than re-serving bytes of a previous block. */
        sq_warn2("data block decompressed short, zero padding", (uint64_t)n,
                 fs->block_size);
        memset(fs->data_buf + n, 0, fs->block_size - (uint32_t)n);
    }
    fs->cached_block = index;
    fs->cached_len = fs->block_size;
    return (int)fs->block_size;
}

/* Fill one fragment block into fs->frag_buf. Returns the number of valid bytes. */
static int sq_fragment_block(uint32_t frag_idx) {
    struct squashfs_fs *fs = &g_squashfs_fs;
    struct squashfs_fragment_entry fe;
    uint8_t raw[sizeof(fe)];
    uint32_t on_disk;
    int r, n;

    r = squashfs_meta_read(&fs->frag_table, (uint64_t)frag_idx * sizeof(fe), raw,
                           sizeof(fe));
    if (r != SQUASHFS_OK) return r;
    fe.start_block = sq_rd64(raw);
    fe.size = sq_rd32(raw + 8);
    fe.unused = sq_rd32(raw + 12);

    if (fe.size == 0) {
        memset(fs->frag_buf, 0, fs->block_size);
        return (int)fs->block_size;
    }
    on_disk = fe.size & SQUASHFS_BLOCK_SIZE_MASK;
    if (on_disk == 0 || on_disk > fs->block_size) return SQUASHFS_ERANGE;
    if (fe.start_block > fs->image_size ||
        (uint64_t)on_disk > fs->image_size - fe.start_block) {
        sq_warn2("fragment block outside the image", fe.start_block, on_disk);
        return SQUASHFS_ERANGE;
    }
    /* reuse data_raw as the on-disk staging buffer (it is block_size bytes) */
    if (sq_dev_read(fe.start_block, fs->data_raw, on_disk) != SQUASHFS_OK) {
        return SQUASHFS_EIO;
    }
    n = squashfs_decompress_block(fs->compression,
                                  (fe.size & SQUASHFS_COMPRESSED_BIT_BLOCK) ? 0 : 1,
                                  fs->data_raw, on_disk, fs->frag_buf, fs->block_size);
    if (n < 0) {
        sq_warn2("fragment block failed to decompress at offset", fe.start_block,
                 (uint64_t)n);
        return SQUASHFS_EIO;
    }
    if ((uint32_t)n < fs->block_size) {
        memset(fs->frag_buf + n, 0, fs->block_size - (uint32_t)n);
    }
    return (int)fs->block_size;
}

static int squashfs_read_ref_impl(uint64_t ref, void *buf, uint32_t offset,
                                  uint32_t size) {
    struct squashfs_fs *fs = &g_squashfs_fs;
    struct squashfs_file f;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0;
    int r;

    if (!fs->mounted) return SQUASHFS_EBUSY;
    if (!buf && size != 0) return SQUASHFS_EINVAL;

    memset(&f, 0, sizeof(f));
    f.inode_ref = ref;
    f.block_sizes = fs->sizes;
    f.block_sizes_cap = fs->sizes_cap;
    r = sq_file_load_blocks(&f);
    if (r != SQUASHFS_OK) return r;
    if ((uint64_t)offset >= f.size) return 0;
    if ((uint64_t)size > f.size - (uint64_t)offset) size = (uint32_t)(f.size - (uint64_t)offset);

    fs->cached_block = 0xFFFFFFFFu;
    fs->cached_len = 0;

    while (done < size) {
        uint64_t abs = (uint64_t)offset + done;
        uint32_t index = (uint32_t)(abs / fs->block_size);
        uint32_t boff = (uint32_t)(abs % fs->block_size);
        uint32_t chunk = fs->block_size - boff;
        uint8_t *src;

        if (chunk > size - done) chunk = size - done;
        if (index < f.block_count) {
            int n = sq_data_block(&f, index);
            if (n < 0) return n;
            src = fs->data_buf;
        } else if (f.fragment_idx != SQUASHFS_INVALID_FRAG) {
            int n = sq_fragment_block(f.fragment_idx);
            if (n < 0) return n;
            src = fs->frag_buf + f.fragment_offset;
            if ((uint64_t)f.fragment_offset + boff + chunk > (uint64_t)fs->block_size) {
                sq_warn2("fragment read runs past the fragment block",
                         f.fragment_offset, (uint64_t)boff + chunk);
                return SQUASHFS_ERANGE;
            }
        } else {
            break;      /* past the last block: nothing left (returns short) */
        }
        memcpy(dst + done, src + boff, chunk);
        done += chunk;
    }
    return (int)done;
}

/* ==================== mount / unmount ==================== */

static void squashfs_teardown(void) {
    struct squashfs_fs *fs = &g_squashfs_fs;

    sq_table_free(&fs->inode_table);
    sq_table_free(&fs->dir_table);
    sq_table_free(&fs->frag_table);
    sq_table_free(&fs->id_table);

    if (fs->meta_buf) { lemis_heap_free(fs->meta_buf); fs->meta_buf = NULL; }
    if (fs->meta_raw) { lemis_heap_free(fs->meta_raw); fs->meta_raw = NULL; }
    if (fs->data_buf) { lemis_heap_free(fs->data_buf); fs->data_buf = NULL; }
    if (fs->data_raw) { lemis_heap_free(fs->data_raw); fs->data_raw = NULL; }
    if (fs->frag_buf) { lemis_heap_free(fs->frag_buf); fs->frag_buf = NULL; }
    if (fs->sizes)    { lemis_heap_free(fs->sizes);    fs->sizes = NULL; }
    fs->sizes_cap = 0;
    fs->mounted = 0;
    g_squashfs_mount.mounted = false;
}

static int squashfs_mount_fail(const char *reason) {
    console_printf("[squashfs] mount failed: %s\n", reason);
    squashfs_teardown();
    return SQUASHFS_ERR;
}

static int squashfs_mount_fail2(const char *reason, uint32_t a, uint32_t b) {
    char buf[128];
    snprintf(buf, sizeof(buf), reason, (unsigned)a, (unsigned)b);
    return squashfs_mount_fail(buf);
}

/* The id table also has a u64 lookup list, but producers differ on which of the
 * two the superblock's s_id_table_start points at. Neither the list's position
 * relative to its metadata nor the metadata's length is stored, so the layout is
 * identified by VALIDATING the candidate: the list's first entry must be a
 * plausible metadata-block offset, and the bytes at the metadata's first entry
 * must carry a sane metadata-block header whose on-disk length is consistent
 * with the list sitting either just before or just after it.
 *
 * Returns SQUASHFS_OK and fills t, or a negative error. `*out_meta_start`
 * receives the byte just BEFORE the id section, i.e. where the directory table
 * ends: that is the list's own position when the metadata follows the list, and
 * the metadata's position when it precedes the list. */
static int sq_table_lookup_id(struct squashfs_meta_table *t, uint32_t blocks,
                              uint64_t id_lo, uint64_t image_size,
                              uint64_t bytes_used, uint64_t *out_meta_start) {
    uint64_t span = (uint64_t)blocks * 8u;
    uint8_t raw[8 * 8];             /* enough for the first lookup entry */
    uint64_t candidates[2];
    uint32_t n = 0;

    if (blocks == 0 || blocks > (uint32_t)(sizeof(raw) / 8u)) return SQUASHFS_ERANGE;
    candidates[n++] = id_lo;
    if (id_lo >= span) candidates[n++] = id_lo - span;

    for (uint32_t k = 0; k < n; k++) {
        uint64_t list_at = candidates[k];
        uint64_t meta, hdr_at;
        uint16_t hdr;
        uint32_t size;
        int r;

        if (list_at > image_size || span > image_size - list_at) continue;
        if (sq_dev_read(list_at, raw, (uint32_t)span) != SQUASHFS_OK) continue;
        meta = sq_rd64(raw);
        if (meta == 0 || meta > image_size || sizeof(struct squashfs_base_inode) >
            image_size - meta) {
            continue;
        }
        /* Pick the header position the candidate order implies:
         *   metadata at list_at            -> list AFTER  metadata, header = meta
         *   metadata at list_at - span     -> list BEFORE metadata, header = list_at */
        hdr_at = (list_at == id_lo) ? meta : list_at;
        if (hdr_at + 2u > image_size) continue;
        if (sq_dev_read(hdr_at, raw, 2) != SQUASHFS_OK) continue;
        hdr = sq_rd16(raw);
        size = (uint32_t)(hdr & ~SQUASHFS_COMPRESSED_BIT);
        if (size == 0 || size > SQUASHFS_METADATA_SIZE) continue;
        /* the metadata must actually start at the offset the first entry names */
        if (meta != hdr_at) continue;
        if (meta + 2u + size > bytes_used) continue;

        r = sq_table_lookup(t, "id", list_at, blocks, meta, bytes_used);
        if (r != SQUASHFS_OK) {
            sq_table_free(t);
            continue;
        }
        if (out_meta_start) *out_meta_start = (meta < list_at) ? meta : list_at;
        return SQUASHFS_OK;
    }
    return SQUASHFS_ERANGE;
}

int squashfs_register_ramdisk(const void *image, uint32_t size) {
    if (!image || size < sizeof(struct squashfs_super)) {
        console_puts("[squashfs] register_ramdisk failed: image is NULL or too small\n");
        return SQUASHFS_EINVAL;
    }
    g_ram_image = (const uint8_t *)image;
    g_ram_image_size = size;
    return SQUASHFS_OK;
}

int squashfs_ramdisk_registered(void) { return g_ram_image != NULL; }

int squashfs_mount(int drive, uint32_t start_lba) {
    squashfs_teardown();
    g_squashfs_fs.drive = drive;
    g_squashfs_fs.start_lba = start_lba;
    g_squashfs_fs.ram = NULL;
    g_squashfs_fs.ram_size = 0;
    return squashfs_mount_current_device();
}

int squashfs_mount_ramdisk(void) {
    if (!g_ram_image) return squashfs_mount_fail("no ramdisk registered");
    squashfs_teardown();
    g_squashfs_fs.ram = g_ram_image;
    g_squashfs_fs.ram_size = g_ram_image_size;
    g_squashfs_fs.drive = -1;
    g_squashfs_fs.start_lba = 0;
    return squashfs_mount_current_device();
}

int squashfs_unmount(void) {
    squashfs_teardown();
    return SQUASHFS_OK;
}

int squashfs_is_mounted(void) { return g_squashfs_fs.mounted; }
uint32_t squashfs_block_size(void) { return g_squashfs_fs.block_size; }
uint32_t squashfs_compression(void) { return g_squashfs_fs.compression; }
uint32_t squashfs_entry_count(void) { return g_squashfs_fs.inode_count; }

static int squashfs_mount_current_device(void) {
    struct squashfs_fs *fs = &g_squashfs_fs;
    uint8_t *sb_bytes;
    uint64_t sector_count = 0;
    uint32_t sector_size = 0;
    int r;

    g_warn_left = SQUASHFS_WARN_BUDGET;

    sb_bytes = (uint8_t *)lemis_heap_alloc(sizeof(struct squashfs_super));
    if (!sb_bytes) return squashfs_mount_fail("out of memory reading the superblock");

    /* the superblock is the first thing in the image */
    if (fs->ram) {
        if (fs->ram_size < sizeof(struct squashfs_super)) {
            lemis_heap_free(sb_bytes);
            return squashfs_mount_fail("ramdisk image is smaller than a superblock");
        }
        fs->image_size = fs->ram_size;
    } else {
        if (blk_get_info(fs->drive, &sector_count, &sector_size) != 0 ||
            sector_count == 0 || sector_size == 0) {
            lemis_heap_free(sb_bytes);
            return squashfs_mount_fail("unknown block device size");
        }
        if (sector_count <= fs->start_lba) {
            lemis_heap_free(sb_bytes);
            return squashfs_mount_fail("device is smaller than the partition offset");
        }
        fs->image_size = (sector_count - fs->start_lba) * (uint64_t)sector_size;
    }

    /* sq_dev_read() bounces sector-straddling reads through fs->meta_raw, which
     * is only allocated further down; buffers are needed before that, so read
     * the superblock (96 bytes at offset 0, never straddling a sector) directly
     * and allocate the scratch buffers right here. */
    fs->meta_buf = (uint8_t *)lemis_heap_alloc(SQUASHFS_METADATA_SIZE);
    fs->meta_raw = (uint8_t *)lemis_heap_alloc(SQUASHFS_METADATA_SIZE);
    if (!fs->meta_buf || !fs->meta_raw) {
        lemis_heap_free(sb_bytes);
        return squashfs_mount_fail("out of memory for the metadata buffers");
    }
    r = sq_dev_read(0, sb_bytes, sizeof(struct squashfs_super));
    if (r != SQUASHFS_OK) {
        lemis_heap_free(sb_bytes);
        return squashfs_mount_fail("I/O error reading the superblock at offset 0");
    }
    memset(&fs->sb, 0, sizeof(fs->sb));
    memcpy(&fs->sb, sb_bytes, sizeof(fs->sb));
    lemis_heap_free(sb_bytes);

    /* ---- superblock validation (never trust on-disk values) ---- */
    if (fs->sb.magic != SQUASHFS_MAGIC) {
        return squashfs_mount_fail2("invalid magic 0x%x (want 0x73717368)",
                                    fs->sb.magic, 0);
    }
    if (fs->sb.version_major != SQUASHFS_VERSION_MAJOR ||
        fs->sb.version_minor != SQUASHFS_VERSION_MINOR) {
        return squashfs_mount_fail2("unsupported version %u.%u (want 4.0)",
                                    fs->sb.version_major, fs->sb.version_minor);
    }
    if (fs->sb.compression != SQUASHFS_COMP_GZIP) {
        return squashfs_mount_fail2("unsupported compression id %u (only gzip=1 is "
                                    "implemented)", fs->sb.compression, 0);
    }
    {
        uint32_t bs = fs->sb.block_size;
        if (bs < SQUASHFS_MIN_BLOCK_SIZE || bs > SQUASHFS_MAX_BLOCK_SIZE ||
            (bs & (bs - 1u)) != 0) {
            return squashfs_mount_fail2("invalid block size %u (need a power of two "
                                        "in [4096,1048576])", bs, 0);
        }
        /* block_log must agree */
        {
            uint32_t log = 12;
            while ((1u << log) < bs && log < 32) log++;
            if ((1u << log) != bs || (uint32_t)fs->sb.block_log != log) {
                return squashfs_mount_fail2("block_log %u disagrees with block_size log %u",
                                            fs->sb.block_log, log);
            }
        }
        fs->block_size = bs;
    }
    if (fs->sb.bytes_used < sizeof(struct squashfs_super) ||
        fs->sb.bytes_used > fs->image_size) {
        return squashfs_mount_fail2("bytes_used %u is outside the image (%u bytes)",
                                    (uint32_t)fs->sb.bytes_used,
                                    (uint32_t)fs->image_size);
    }
    if (fs->sb.inode_table_start >= fs->sb.bytes_used ||
        fs->sb.directory_table_start >= fs->sb.bytes_used ||
        fs->sb.inode_table_start >= fs->sb.directory_table_start) {
        return squashfs_mount_fail("inode/directory table starts are inconsistent");
    }
    if (fs->sb.id_table_start == 0 || fs->sb.id_table_start >= fs->sb.bytes_used) {
        return squashfs_mount_fail("id_table_start is outside the image");
    }
    if (fs->sb.fragment_entry_count > SQUASHFS_MAX_FRAGMENTS) {
        return squashfs_mount_fail2("too many fragments (%u, max %u)",
                                    fs->sb.fragment_entry_count,
                                    SQUASHFS_MAX_FRAGMENTS);
    }

    fs->flags = fs->sb.flags;
    fs->compression = fs->sb.compression;
    fs->inode_count = fs->sb.inode_count;
    fs->fragment_count = fs->sb.fragment_entry_count;
    fs->id_count = fs->sb.id_count;
    fs->root_inode_ref = fs->sb.root_inode;

    /* ---- scratch buffers ---- */
    fs->data_buf = (uint8_t *)lemis_heap_alloc(fs->block_size);
    fs->data_raw = (uint8_t *)lemis_heap_alloc(fs->block_size);
    fs->frag_buf = (uint8_t *)lemis_heap_alloc(fs->block_size);
    fs->sizes = (uint32_t *)lemis_heap_alloc(SQUASHFS_MAX_FILE_BLOCKS * sizeof(uint32_t));
    if (!fs->data_buf || !fs->data_raw || !fs->frag_buf || !fs->sizes) {
        return squashfs_mount_fail("out of memory for the driver buffers");
    }
    fs->sizes_cap = SQUASHFS_MAX_FILE_BLOCKS;
    fs->cached_block = 0xFFFFFFFFu;
    fs->cached_len = 0;

    /* ---- metadata table discovery ----
     * Only the fragment table and the id table have a u64 lookup list; the
     * inode and directory tables are plain chains of metadata blocks, each
     * carrying its own on-disk length in a 2-byte header.
     *
     * Extents:
     *   inode table     : [inode_table_start, directory_table_start)
     *   directory table : [directory_table_start, <first thing after it>)
     * where the first thing after it is the fragment table (when there are
     * fragments) or the id table (when there are none). Because the id table's
     * metadata may be either side of its lookup list, the id table is resolved
     * FIRST and its metadata start is used as the directory table's end. */
    {
        uint64_t inode_lo = fs->sb.inode_table_start;
        uint64_t dir_lo = fs->sb.directory_table_start;
        uint64_t frag_lo = fs->sb.fragment_table_start;
        uint64_t lookup_lo = fs->sb.lookup_table_start;
        uint64_t id_lo = fs->sb.id_table_start;
        uint64_t dir_hi;
        uint32_t frag_blocks = 0;
        uint32_t id_blocks;
        int r2;

        if (fs->fragment_count > 0) {
            uint64_t bytes = (uint64_t)fs->fragment_count *
                             sizeof(struct squashfs_fragment_entry);
            frag_blocks = (uint32_t)((bytes + SQUASHFS_METADATA_SIZE - 1u) /
                                     SQUASHFS_METADATA_SIZE);
            if (frag_lo == (uint64_t)-1 || lookup_lo == (uint64_t)-1 ||
                frag_lo >= lookup_lo || lookup_lo > fs->sb.bytes_used) {
                return squashfs_mount_fail("fragment_entry_count is set but the "
                                           "fragment/lookup table starts are invalid");
            }
        }
        if (id_lo == (uint64_t)-1 || id_lo > fs->sb.bytes_used) {
            return squashfs_mount_fail("id_table_start is outside the image");
        }
        {
            uint64_t bytes = (uint64_t)fs->id_count * 4u;
            id_blocks = (uint32_t)((bytes + SQUASHFS_METADATA_SIZE - 1u) /
                                   SQUASHFS_METADATA_SIZE);
            if (id_blocks == 0) id_blocks = 1;
        }

        /* Resolve the id table first: its metadata start is the byte after the
         * directory table, which is what bounds the directory walk. */
        {
            uint64_t id_meta = 0;
            r2 = sq_table_lookup_id(&fs->id_table, id_blocks, id_lo, fs->image_size,
                                    fs->sb.bytes_used, &id_meta);
            if (r2 != SQUASHFS_OK) {
                return squashfs_mount_fail("cannot identify the id table layout "
                                           "from s_id_table_start");
            }
            dir_hi = id_meta;
        }
        if (frag_blocks > 0 && frag_lo < dir_hi) dir_hi = frag_lo;
        if (dir_hi <= dir_lo) {
            return squashfs_mount_fail("no room for a directory table between the "
                                       "inode and id/fragment tables");
        }

        r2 = sq_table_walk(&fs->inode_table, "inode", inode_lo, dir_lo);
        if (r2 != SQUASHFS_OK) return r2;
        r2 = sq_table_walk(&fs->dir_table, "directory", dir_lo, dir_hi);
        if (r2 != SQUASHFS_OK) return r2;
        r2 = sq_table_lookup(&fs->frag_table, "fragment", lookup_lo, frag_blocks,
                             frag_lo, lookup_lo);
        if (r2 != SQUASHFS_OK) return r2;
    }

    /* ---- compressor options block (informative; validated only) ---- */
    if (fs->flags & SQUASHFS_FLAG_COMPRESSOR_OPTIONS) {
        struct squashfs_comp_options co;
        if (sq_dev_read(sizeof(struct squashfs_super), &co, sizeof(co)) != SQUASHFS_OK) {
            return squashfs_mount_fail("compressor options block is unreadable");
        }
        console_printf("[squashfs] gzip compressor options: 0x%x 0x%x\n",
                       (uint64_t)co.options, (uint64_t)co.flags);
    }

    /* ---- the root inode must exist and be a directory ---- */
    {
        struct squashfs_stat st;
        r = sq_inode_stat(fs->root_inode_ref, &st);
        if (r != SQUASHFS_OK) return squashfs_mount_fail("cannot read the root inode");
        if (!st.is_dir) return squashfs_mount_fail("the root inode is not a directory");
    }

    fs->mounted = 1;
    g_squashfs_mount.drive = fs->drive;
    g_squashfs_mount.start_lba = fs->start_lba;
    g_squashfs_mount.block_size = fs->block_size;
    g_squashfs_mount.block_log = fs->sb.block_log;
    g_squashfs_mount.inodes = fs->inode_count;
    g_squashfs_mount.fragments = fs->fragment_count;
    g_squashfs_mount.compression = fs->compression;
    g_squashfs_mount.flags = fs->flags;
    g_squashfs_mount.inode_table_start = (uint32_t)fs->sb.inode_table_start;
    g_squashfs_mount.directory_table_start = (uint32_t)fs->sb.directory_table_start;
    g_squashfs_mount.fragment_table_start = (uint32_t)fs->sb.fragment_table_start;
    g_squashfs_mount.lookup_table_start = (uint32_t)fs->sb.lookup_table_start;
    g_squashfs_mount.mounted = true;

    /* THE mount line (exactly one, as specified) */
    console_printf("[squashfs] mount blocksz=%u inodes=%u fragments=%u comp=%u "
                   "image=%u bytes\n",
                   (uint64_t)fs->block_size, (uint64_t)fs->inode_count,
                   (uint64_t)fs->fragment_count, (uint64_t)fs->compression,
                   (uint64_t)fs->sb.bytes_used);
    return SQUASHFS_OK;
}

/* ==================== public file API ==================== */

int squashfs_lookup(const char *path, uint64_t *out_ref) {
    if (!g_squashfs_fs.mounted) return SQUASHFS_EBUSY;
    if (!out_ref) return SQUASHFS_EINVAL;
    return squashfs_resolve(path, out_ref, NULL);
}

int squashfs_stat(const char *path, struct squashfs_stat *out) {
    if (!g_squashfs_fs.mounted) return SQUASHFS_EBUSY;
    if (!out) return SQUASHFS_EINVAL;
    return squashfs_resolve(path, NULL, out);
}

int squashfs_readlink_ref(uint64_t ref, char *buf, uint32_t cap) {
    struct squashfs_fs *fs = &g_squashfs_fs;
    struct squashfs_base_inode hdr;
    /* SutureOS fix: the extended-symlink inode needs the 16-byte base header
     * plus 8 bytes of payload plus the 256-byte target/offset field (280 in
     * total). The old buffer was sizeof(struct squashfs_lsymlink_inode) = 264,
     * which made the guard unsatisfiable and the payload copy read past the
     * end - extended symlinks could never be read. */
    uint8_t tmp[sizeof(struct squashfs_base_inode) + 8u + 256u];
    uint32_t len = 0;
    uint32_t tsize;
    int r;

    if (!buf || cap == 0) return SQUASHFS_EINVAL;
    if (!fs->mounted) return SQUASHFS_EBUSY;
    r = sq_inode_load(ref, &hdr, tmp, sizeof(tmp), &len);
    if (r != SQUASHFS_OK) return r;

    if (hdr.inode_type == SQUASHFS_INODE_SYMLINK) {
        struct squashfs_symlink_inode in;
        if (len < sizeof(hdr) + sizeof(in)) return SQUASHFS_ERANGE;
        memcpy(&in, tmp + sizeof(hdr), sizeof(in));
        tsize = in.target_size;
        if (tsize > SQUASHFS_NAME_MAX) return SQUASHFS_ERANGE;
        /* inline target follows the inode */
        {
            uint8_t nb[SQUASHFS_NAME_MAX + 1];
            uint32_t want = tsize < cap - 1 ? tsize : cap - 1;
            r = squashfs_meta_read(&fs->inode_table,
                                   (ref >> 16) * (uint64_t)SQUASHFS_METADATA_SIZE +
                                   (uint64_t)(ref & 0xFFFFu) + sizeof(hdr) + sizeof(in),
                                   nb, tsize);
            if (r != SQUASHFS_OK) return r;
            memcpy(buf, nb, want);
            buf[want] = '\0';
        }
        return (int)tsize;
    }
    if (hdr.inode_type == SQUASHFS_INODE_EXT_SYMLINK) {
        const uint8_t *in = tmp + sizeof(hdr);      /* inode payload */
        uint32_t tsize;

        if (len < sizeof(hdr) + 8u) return SQUASHFS_ERANGE;
        memcpy(&tsize, in + 4u, 4u);
        if (tsize < 256u) {
            /* target stored inline in the 256-byte field */
            uint32_t want = tsize < cap - 1 ? tsize : cap - 1;
            if (len < sizeof(hdr) + 8u + tsize) return SQUASHFS_ERANGE;
            memcpy(buf, in + 8u, want);
            buf[want] = '\0';
            return (int)tsize;
        }
        /* out-of-line target: the first 8 bytes of the field hold the absolute
         * image offset of the target text */
        if (len < sizeof(hdr) + 8u + 8u) return SQUASHFS_ERANGE;
        {
            uint64_t off = sq_rd64(in + 8u);
            uint32_t want = tsize < cap - 1 ? tsize : cap - 1;
            if (sq_dev_read(off, buf, want) != SQUASHFS_OK) return SQUASHFS_EIO;
            buf[want] = '\0';
        }
        return (int)tsize;
    }
    return SQUASHFS_EINVAL;
}

int squashfs_readlink(const char *path, char *buf, uint32_t cap) {
    uint64_t ref = 0;
    int r = squashfs_lookup(path, &ref);
    if (r != SQUASHFS_OK) return r;
    return squashfs_readlink_ref(ref, buf, cap);
}

int squashfs_read_ref(uint64_t ref, void *buf, uint32_t offset, uint32_t size) {
    struct squashfs_stat st;
    int r;

    if (!g_squashfs_fs.mounted) return SQUASHFS_EBUSY;
    if (!buf && size != 0) return SQUASHFS_EINVAL;
    r = sq_inode_stat(ref, &st);
    if (r != SQUASHFS_OK) return r;
    if (st.is_dir) return SQUASHFS_EISDIR;
    if (!st.is_file) return SQUASHFS_ENOTSUP;
    return squashfs_read_ref_impl(ref, buf, offset, size);
}

int squashfs_open(const char *path, struct squashfs_file **out) {
    uint64_t ref = 0;
    struct squashfs_stat st;
    struct squashfs_file *f = NULL;
    int i, r;

    if (!g_squashfs_fs.mounted) return SQUASHFS_EBUSY;
    if (!out) return SQUASHFS_EINVAL;
    r = squashfs_resolve(path, &ref, &st);
    if (r != SQUASHFS_OK) return r;
    if (st.is_dir) return SQUASHFS_EISDIR;
    if (!st.is_file) return SQUASHFS_ENOTSUP;

    for (i = 0; i < SQUASHFS_MAX_OPEN_FILES; i++) {
        if (!g_files[i].used) { f = &g_files[i]; break; }
    }
    if (!f) return SQUASHFS_EBUSY;

    memset(f, 0, sizeof(*f));
    f->used = 1;
    f->inode_ref = ref;
    f->block_sizes = g_squashfs_fs.sizes;
    f->block_sizes_cap = g_squashfs_fs.sizes_cap;
    r = sq_file_load_blocks(f);
    if (r != SQUASHFS_OK) {
        memset(f, 0, sizeof(*f));
        return r;
    }
    *out = f;
    return SQUASHFS_OK;
}

int squashfs_file_read(struct squashfs_file *f, void *buf, uint32_t size,
                       uint32_t offset) {
    if (!f || !f->used) return SQUASHFS_EINVAL;
    if (!g_squashfs_fs.mounted) return SQUASHFS_EBUSY;
    if ((uint64_t)offset >= f->size) return 0;                 /* EOF */
    if ((uint64_t)size > f->size - (uint64_t)offset) {
        size = (uint32_t)(f->size - (uint64_t)offset);
    }
    return squashfs_read_ref_impl(f->inode_ref, buf, offset, size);
}

int squashfs_close(struct squashfs_file *f) {
    if (!f) return SQUASHFS_EINVAL;
    memset(f, 0, sizeof(*f));
    return SQUASHFS_OK;
}

/* ==================== directories ==================== */

/* Fill a directory handle from the directory inode at `ref`. */
static int sq_dir_open_ref(struct squashfs_dir *d, uint64_t ref) {
    struct squashfs_fs *fs = &g_squashfs_fs;
    struct squashfs_base_inode hdr;
    uint8_t buf[64];
    uint32_t len = 0;
    int r;

    r = sq_inode_load(ref, &hdr, buf, sizeof(buf), &len);
    if (r != SQUASHFS_OK) return r;
    memset(d, 0, sizeof(*d));
    d->inode_ref = ref;
    if (hdr.inode_type == SQUASHFS_INODE_DIR) {
        struct squashfs_reg_dir_inode in;
        if (len < sizeof(hdr) + sizeof(in)) return SQUASHFS_ERANGE;
        memcpy(&in, buf + sizeof(hdr), sizeof(in));
        if (in.size < 3u) return SQUASHFS_ERANGE;
        d->start_block = in.start_block;
        d->start_offset = in.offset;
        d->size = (uint32_t)in.size - 3u;
    } else if (hdr.inode_type == SQUASHFS_INODE_EXT_DIR) {
        struct squashfs_lreg_dir_inode in;
        if (len < sizeof(hdr) + sizeof(in)) return SQUASHFS_ERANGE;
        memcpy(&in, buf + sizeof(hdr), sizeof(in));
        if (in.size < 3u) return SQUASHFS_ERANGE;
        d->start_block = in.start_block;
        d->start_offset = in.offset;
        d->size = in.size - 3u;
    } else {
        return SQUASHFS_ENOTDIR;
    }
    d->used = 1;
    d->pos = 0;
    (void)fs;
    return SQUASHFS_OK;
}

int squashfs_opendir(const char *path, struct squashfs_dir **out) {
    uint64_t ref = 0;
    struct squashfs_stat st;
    struct squashfs_dir *d = NULL;
    int i, r;

    if (!g_squashfs_fs.mounted) return SQUASHFS_EBUSY;
    if (!out) return SQUASHFS_EINVAL;
    r = squashfs_resolve(path, &ref, &st);
    if (r != SQUASHFS_OK) return r;
    if (!st.is_dir) return SQUASHFS_ENOTDIR;

    for (i = 0; i < SQUASHFS_MAX_OPEN_DIRS; i++) {
        if (!g_dirs[i].used) { d = &g_dirs[i]; break; }
    }
    if (!d) return SQUASHFS_EBUSY;
    r = sq_dir_open_ref(d, ref);
    if (r != SQUASHFS_OK) {
        memset(d, 0, sizeof(*d));
        return r;
    }
    *out = d;
    return SQUASHFS_OK;
}

/* Returns SQUASHFS_OK for one entry, SQUASHFS_ENOENT at the end.
 * "." and ".." are skipped. Names longer than name_cap-1 are truncated. */
int squashfs_readdir(struct squashfs_dir *d, char *name, uint32_t name_cap,
                     uint32_t *name_len, int *is_dir) {
    struct squashfs_fs *fs = &g_squashfs_fs;
    uint64_t base;

    if (!d || !d->used || !name || name_cap == 0) return SQUASHFS_EINVAL;
    if (!fs->mounted) return SQUASHFS_EBUSY;

    /* see sq_dir_find(): the listing lives at (start_block, start_offset) */
    base = d->start_block * (uint64_t)SQUASHFS_METADATA_SIZE +
           (uint64_t)d->start_offset;

    while (d->pos < (uint64_t)d->size) {
        if (d->header_count == 0) {
            uint32_t count = 0, hblock = 0, hinode = 0;
            int r = sq_dir_header(fs, base + d->pos, &count, &hblock, &hinode);
            if (r != SQUASHFS_OK) return r;
            d->pos += sizeof(struct squashfs_dir_header);
            d->header_count = count;
            d->header_index = 0;
            d->header_num = count;
            d->header_block = hblock;
            d->header_inode = hinode;
            if (++d->entries_done > SQUASHFS_MAX_DIR_ENTRIES) return SQUASHFS_ERANGE;
            continue;
        }
        {
            uint8_t raw[sizeof(struct squashfs_dir_entry)];
            uint32_t nlen;
            uint16_t etype;
            int r;

            if (d->pos + sizeof(raw) > (uint64_t)d->size) return SQUASHFS_ERANGE;
            r = squashfs_meta_read(&fs->dir_table, base + d->pos, raw, sizeof(raw));
            if (r != SQUASHFS_OK) return r;
            nlen = sq_rd16(raw + 6) + 1u;
            etype = sq_rd16(raw + 4);
            if (nlen > SQUASHFS_NAME_MAX) return SQUASHFS_ERANGE;
            if (d->pos + sizeof(raw) + nlen > (uint64_t)d->size) return SQUASHFS_ERANGE;

            d->pos += sizeof(raw) + nlen;
            d->header_count--;
            d->header_index++;
            if (++d->entries_done > SQUASHFS_MAX_DIR_ENTRIES) return SQUASHFS_ERANGE;

            if (nlen == 1) {
                uint8_t c;
                r = squashfs_meta_read(&fs->dir_table, base + d->pos - nlen, &c, 1);
                if (r != SQUASHFS_OK) return r;
                if (c == '.') continue;
            }
            if (nlen == 2) {
                uint8_t c[2];
                r = squashfs_meta_read(&fs->dir_table, base + d->pos - nlen, c, 2);
                if (r != SQUASHFS_OK) return r;
                if (c[0] == '.' && c[1] == '.') continue;
            }
            {
                uint32_t copy = nlen < name_cap - 1 ? nlen : name_cap - 1;
                r = squashfs_meta_read(&fs->dir_table, base + d->pos - nlen,
                                       (uint8_t *)name, copy);
                if (r != SQUASHFS_OK) return r;
                name[copy] = '\0';
                if (name_len) *name_len = copy;
            }
            if (is_dir) {
                *is_dir = (etype == SQUASHFS_INODE_DIR ||
                           etype == SQUASHFS_INODE_EXT_DIR) ? 1 : 0;
            }
            return SQUASHFS_OK;
        }
    }
    return SQUASHFS_ENOENT;
}

int squashfs_closedir(struct squashfs_dir *d) {
    if (!d) return SQUASHFS_EINVAL;
    memset(d, 0, sizeof(*d));
    return SQUASHFS_OK;
}

/* ==================== guest selftest ==================== */

/* Content rules - MUST match scripts/mkfs_squashfs_testimg.py (see the file
 * header comment for the full table). */
static const char SQ_ST_HELLO[] =
    "Hello from SutureOS SquashFS!\nSecond line: gzip metadata and data.\n";
static const char SQ_ST_README[] =
    "SquashFS 4.0 read-only driver test volume.\n";
static const char SQ_ST_NESTED[] =
    "Nested file: /SUB/NESTED.TXT via the SquashFS directory table.\n";
static const char SQ_ST_DEEP[] =
    "Deep directory chain reached through SquashFS.\n";
static const char SQ_ST_LINK_TARGET[] = "LARGE.BIN";

#define SQ_ST_HELLO_SIZE  ((uint32_t)sizeof(SQ_ST_HELLO) - 1u)   /* 67 */
#define SQ_ST_README_SIZE ((uint32_t)sizeof(SQ_ST_README) - 1u)  /* 43 */
#define SQ_ST_NESTED_SIZE ((uint32_t)sizeof(SQ_ST_NESTED) - 1u)  /* 63 */
#define SQ_ST_DEEP_SIZE   ((uint32_t)sizeof(SQ_ST_DEEP) - 1u)    /* 47 */
#define SQ_ST_LARGE_SIZE  (100u * 1024u)     /* byte i = (i*31 + 7) & 0xFF */
#define SQ_ST_UNCOMP_SIZE (8u * 1024u)       /* byte i = lcg(i) & 0xFF */
#define SQ_ST_FRAG_SIZE   1000u              /* byte i = (i*5 + 1) & 0xFF */

enum sq_st_kind {
    SQ_ST_TEXT = 0,
    SQ_ST_LARGE,
    SQ_ST_UNCOMP,
    SQ_ST_FRAG,
};

/* /UNCOMP.BIN uses a linear congruential generator so the bytes do not
 * compress; the image generator uses the identical recurrence. */
static uint32_t sq_st_lcg(uint32_t i) {
    uint32_t s = 0x12345678u + i * 2654435761u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static uint8_t sq_st_expected(int kind, uint32_t off, const char *text) {
    switch (kind) {
        case SQ_ST_TEXT:   return (uint8_t)text[off];
        case SQ_ST_LARGE:  return (uint8_t)((off * 31u + 7u) & 0xFFu);
        case SQ_ST_UNCOMP: return (uint8_t)(sq_st_lcg(off) & 0xFFu);
        case SQ_ST_FRAG:   return (uint8_t)((off * 5u + 1u) & 0xFFu);
        default:           return 0;
    }
}

static int sq_selftest_fail(const char *reason) {
    console_printf("[squashfs] selftest FAIL (%s)\n", reason);
    return SQUASHFS_ERR;
}

static int sq_selftest_file(const char *path, int kind, uint32_t expect_size,
                            const char *text, uint8_t *scratch, uint32_t *out_crc) {
    struct squashfs_fs *fs = &g_squashfs_fs;
    struct squashfs_stat st;
    uint32_t offset = 0;
    uint32_t crc = 0xFFFFFFFFu;
    int r;

    r = squashfs_stat(path, &st);
    if (r != SQUASHFS_OK) return sq_selftest_fail("cannot stat a test file");
    if (!st.is_file) return sq_selftest_fail("a test file is not a regular file");
    if (st.size != expect_size) {
        console_printf("[squashfs] selftest detail: %s size=%u expected=%u\n", path,
                       (uint64_t)st.size, (uint64_t)expect_size);
        return sq_selftest_fail("unexpected test file size");
    }
    while (offset < expect_size) {
        uint32_t n = fs->block_size;
        uint32_t i;
        int got;
        if (n > expect_size - offset) n = expect_size - offset;
        got = squashfs_read_ref(st.inode_ref, scratch, offset, n);
        if (got != (int)n) return sq_selftest_fail("short read from a test file");
        for (i = 0; i < n; i++) {
            if (scratch[i] != sq_st_expected(kind, offset + i, text)) {
                console_printf("[squashfs] selftest detail: %s mismatch at offset %u\n",
                               path, (uint64_t)(offset + i));
                return sq_selftest_fail("test file content mismatch");
            }
        }
        crc = sq_crc_update(crc, scratch, n);
        offset += n;
    }
    if (out_crc) *out_crc = crc ^ 0xFFFFFFFFu;
    return SQUASHFS_OK;
}

static const char *const SQ_ST_ROOT_NAMES[] = {
    "HELLO.TXT", "README", "LARGE.BIN", "UNCOMP.BIN", "FRAG.TXT", "ZERO.BIN",
    "LINK.TXT", "SUB"
};
#define SQ_ST_ROOT_NAME_COUNT 8

static int sq_selftest_readdir(const char *path, const char *const *want,
                               uint32_t want_n, int *seen) {
    struct squashfs_dir *d = NULL;
    char name[128];
    int r;

    r = squashfs_opendir(path, &d);
    if (r != SQUASHFS_OK) return sq_selftest_fail("cannot open a test directory");

    console_printf("[squashfs] selftest readdir %s =", path);
    for (;;) {
        uint32_t len = 0;
        int is_dir = 0;
        r = squashfs_readdir(d, name, sizeof(name), &len, &is_dir);
        if (r == SQUASHFS_ENOENT) break;
        if (r != SQUASHFS_OK) {
            squashfs_closedir(d);
            return sq_selftest_fail("readdir failed");
        }
        console_printf(" %s%s", name, is_dir ? "/" : "");
        if (want) {
            for (uint32_t i = 0; i < want_n; i++) {
                if (strcmp(name, want[i]) == 0) seen[i] = 1;
            }
        }
    }
    console_puts("\n");
    squashfs_closedir(d);
    return SQUASHFS_OK;
}

int squashfs_selftest(void) {
    struct squashfs_fs *fs = &g_squashfs_fs;
    uint8_t *scratch;
    uint32_t crc_hello = 0, crc_large = 0, crc_nested = 0, crc_uncomp = 0, crc_frag = 0;
    int seen[SQ_ST_ROOT_NAME_COUNT];
    struct squashfs_stat st;
    char link[128];
    int r;

    for (uint32_t i = 0; i < SQ_ST_ROOT_NAME_COUNT; i++) seen[i] = 0;

    if (!fs->mounted) {
        if (!g_ram_image) {
            return sq_selftest_fail("no ramdisk registered and no SquashFS volume mounted");
        }
        if (squashfs_mount_ramdisk() != SQUASHFS_OK) {
            return sq_selftest_fail("mounting the registered ramdisk failed");
        }
    }

    scratch = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!scratch) return sq_selftest_fail("out of memory");

    r = sq_selftest_file("/HELLO.TXT", SQ_ST_TEXT, SQ_ST_HELLO_SIZE, SQ_ST_HELLO,
                         scratch, &crc_hello);
    if (r != SQUASHFS_OK) goto fail;
    r = sq_selftest_file("/README", SQ_ST_TEXT, SQ_ST_README_SIZE, SQ_ST_README,
                         scratch, NULL);
    if (r != SQUASHFS_OK) goto fail;
    r = sq_selftest_file("/SUB/NESTED.TXT", SQ_ST_TEXT, SQ_ST_NESTED_SIZE, SQ_ST_NESTED,
                         scratch, &crc_nested);
    if (r != SQUASHFS_OK) goto fail;
    r = sq_selftest_file("/SUB/DEEP/A/B/C/DEEP.TXT", SQ_ST_TEXT, SQ_ST_DEEP_SIZE,
                         SQ_ST_DEEP, scratch, NULL);
    if (r != SQUASHFS_OK) goto fail;
    r = sq_selftest_file("/LARGE.BIN", SQ_ST_LARGE, SQ_ST_LARGE_SIZE, NULL, scratch,
                         &crc_large);
    if (r != SQUASHFS_OK) goto fail;
    r = sq_selftest_file("/UNCOMP.BIN", SQ_ST_UNCOMP, SQ_ST_UNCOMP_SIZE, NULL, scratch,
                         &crc_uncomp);
    if (r != SQUASHFS_OK) goto fail;
    r = sq_selftest_file("/FRAG.TXT", SQ_ST_FRAG, SQ_ST_FRAG_SIZE, NULL, scratch,
                         &crc_frag);
    if (r != SQUASHFS_OK) goto fail;

    /* a zero-length file: stat says 0 bytes, a read returns 0, no blocks */
    {
        uint64_t ref = 0;
        uint8_t one = 0;
        if (squashfs_lookup("/ZERO.BIN", &ref) != SQUASHFS_OK) {
            sq_selftest_fail("cannot look up ZERO.BIN");
            goto fail;
        }
        if (squashfs_stat("/ZERO.BIN", &st) != SQUASHFS_OK || st.size != 0 || !st.is_file) {
            sq_selftest_fail("ZERO.BIN is not a zero-length regular file");
            goto fail;
        }
        if (squashfs_read_ref(ref, &one, 0, 1) != 0) {
            sq_selftest_fail("reading ZERO.BIN did not return EOF");
            goto fail;
        }
    }

    /* symlink: the target must come back as stored */
    {
        int n = squashfs_readlink("/LINK.TXT", link, sizeof(link));
        if (n != (int)(sizeof(SQ_ST_LINK_TARGET) - 1u) ||
            strcmp(link, SQ_ST_LINK_TARGET) != 0) {
            sq_selftest_fail("LINK.TXT does not point at LARGE.BIN");
            goto fail;
        }
        if (squashfs_stat("/LINK.TXT", &st) != SQUASHFS_OK || !st.is_symlink) {
            sq_selftest_fail("LINK.TXT is not reported as a symlink");
            goto fail;
        }
    }

    /* partial read via offset+len must match the full-file pattern */
    {
        uint64_t ref = 0;
        uint8_t part[64];
        if (squashfs_lookup("/LARGE.BIN", &ref) != SQUASHFS_OK) {
            sq_selftest_fail("cannot look up LARGE.BIN");
            goto fail;
        }
        r = squashfs_read_ref(ref, part, 4096, sizeof(part));
        if (r != (int)sizeof(part)) {
            sq_selftest_fail("partial read of LARGE.BIN failed");
            goto fail;
        }
        for (uint32_t i = 0; i < sizeof(part); i++) {
            if (part[i] != (uint8_t)(((4096u + i) * 31u + 7u) & 0xFFu)) {
                sq_selftest_fail("partial read of LARGE.BIN returned wrong bytes");
                goto fail;
            }
        }
        /* a read starting past EOF returns 0 */
        if (squashfs_read_ref(ref, part, SQ_ST_LARGE_SIZE + 10u, 16) != 0) {
            sq_selftest_fail("reading past EOF did not return 0");
            goto fail;
        }
    }

    /* '..' resolution through the real directory table */
    {
        uint64_t a = 0, b = 0;
        if (squashfs_lookup("/HELLO.TXT", &a) != SQUASHFS_OK ||
            squashfs_lookup("/SUB/../HELLO.TXT", &b) != SQUASHFS_OK || a != b) {
            sq_selftest_fail("'..' path resolution failed");
            goto fail;
        }
        if (squashfs_stat("/SUB", &st) != SQUASHFS_OK || !st.is_dir) {
            sq_selftest_fail("/SUB is not a directory");
            goto fail;
        }
        if (squashfs_stat("/", &st) != SQUASHFS_OK || !st.is_dir) {
            sq_selftest_fail("'/' is not a directory");
            goto fail;
        }
    }

    if (sq_selftest_readdir("/", SQ_ST_ROOT_NAMES, SQ_ST_ROOT_NAME_COUNT, seen) !=
        SQUASHFS_OK) {
        goto fail;
    }
    for (uint32_t i = 0; i < SQ_ST_ROOT_NAME_COUNT; i++) {
        if (!seen[i]) {
            sq_selftest_fail("the root directory listing is incomplete");
            goto fail;
        }
    }
    if (sq_selftest_readdir("/SUB", NULL, 0, NULL) != SQUASHFS_OK) goto fail;

    /* the extended-metadata path: an empty directory must readdir cleanly as
     * the single "." / ".." pair and return end-of-directory at once */
    if (sq_selftest_readdir("/SUB/DEEP/A/B/C", NULL, 0, NULL) != SQUASHFS_OK) goto fail;

    console_printf("[squashfs] selftest sizes hello=%u readme=%u nested=%u deep=%u "
                   "large=%u uncomp=%u frag=%u\n",
                   (uint64_t)SQ_ST_HELLO_SIZE, (uint64_t)SQ_ST_README_SIZE,
                   (uint64_t)SQ_ST_NESTED_SIZE, (uint64_t)SQ_ST_DEEP_SIZE,
                   (uint64_t)SQ_ST_LARGE_SIZE, (uint64_t)SQ_ST_UNCOMP_SIZE,
                   (uint64_t)SQ_ST_FRAG_SIZE);
    console_printf("[squashfs] selftest PASS (hello=0x%08x large=0x%08x nested=0x%08x "
                   "uncomp=0x%08x frag=0x%08x)\n",
                   (uint64_t)crc_hello, (uint64_t)crc_large, (uint64_t)crc_nested,
                   (uint64_t)crc_uncomp, (uint64_t)crc_frag);
    lemis_heap_free(scratch);
    return SQUASHFS_OK;

fail:
    lemis_heap_free(scratch);
    return SQUASHFS_ERR;
}

/* ==================== legacy entry points ==================== */

int squashfs_init(void) {
    console_puts("[squashfs] init: real SquashFS 4.0 read-only driver (gzip)\n");
    return SQUASHFS_OK;
}

int squashfs_register(void) {
    /* The VFS driver is a compile-time object (squashfs_driver in
     * kernel/fs/squashfs_vfs.c); vfs_init() registers it. Kept for compatibility
     * with the pre-existing stub API and as an explicit success marker. */
    console_puts("[squashfs] registered with the VFS (driver name \"squashfs\")\n");
    return SQUASHFS_OK;
}

int squashfs_mkdir(const char *path, uint32_t mode) {
    (void)path; (void)mode;
    return SQUASHFS_EROFS;
}

int squashfs_unlink(const char *path) { (void)path; return SQUASHFS_EROFS; }
int squashfs_rmdir(const char *path) { (void)path; return SQUASHFS_EROFS; }

int squashfs_stat_vfs64(const char *path, struct Vfs64Info64 *out) {
    struct squashfs_stat st;
    int r;

    if (!out) return SQUASHFS_EINVAL;
    r = squashfs_stat(path, &st);
    if (r != SQUASHFS_OK) return r;

    memset(out, 0, sizeof(*out));
    out->index = st.inode_number;
    out->type = st.is_dir ? VFS64_TYPE_DIR : VFS64_TYPE_FILE;
    out->size = (uint32_t)st.size;
    out->mtime = st.mtime;
    out->parent = 0;
    out->nlink = st.nlink;
    out->kind = st.is_dir ? VFS64_KIND_DIR : VFS64_KIND_FILE;
    out->uid = st.uid;
    out->gid = st.gid;
    out->mode = st.mode;
    out->name_len = 0;
    return SQUASHFS_OK;
}

/* Read one whole metadata block from the image (the legacy API's meaning). */
int squashfs_read_block(uint32_t block_num, void *buf) {
    if (!buf) return SQUASHFS_EINVAL;
    if (!g_squashfs_fs.mounted) return SQUASHFS_EBUSY;
    if (block_num >= g_squashfs_fs.inode_table.block_count) return SQUASHFS_ERANGE;
    return (sq_meta_block_load(&g_squashfs_fs.inode_table, block_num, (uint8_t *)buf,
                               SQUASHFS_METADATA_SIZE, NULL) == SQUASHFS_OK)
           ? SQUASHFS_OK : SQUASHFS_EIO;
}

int squashfs_write_block(uint32_t block_num, const void *buf) {
    (void)block_num; (void)buf;
    return SQUASHFS_EROFS;
}

/* The legacy 32-bit "inode number" cannot name a SquashFS inode (its real
 * identity is a 64-bit table reference). Only the root is unambiguous, so every
 * other value fails honestly instead of returning a wrong inode. */
int squashfs_read_inode(uint32_t ino, void *inode_buf) {
    if (!inode_buf) return SQUASHFS_EINVAL;
    if (!g_squashfs_fs.mounted) return SQUASHFS_EBUSY;
    if (ino != 2) return SQUASHFS_ENOTSUP;      /* legacy API is root-only */
    return sq_inode_stat(g_squashfs_fs.root_inode_ref, (struct squashfs_stat *)inode_buf);
}

int squashfs_write_inode(uint32_t ino, const void *inode_buf) {
    (void)ino; (void)inode_buf;
    return SQUASHFS_EROFS;
}

int squashfs_read_file(uint32_t ino, void *buf, uint32_t offset, uint32_t size) {
    if (!g_squashfs_fs.mounted) return SQUASHFS_EBUSY;
    if (ino != 2) return SQUASHFS_ENOTSUP;      /* legacy API is root-only */
    return squashfs_read_ref(g_squashfs_fs.root_inode_ref, buf, offset, size);
}

int squashfs_write_file(uint32_t ino, const void *buf, uint32_t offset, uint32_t size) {
    (void)ino; (void)buf; (void)offset; (void)size;
    return SQUASHFS_EROFS;
}

int squashfs_read_dir(uint32_t dir_ino, void *buf, uint32_t max_entries) {
    (void)dir_ino; (void)buf; (void)max_entries;
    return SQUASHFS_ENOTSUP;   /* legacy "dump into one buffer" API is gone */
}

int squashfs_compress(uint16_t algo, const void *src, size_t src_len,
                      void *dst, size_t *dst_len) {
    (void)algo; (void)src; (void)src_len; (void)dst; (void)dst_len;
    return SQUASHFS_ENOTSUP;   /* read-only: no compressor in the kernel */
}

int squashfs_decompress(uint16_t algo, const void *src, size_t src_len,
                        void *dst, size_t *dst_len) {
    int n;
    if (!src || !dst || !dst_len) return SQUASHFS_EINVAL;
    if (src_len > 0xFFFFFFFFull || *dst_len > 0xFFFFFFFFull) return SQUASHFS_EINVAL;
    n = squashfs_decompress_block(algo, 1, (const uint8_t *)src, (uint32_t)src_len,
                                  (uint8_t *)dst, (uint32_t)*dst_len);
    if (n < 0) return n;
    *dst_len = (size_t)n;
    return SQUASHFS_OK;
}

int squashfs_vfs_init(void) {
    console_puts("[squashfs] vfs init: real SquashFS 4.0 read-only driver\n");
    return squashfs_init();
}

int squashfs_vfs_register(void) { return squashfs_register(); }
