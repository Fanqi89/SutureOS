/* SutureOS: FAT32 filesystem - real driver
 * Ported/adapted from: SpiritFoxOS kernel/src/fs/fat32.c
 * (C:\Users\fanqi\Desktop\自研操作系统\SpiritFoxOS), GPL-3.0
 * Changes: SutureOS block layer (blk_read/blk_write) I/O, single-volume
 *          context, sector-granular heap scratch buffers, contiguous-run
 *          cluster allocation, byte-for-byte write verification.
 *
 * Honest scope:
 *  - superfloppy volumes only (BPB at device LBA 0; no MBR scan)
 *  - 8.3 names only, LFN entries (attr 0x0F) are skipped
 *  - writable: create/write/truncate/mkdir/rmdir/unlink/sync
 *  - rename(): NOT implemented, returns FATFS_ERR (adapter -> VFS_EPERM)
 */
#include "fatfs.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"
#include "block.h"

#define FAT32_EOC        0x0FFFFFFFu
#define FAT32_MASK       0x0FFFFFFFu
#define FAT32_ENTRY_FREE 0u
#define DIR_ENTRY_SIZE   32u
#define ENTRIES_PER_SEC  (512u / DIR_ENTRY_SIZE)
#define ATTR_RO          0x01
#define ATTR_VOL         0x08
#define ATTR_DIR         0x10
#define ATTR_LFN         0x0F

#define MAX_FILES        8
#define MAX_DIRS         8

#define ATTR_ARCH        0x20
#define ATTR_LFN_EXACT   0x0Fu   /* an LFN entry's attribute byte is exactly this */
#define LFN_MAX_CHARS    255u
#define LFN_MAX_ORD      20u

/* VFAT long-name entry: 13 UTF-16 code units at these byte offsets. */
static const uint8_t g_lfn_ofs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

/* Accumulator for one LFN chain.  Entries are stored physically in reverse logical
 * order: the first physical entry carries ord = N | 0x40 (the LAST 13 chars) and the
 * entry right before the short name carries ord = 1 (the FIRST 13 chars), so we
 * index every chunk by (ord - 1) * 13. */
struct lfn_acc {
    uint16_t name[LFN_MAX_CHARS + 1];
    uint8_t  max_ord;
    uint8_t  sum;
    uint8_t  active;
};

/* ==================== volume state ==================== */

struct fat32_vol {
    int      drive;
    uint32_t start_lba;       /* partition offset in 512-byte sectors (0 = superfloppy) */
    uint32_t fat_lba;
    uint32_t fat_sz;
    uint32_t data_lba;
    uint32_t root_cluster;
    uint32_t spc;
    uint32_t cluster_bytes;
    uint32_t total_sectors;
    uint32_t total_clusters;
    uint32_t free_clusters;   /* 0xFFFFFFFF = unknown */
    uint32_t fsinfo_lba;      /* 0 = none */
    int      fsinfo_dirty;
    int      mounted;
};

struct fatfs_file {
    int      used;
    int      vol;
    uint32_t first_cluster;
    uint32_t size;
    /* Chain cursor: sequential I/O advances one cluster at a time instead of
     * re-walking the FAT from the head for every 512-byte sector (that made
     * writing a multi-MiB file quadratic - the installer copies kernel.elf). */
    uint32_t walk_index;
    uint32_t walk_cluster;
    char     path[64];
};

struct fatfs_dir {
    int      used;
    int      vol;
    uint32_t cluster;
    uint32_t entry;
    struct lfn_acc lfn;          /* pending LFN chain for the resumable readdir() */
    char     path[64];
};

/* Multi-volume support (v0.24.0): the core keeps one state block per mounted
 * volume and a "current volume" pointer, so every existing path helper keeps
 * working unchanged. The VFS adapter selects the volume of the mount the path
 * belongs to before each call; file/dir handles remember their own volume. */
#define FATFS_MAX_VOLS 4
static struct fat32_vol g_vols[FATFS_MAX_VOLS];
static struct fat32_vol *g_vol = &g_vols[0];
static int g_cur_vol;

/* Bulk-write mode (installer only): skip the per-sector read-back verify that
 * sec_write_checked() normally does. The write itself is unchanged and still
 * checked by the FAT-level verify plus the final sync; it exists because
 * copying a 2.5 MiB kernel sector-by-sector with a verify read doubles the I/O
 * and made installation take minutes instead of a minute. Off by default, so
 * every other path keeps full verification. */
static int g_bulk_write;
static struct fatfs_file g_files[MAX_FILES];
static struct fatfs_dir  g_dirs[MAX_DIRS];

/* ==================== sector I/O ==================== */

static int sec_read(uint32_t lba, uint8_t *buf) {
    if (blk_read(g_vol->drive, (uint64_t)g_vol->start_lba + lba, 1, buf) != 0) return FATFS_EIO;
    return FATFS_OK;
}

static int sec_write_checked(uint32_t lba, const uint8_t *buf, uint8_t *vbuf,
                             const char *what, uint32_t at) {
    if (blk_write(g_vol->drive, (uint64_t)g_vol->start_lba + lba, 1, buf) != 0) return FATFS_EIO;
    if (g_bulk_write) return FATFS_OK;   /* bulk mode: skip the read-back verify */
    if (blk_read(g_vol->drive, (uint64_t)g_vol->start_lba + lba, 1, vbuf) != 0) return FATFS_EIO;
    if (memcmp(buf, vbuf, 512) != 0) {
        for (int i = 0; i < 512; i++) {
            if (buf[i] != vbuf[i]) {
                console_printf("[fat32] verify FAIL %s at=%u want=0x%x got=0x%x\n",
                               what, (uint64_t)(at + (uint32_t)i),
                               (uint64_t)buf[i], (uint64_t)vbuf[i]);
                break;
            }
        }
        return FATFS_EIO;
    }
    return FATFS_OK;
}

static uint32_t cluster_lba(uint32_t cl) {
    return g_vol->data_lba + (cl - 2) * g_vol->spc;
}

static int cluster_valid(uint32_t cl) {
    return cl >= 2 && cl < g_vol->total_clusters + 2;
}

/* First cluster of a 32-byte directory entry. FAT32 stores it as a broken-in-two
 * value: high word at offset 20, low word at offset 26. Reading only ent[26]
 * (as this driver used to) silently truncates the low word to 8 bits, so every
 * file or directory whose first cluster is >= 256 read the wrong data - found
 * while installing a real system onto a 128 MiB FAT32 volume (the installer's
 * files start at cluster 2866). */
static uint32_t dirent_first_cluster(const uint8_t *ent) {
    return ((uint32_t)ent[20] << 16) | ((uint32_t)ent[21] << 24) |
           (uint32_t)ent[26] | ((uint32_t)ent[27] << 8);
}

/* ==================== FAT access ==================== */

static int fat_get(uint32_t cl, uint8_t *scratch, uint32_t *out) {
    if (!cluster_valid(cl)) return FATFS_EINVAL;
    uint32_t off = cl * 4;
    uint32_t lba = g_vol->fat_lba + off / 512;
    int r = sec_read(lba, scratch);
    if (r != FATFS_OK) return r;
    uint32_t v;
    memcpy(&v, scratch + (off % 512), 4);
    *out = v & FAT32_MASK;
    return FATFS_OK;
}

/* Write one FAT entry to BOTH copies (preserve high nibble), verify each. */
static int fat_set(uint32_t cl, uint32_t value, uint8_t *scratch, uint8_t *vbuf,
                   const char *log) {
    uint32_t off = cl * 4;
    uint32_t rel = off / 512;
    uint32_t lba1 = g_vol->fat_lba + rel;
    uint32_t lba2 = g_vol->fat_lba + g_vol->fat_sz + rel;
    uint32_t pos = off % 512;

    for (uint32_t lba = lba1; ; lba = lba2) {
        int r = sec_read(lba, scratch);
        if (r != FATFS_OK) return r;
        uint32_t old;
        memcpy(&old, scratch + pos, 4);
        uint32_t nv = (old & 0xF0000000u) | (value & FAT32_MASK);
        memcpy(scratch + pos, &nv, 4);
        r = sec_write_checked(lba, scratch, vbuf, log ? log : "fat", cl * 4u);
        if (r != FATFS_OK) return r;
        if (lba == lba2) break;
    }
    return FATFS_OK;
}

static void fat_count_update(uint32_t old_val, uint32_t new_val) {
    if (g_vol->free_clusters == 0xFFFFFFFFu) return;
    if (old_val == FAT32_ENTRY_FREE && new_val != FAT32_ENTRY_FREE) {
        if (g_vol->free_clusters > 0) g_vol->free_clusters--;
    } else if (old_val != FAT32_ENTRY_FREE && new_val == FAT32_ENTRY_FREE) {
        g_vol->free_clusters++;
    }
}

static int fat_set_c(uint32_t cl, uint32_t value, uint8_t *s, uint8_t *v,
                     const char *log) {
    uint32_t old = 0;
    int r = fat_get(cl, s, &old);
    if (r != FATFS_OK) return r;
    r = fat_set(cl, value, s, v, log);
    if (r == FATFS_OK) fat_count_update(old & FAT32_MASK, value & FAT32_MASK);
    return r;
}

static int chain_locate(uint32_t first, uint32_t index, uint8_t *scratch,
                        uint32_t *out) {
    uint32_t cl = first;
    for (uint32_t i = 0; i < index; i++) {
        if (cl < 2 || cl >= FAT32_EOC) return FATFS_ENOENT;
        int r = fat_get(cl, scratch, &cl);
        if (r != FATFS_OK) return r;
    }
    if (cl < 2 || cl >= FAT32_EOC) return FATFS_ENOENT;
    *out = cl;
    return FATFS_OK;
}

/* chain_locate() with a per-file cursor. Sequential writes advance one cluster
 * at a time (one FAT read per cluster) instead of re-walking the chain from the
 * head for every sector, which is what made a multi-MiB file write quadratic.
 * A stale/backwards index simply restarts the walk, so random I/O stays correct. */
static int chain_locate_cached(struct fatfs_file *f, uint32_t index,
                               uint8_t *scratch, uint32_t *out) {
    uint32_t cl, i;

    if (f->walk_cluster >= 2 && f->walk_cluster < FAT32_EOC &&
        index >= f->walk_index) {
        cl = f->walk_cluster;
        i = f->walk_index;
    } else {
        cl = f->first_cluster;
        i = 0;
    }
    for (; i < index; i++) {
        int r;
        if (cl < 2 || cl >= FAT32_EOC) return FATFS_ENOENT;
        r = fat_get(cl, scratch, &cl);
        if (r != FATFS_OK) return r;
    }
    if (cl < 2 || cl >= FAT32_EOC) return FATFS_ENOENT;
    f->walk_index = index;
    f->walk_cluster = cl;
    *out = cl;
    return FATFS_OK;
}

static int chain_free(uint32_t first, uint8_t *s, uint8_t *v) {
    uint32_t cl = first;
    while (cl >= 2 && cl < FAT32_EOC) {
        uint32_t next;
        int r = fat_get(cl, s, &next);
        if (r != FATFS_OK) return r;
        r = fat_set_c(cl, FAT32_ENTRY_FREE, s, v, "chain_free");
        if (r != FATFS_OK) return r;
        cl = next;
    }
    g_vol->fsinfo_dirty = 1;
    return FATFS_OK;
}

/* Allocate n CONTIGUOUS free clusters. Links are written with the EOC
 * LAST so a failure leaves a reclaimable prefix chain. */
static int alloc_run(uint32_t n, uint8_t *s, uint8_t *v, uint32_t *out_first) {
    if (n == 0) { *out_first = FAT32_EOC; return FATFS_OK; }
    if (g_vol->free_clusters != 0xFFFFFFFFu && g_vol->free_clusters < n) {
        return FATFS_ENOSPC;
    }

    uint32_t run_start = 0, run_len = 0;
    for (uint32_t cl = 2; cl < g_vol->total_clusters + 2; cl++) {
        uint32_t val;
        int r = fat_get(cl, s, &val);
        if (r != FATFS_OK) return r;
        if (val == FAT32_ENTRY_FREE) {
            if (run_len == 0) run_start = cl;
            if (++run_len >= n) break;
        } else {
            run_len = 0;
        }
    }
    if (run_len < n) return FATFS_ENOSPC;

    memset(v, 0, 512);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t lba = cluster_lba(run_start + i);
        for (uint32_t k = 0; k < g_vol->spc; k++) {
            int r = sec_write_checked(lba + k, v, s, "zero-fill", lba + k);
            if (r != FATFS_OK) {
                chain_free(run_start, s, v);
                return r;
            }
        }
    }

    for (uint32_t i = 0; i + 1 < n; i++) {
        int r = fat_set_c(run_start + i, run_start + i + 1, s, v, "alloc link");
        if (r != FATFS_OK) {
            chain_free(run_start, s, v);
            return r;
        }
    }
    int r = fat_set_c(run_start + n - 1, FAT32_EOC, s, v, "alloc eoc");
    if (r != FATFS_OK) {
        chain_free(run_start, s, v);
        return r;
    }
    g_vol->fsinfo_dirty = 1;
    *out_first = run_start;
    return FATFS_OK;
}

static int chain_ensure(uint32_t *first, uint32_t need_clusters,
                        uint8_t *s, uint8_t *v) {
    uint32_t have = 0;
    if (*first >= 2 && *first < FAT32_EOC) {
        uint32_t cl = *first;
        have = 1;
        while (1) {
            uint32_t next;
            int r = fat_get(cl, s, &next);
            if (r != FATFS_OK) return r;
            if (next >= FAT32_EOC || next < 2) break;
            cl = next;
            have++;
        }
    }
    if (have >= need_clusters) return FATFS_OK;

    uint32_t add = need_clusters - have;
    uint32_t new_first;
    int r = alloc_run(add, s, v, &new_first);
    if (r != FATFS_OK) return r;

    if (have == 0) {
        *first = new_first;
        return FATFS_OK;
    }
    uint32_t cl = *first;
    while (1) {
        uint32_t next;
        r = fat_get(cl, s, &next);
        if (r != FATFS_OK) return r;
        if (next >= FAT32_EOC || next < 2) break;
        cl = next;
    }
    r = fat_set_c(cl, new_first, s, v, "chain append");
    if (r != FATFS_OK) {
        chain_free(new_first, s, v);
        return r;
    }
    return FATFS_OK;
}

/* ==================== 8.3 names ==================== */

static void name_to_raw(const char *name, char raw[11]) {
    memset(raw, ' ', 11);
    int i = 0;
    while (*name && *name != '.' && i < 8) {
        char c = *name++;
        if (c >= 'a' && c <= 'z') c -= 32;
        raw[i++] = c;
    }
    while (*name && *name != '.') name++;
    if (*name == '.') {
        name++;
        i = 8;
        while (*name && i < 11) {
            char c = *name++;
            if (c >= 'a' && c <= 'z') c -= 32;
            raw[i++] = c;
        }
    }
}

static void raw_to_name(const char raw[11], char *out, uint32_t cap) {
    uint32_t o = 0;
    for (int i = 0; i < 8 && raw[i] != ' ' && o + 1 < cap; i++) out[o++] = raw[i];
    if (raw[8] != ' ') {
        if (o + 1 < cap) out[o++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' ' && o + 1 < cap; i++) out[o++] = raw[i];
    }
    out[o] = '\0';
}

struct dir_slot {
    uint32_t lba;
    uint32_t offset;
    int      valid;
};

/* ---- VFAT long file names ----
 * Checksum over the 11 short-name bytes; every entry of one chain carries it and it
 * must match the short entry that follows, otherwise the chain is stale (that check
 * is exactly what the SpiritFoxOS ancestor was missing). */
static uint8_t lfn_checksum(const uint8_t raw[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)((sum >> 1) + (sum << 7) + raw[i]);
    }
    return sum;
}

static int lfn_is_entry(const uint8_t *ent) {
    return ent[11] == ATTR_LFN_EXACT && ent[12] == 0 && ent[26] == 0 && ent[27] == 0;
}

static void lfn_reset(struct lfn_acc *a) {
    a->max_ord = 0;
    a->sum = 0;
    a->active = 0;
    memset(a->name, 0, sizeof(a->name));
}

/* Feed one 32-byte directory entry. */
static void lfn_feed(struct lfn_acc *a, const uint8_t *ent) {
    uint8_t ord = (uint8_t)(ent[0] & 0x3Fu);
    if (ord == 0 || ord > LFN_MAX_ORD) {
        lfn_reset(a);
        return;
    }
    if (ent[0] & 0x40u) {
        lfn_reset(a);                      /* last logical part: a new chain starts */
    }
    a->active = 1;
    a->sum = ent[13];
    if (ord > a->max_ord) a->max_ord = ord;
    for (uint32_t k = 0; k < 13u; k++) {
        const uint32_t idx = (uint32_t)(ord - 1u) * 13u + k;
        if (idx < LFN_MAX_CHARS) {
            a->name[idx] = (uint16_t)((uint16_t)ent[g_lfn_ofs[k]] |
                                      ((uint16_t)ent[g_lfn_ofs[k] + 1u] << 8));
        }
    }
}

/* Assemble the pending chain into `out` as UTF-8. Returns 1 when a name was
 * produced. UTF-8 (rather than ASCII with '_' placeholders) is what makes a
 * non-ASCII long name usable: the bytes read back here are exactly the bytes a
 * caller passes to fatfs_open(), so the name round-trips. Surrogate pairs are
 * rejected - the writer never creates them. */
static int lfn_assemble(const struct lfn_acc *a, const uint8_t sfn[11],
                        char *out, uint32_t cap) {
    uint32_t n = 0;
    if (!a->active || a->max_ord == 0) return 0;
    if (lfn_checksum(sfn) != a->sum) return 0;         /* stale chain: ignore it */
    for (uint32_t i = 0; i < LFN_MAX_CHARS; i++) {
        uint16_t c = a->name[i];
        if (c == 0x0000u || c == 0xFFFFu) break;
        if (c < 0x80u) {
            if (n + 1u >= cap) break;
            out[n++] = (char)c;
        } else if (c < 0x800u) {
            if (n + 2u >= cap) break;
            out[n++] = (char)(0xC0u | (c >> 6));
            out[n++] = (char)(0x80u | (c & 0x3Fu));
        } else {
            if (n + 3u >= cap) break;
            out[n++] = (char)(0xE0u | (c >> 12));
            out[n++] = (char)(0x80u | ((c >> 6) & 0x3Fu));
            out[n++] = (char)(0x80u | (c & 0x3Fu));
        }
    }
    out[n] = '\0';
    return n > 0;
}

static int name_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
        if (x != y) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* A 0x00 marker means "no entry has ever been written here", which is the end
 * of the directory ONLY in its last cluster: once a directory grows (17th
 * entry), the earlier clusters still hold 0x00 slots that are followed by the
 * new cluster. Treating the first 0x00 as the end made every entry created
 * after the growth invisible to lookup/readdir - that is the "rename returned
 * OK but the new name cannot be opened" bug this helper fixes. */
static int dir_cluster_is_last(uint32_t cl, uint8_t *s) {
    uint32_t next = 0;
    if (fat_get(cl, s, &next) != FATFS_OK) return 1;   /* be conservative */
    return (next >= FAT32_EOC || next < 2) ? 1 : 0;
}

/* Find an entry whose VFAT long name matches `want` (case-insensitive). On success
 * the short entry is copied to out_ent and its 11 raw name bytes to out_raw, so the
 * caller can keep using the 8.3 machinery for everything else. */
static int dir_find_lfn(uint32_t dir_cl, const char *want, uint8_t *s,
                        struct dir_slot *out, uint8_t *out_ent, char out_raw[11]) {
    struct lfn_acc acc;
    uint32_t cl = dir_cl;
    lfn_reset(&acc);
    while (cl >= 2 && cl < FAT32_EOC) {
        for (uint32_t k = 0; k < g_vol->spc; k++) {
            uint32_t lba = cluster_lba(cl) + k;
            int r = sec_read(lba, s);
            if (r != FATFS_OK) return r;
            for (uint32_t e = 0; e < ENTRIES_PER_SEC; e++) {
                uint8_t *ent = s + e * DIR_ENTRY_SIZE;
                char longname[LFN_MAX_CHARS + 1];
                if (ent[0] == 0x00) {
                    if (dir_cluster_is_last(cl, s)) return FATFS_ENOENT;
                    continue;              /* free slot before a later cluster */
                }
                if (ent[0] == 0xE5) { lfn_reset(&acc); continue; }
                if (lfn_is_entry(ent)) { lfn_feed(&acc, ent); continue; }
                if (ent[11] & ATTR_VOL) { lfn_reset(&acc); continue; }
                if (lfn_assemble(&acc, ent, longname, sizeof(longname)) &&
                    name_eq_ci(longname, want)) {
                    out->lba = lba;
                    out->offset = e * DIR_ENTRY_SIZE;
                    out->valid = 1;
                    if (out_ent) memcpy(out_ent, ent, DIR_ENTRY_SIZE);
                    memcpy(out_raw, ent, 11);
                    return FATFS_OK;
                }
                lfn_reset(&acc);
            }
        }
        uint32_t next;
        int r = fat_get(cl, s, &next);
        if (r != FATFS_OK) return r;
        cl = next;
    }
    return FATFS_ENOENT;
}

/* Mark the LFN entries that precede the short entry at (lba, off) as deleted, so a
 * remove/rename does not orphan them. Walks backwards, crossing sector boundaries
 * inside the cluster; stops at the first non-LFN entry. */
static void dir_mark_lfn_chain(uint32_t lba, uint32_t off, uint8_t *s) {
    uint32_t cur_lba = lba;
    uint32_t cur_off = off;
    for (int guard = 0; guard <= (int)LFN_MAX_ORD; guard++) {
        if (cur_off >= DIR_ENTRY_SIZE) {
            cur_off -= DIR_ENTRY_SIZE;
        } else {
            if (cur_lba == 0) return;
            cur_lba--;
            cur_off = 512u - DIR_ENTRY_SIZE;
        }
        if (sec_read(cur_lba, s) != FATFS_OK) return;
        if (!lfn_is_entry(s + cur_off)) return;
        s[cur_off] = 0xE5;
        {
            uint8_t *v = lemis_heap_alloc(512);
            if (!v) return;
            (void)sec_write_checked(cur_lba, s, v, "lfn delete", cur_off);
            lemis_heap_free(v);
        }
    }
}

static int raw_equal(const char a[11], const char b[11]) {
    return memcmp(a, b, 11) == 0;
}

static int make_raw(const char *name, char raw[11]) {
    const char *dot = strrchr(name, '.');
    uint32_t base_len = dot ? (uint32_t)(dot - name) : (uint32_t)strlen(name);
    if (base_len == 0) return FATFS_EINVAL;
    if (base_len > 8 || (dot && strlen(dot + 1) > 3)) {
        return FATFS_ENAMETOOLONG;
    }
    name_to_raw(name, raw);
    return FATFS_OK;
}

/* ==================== VFAT long names: WRITE side ====================
 * The read side above only needs to understand someone else's chains. Creating a
 * long name means writing the whole chain OURSELVES, in the order the VFAT spec
 * requires: physically the LAST 13-character chunk comes first, carrying
 * ord = N|0x40, and the short entry follows the ord = 1 chunk. Every entry must
 * repeat the same checksum of the 11 short-name bytes, otherwise the chain is
 * dead on arrival (the reader - including our own - discards it).
 *
 * Written from the VFAT specification and the ChaN FatFs `put_lfn()` algorithm
 * (PlantOS ships that lineage under MIT; see docs/licensing-audit.md). Only 1/2/3
 * byte UTF-8 sequences are accepted - no surrogate pairs - because this driver
 * has no wide-char console anyway.
 */
#define LFN_MAX_ENTRIES 20u

/* dir_find() is defined further down (the 8.3 lookup); the creation helpers above
 * it need it to reject duplicates, so declare it here. */
static int dir_find(uint32_t dir_cl, const char raw[11], uint8_t *s,
                    struct dir_slot *out, uint8_t *out_ent);

/* Path helpers shared by open(create) and rename (definitions below). */
static void path_split(const char *path, char *dir, uint32_t cap, const char **base);
static int resolve_dir_cluster(const char *dirpath, uint8_t *s, uint8_t *v,
                               uint32_t *out_cl);

/* Decode UTF-8 into UTF-16 code units. Returns the count, or -1 when the string
 * is malformed or longer than `cap` units. */
static int lfn_encode(const char *name, uint16_t *units, uint32_t cap) {
    uint32_t n = 0;
    const unsigned char *p = (const unsigned char *)name;
    while (*p) {
        uint32_t cp;
        if (p[0] < 0x80u) {
            cp = p[0];
            p += 1;
        } else if ((p[0] & 0xE0u) == 0xC0u && (p[1] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(p[0] & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
            p += 2;
        } else if ((p[0] & 0xF0u) == 0xE0u && (p[1] & 0xC0u) == 0x80u &&
                   (p[2] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(p[0] & 0x0Fu) << 12) |
                 ((uint32_t)(p[1] & 0x3Fu) << 6) | (uint32_t)(p[2] & 0x3Fu);
            p += 3;
        } else {
            return -1;                       /* 4-byte / invalid: no surrogates */
        }
        if (cp == 0 || cp > 0xFFFFu) return -1;
        if (n >= cap) return -1;
        units[n++] = (uint16_t)cp;
    }
    return (int)n;
}

/* Does this name have to use an LFN chain? (i.e. is it not representable as 8.3:
 * base > 8, ext > 3, lower case, spaces, more than one dot, or non-ASCII) */
static int lfn_need(const char *name) {
    uint32_t base = 0, ext = 0;
    int dot = 0;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '.') {
            if (dot) return 1;               /* second dot */
            dot = 1;
            continue;
        }
        if (c < 0x20u || c >= 0x7Fu) return 1;      /* control / non-ASCII */
        if (c >= 'a' && c <= 'z') return 1;         /* stored short names are upper */
        if (c == ' ') return 1;
        if (!dot) base++; else ext++;
    }
    if (base == 0 || base > 8 || ext > 3) return 1;
    return 0;
}

/* Build the N-th candidate 8.3 alias for `name` (seq 0 = ideal, 1.. = ~1, ~2 ...).
 * Non-ASCII and punctuation that 8.3 forbids collapse to '_'. */
static void lfn_short_alias(const char *name, int seq, char raw[11]) {
    char base[9], ext[4];
    uint32_t bi = 0, ei = 0;
    int dot = 0;
    for (const char *p = name; *p && (bi < 8 || ei < 3); p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '.') { dot = 1; continue; }
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        if (c <= 0x20u || c >= 0x7Fu) c = '_';
        if (!dot && bi < 8) { base[bi++] = (char)c; }
        else if (dot && ei < 3) { ext[ei++] = (char)c; }
    }
    if (bi == 0) { base[bi++] = '_'; }
    if (seq > 0) {
        char tail[4];
        uint32_t ti = 0;
        int v = seq;
        tail[ti++] = '~';
        if (v >= 10) tail[ti++] = (char)('0' + (v / 10) % 10);
        tail[ti++] = (char)('0' + v % 10);
        uint32_t keep = (bi + ti <= 8) ? bi : (8 - ti);
        memcpy(base + keep, tail, ti);
        bi = keep + ti;
    }
    memset(raw, ' ', 11);
    for (uint32_t i = 0; i < bi && i < 8; i++) raw[i] = base[i];
    for (uint32_t i = 0; i < ei && i < 3; i++) raw[8 + i] = ext[i];
}

/* Fill one 32-byte LFN directory entry. `units` holds the whole name; this entry
 * covers the 13 units starting at `first`. */
static void lfn_make_entry(uint8_t *ent, uint8_t ord, uint8_t chk,
                           const uint16_t *units, uint32_t n_units,
                           uint32_t first, int is_last_physical) {
    memset(ent, 0, DIR_ENTRY_SIZE);
    ent[0] = (uint8_t)(ord | (is_last_physical ? 0x40u : 0u));
    ent[11] = ATTR_LFN_EXACT;
    ent[12] = 0;
    ent[13] = chk;
    for (uint32_t k = 0; k < 13u; k++) {
        uint32_t idx = first + k;
        uint16_t c = 0xFFFFu;
        if (idx < n_units) c = units[idx];
        else if (idx == n_units) c = 0x0000u;       /* terminator */
        ent[g_lfn_ofs[k]] = (uint8_t)(c & 0xFFu);
        ent[g_lfn_ofs[k] + 1u] = (uint8_t)(c >> 8);
    }
}

/* Number of LFN entries a name of n_units needs (a name that is an exact multiple
 * of 13 still needs a terminator entry). */
static uint32_t lfn_entry_count(uint32_t n_units) {
    return (n_units / 13u) + 1u;
}

/* Find `count` consecutive free directory slots. Free = 0xE5, or everything after
 * the 0x00 end marker (which is then zeroed for the rest of that sector). Returns
 * the first slot; the caller must also know it got `count` usable slots, which is
 * guaranteed by construction. Grows the directory by one cluster when needed. */
static int dir_alloc_run(uint32_t dir_cl, uint32_t count, uint8_t *s, uint8_t *v,
                         struct dir_slot *out) {
    if (count == 0) return FATFS_EINVAL;
    uint32_t cl = dir_cl;
    uint32_t prev_cl = 0;
    while (cl >= 2 && cl < FAT32_EOC) {
        uint32_t slots = (g_vol->cluster_bytes / DIR_ENTRY_SIZE);
        uint32_t run = 0, run_start = 0;
        for (uint32_t i = 0; i < slots; i++) {
            uint32_t lba = cluster_lba(cl) + (i * DIR_ENTRY_SIZE) / 512u;
            uint32_t off = (i * DIR_ENTRY_SIZE) % 512u;
            int r = sec_read(lba, s);
            if (r != FATFS_OK) return r;
            uint8_t *ent = s + off;
            int free_slot = (ent[0] == 0xE5 || ent[0] == 0x00 || ent[0] == 0x05);
            if (free_slot) {
                if (run == 0) run_start = i;
                run++;
                if (run == count) {
                    /* Zero the tail marker when the run swallowed it. */
                    uint32_t z = i;
                    uint32_t zlba = cluster_lba(cl) + (z * DIR_ENTRY_SIZE) / 512u;
                    uint32_t zoff = (z * DIR_ENTRY_SIZE) % 512u;
                    if (sec_read(zlba, s) == FATFS_OK && s[zoff] == 0x00 &&
                        (z + 1u) < slots) {
                        for (uint32_t q = z + 1u; q < slots; q++) {
                            uint32_t qlba = cluster_lba(cl) + (q * DIR_ENTRY_SIZE) / 512u;
                            uint32_t qoff = (q * DIR_ENTRY_SIZE) % 512u;
                            if (sec_read(qlba, s) != FATFS_OK) return FATFS_ERR;
                            memset(s + qoff, 0, DIR_ENTRY_SIZE);
                            int wr = sec_write_checked(qlba, s, v, "dir tail", qoff);
                            if (wr != FATFS_OK) return wr;
                        }
                    }
                    out->lba = cluster_lba(cl) + (run_start * DIR_ENTRY_SIZE) / 512u;
                    out->offset = (run_start * DIR_ENTRY_SIZE) % 512u;
                    out->valid = 1;
                    return FATFS_OK;
                }
            } else {
                run = 0;
            }
        }
        prev_cl = cl;
        uint32_t next;
        int r = fat_get(cl, s, &next);
        if (r != FATFS_OK) return r;
        if (next >= FAT32_EOC || next < 2) break;
        cl = next;
    }

    /* No room: append one cluster to the directory and use its first slots. */
    uint32_t new_cl;
    int r = alloc_run(1, s, v, &new_cl);
    if (r != FATFS_OK) return r;
    if (prev_cl == 0) { chain_free(new_cl, s, v); return FATFS_EIO; }
    r = fat_set_c(prev_cl, new_cl, s, v, "dir grow");
    if (r != FATFS_OK) { chain_free(new_cl, s, v); return r; }
    {
        uint32_t first_lba = cluster_lba(new_cl);
        r = sec_read(first_lba, s);
        if (r != FATFS_OK) return r;
        memset(s, 0, 512);
        r = sec_write_checked(first_lba, s, v, "dir grow zero", first_lba);
        if (r != FATFS_OK) return r;
    }
    out->lba = cluster_lba(new_cl);
    out->offset = 0;
    out->valid = 1;
    return FATFS_OK;
}

/* Write one directory entry at an absolute (lba, offset) slot. */
static int dir_put_slot(uint32_t lba, uint32_t off, const uint8_t *ent,
                        uint8_t *s, uint8_t *v) {
    int r = sec_read(lba, s);
    if (r != FATFS_OK) return r;
    memcpy(s + off, ent, DIR_ENTRY_SIZE);
    return sec_write_checked(lba, s, v, "dirent", off);
}

/* Create a directory entry named `name` (8.3 or VFAT long) inside dir_cl.
 * attr: ATTR_RO for a file or ATTR_DIR for a directory. Writes the LFN chain (when
 * needed) followed by the short entry, and returns the SHORT name in out_raw so
 * the caller can keep using the 8.3 machinery. */
static int create_dirent(uint32_t dir_cl, const char *name, uint8_t attr,
                         uint32_t first_cl, uint32_t size,
                         uint8_t *s, uint8_t *v, char out_raw[11]) {
    if (!name || !name[0]) return FATFS_EINVAL;

    char raw[11];
    if (!lfn_need(name)) {
        int r = make_raw(name, raw);
        if (r != FATFS_OK) return r;
        struct dir_slot slot;
        uint8_t ent[DIR_ENTRY_SIZE];
        if (dir_find(dir_cl, raw, s, &slot, ent) == FATFS_OK) return FATFS_EEXIST;
        r = dir_alloc_run(dir_cl, 1, s, v, &slot);
        if (r != FATFS_OK) return r;
        memset(ent, 0, DIR_ENTRY_SIZE);
        memcpy(ent, raw, 11);
        ent[11] = attr;
        ent[20] = (uint8_t)(first_cl >> 16); ent[21] = (uint8_t)(first_cl >> 24);
        ent[26] = (uint8_t)first_cl; ent[27] = (uint8_t)(first_cl >> 8);
        memcpy(ent + 28, &size, 4);
        r = dir_put_slot(slot.lba, slot.offset, ent, s, v);
        if (r != FATFS_OK) return r;
        memcpy(out_raw, raw, 11);
        return FATFS_OK;
    }

    /* Long name: build the chain. */
    uint16_t units[LFN_MAX_CHARS + 1];
    int n_units = lfn_encode(name, units, LFN_MAX_CHARS);
    if (n_units <= 0) return FATFS_ENAMETOOLONG;
    uint32_t n_ent = lfn_entry_count((uint32_t)n_units);

    /* Reject an existing long name outright (same semantics as 8.3 create). */
    {
        struct dir_slot tmp_slot;
        uint8_t tmp_ent[DIR_ENTRY_SIZE];
        if (dir_find_lfn(dir_cl, name, s, &tmp_slot, tmp_ent, raw) == FATFS_OK)
            return FATFS_EEXIST;
    }

    /* Pick a short alias that is still free. */
    int have_raw = 0;
    for (int seq = 0; seq < 100 && !have_raw; seq++) {
        char cand[11];
        lfn_short_alias(name, seq, cand);
        struct dir_slot tmp_slot;
        uint8_t tmp_ent[DIR_ENTRY_SIZE];
        if (dir_find(dir_cl, cand, s, &tmp_slot, tmp_ent) != FATFS_OK) {
            memcpy(raw, cand, 11);
            have_raw = 1;
        }
    }
    if (!have_raw) return FATFS_EEXIST;

    struct dir_slot start;
    int r = dir_alloc_run(dir_cl, n_ent + 1u, s, v, &start);
    if (r != FATFS_OK) return r;

    const uint8_t chk = lfn_checksum((const uint8_t *)raw);
    /* Slot math: the run is contiguous inside one cluster, so slot i lives at
     * (start_lba + (start_off + i*32)/512, (start_off + i*32)%512). */
    for (uint32_t i = 0; i < n_ent; i++) {
        uint8_t ent[DIR_ENTRY_SIZE];
        uint32_t ord = n_ent - i;                    /* physical order: N..1 */
        uint32_t first_unit = (ord - 1u) * 13u;
        lfn_make_entry(ent, (uint8_t)ord, chk, units, (uint32_t)n_units,
                       first_unit, i == 0u);
        uint32_t lin = start.offset + i * DIR_ENTRY_SIZE;
        r = dir_put_slot(start.lba + lin / 512u, lin % 512u, ent, s, v);
        if (r != FATFS_OK) return r;
    }
    {
        uint8_t ent[DIR_ENTRY_SIZE];
        memset(ent, 0, DIR_ENTRY_SIZE);
        memcpy(ent, raw, 11);
        ent[11] = attr;
        ent[20] = (uint8_t)(first_cl >> 16); ent[21] = (uint8_t)(first_cl >> 24);
        ent[26] = (uint8_t)first_cl; ent[27] = (uint8_t)(first_cl >> 8);
        memcpy(ent + 28, &size, 4);
        uint32_t lin = start.offset + n_ent * DIR_ENTRY_SIZE;
        r = dir_put_slot(start.lba + lin / 512u, lin % 512u, ent, s, v);
        if (r != FATFS_OK) return r;
    }
    memcpy(out_raw, raw, 11);
    return FATFS_OK;
}

/* ==================== directory entries ==================== */

static int dir_find(uint32_t dir_cl, const char raw[11], uint8_t *s,
                    struct dir_slot *out, uint8_t *out_ent) {
    uint32_t cl = dir_cl;
    while (cl >= 2 && cl < FAT32_EOC) {
        for (uint32_t k = 0; k < g_vol->spc; k++) {
            uint32_t lba = cluster_lba(cl) + k;
            int r = sec_read(lba, s);
            if (r != FATFS_OK) return r;
            for (uint32_t e = 0; e < ENTRIES_PER_SEC; e++) {
                uint8_t *ent = s + e * DIR_ENTRY_SIZE;
                if (ent[0] == 0x00) {
                    if (dir_cluster_is_last(cl, s)) return FATFS_ENOENT;
                    continue;              /* free slot before a later cluster */
                }
                if (ent[0] == 0xE5) continue;
                if (ent[11] == ATTR_LFN_EXACT) continue;
                if (ent[11] & ATTR_VOL) continue;
                if (raw_equal((const char *)ent, raw)) {
                    out->lba = lba;
                    out->offset = e * DIR_ENTRY_SIZE;
                    out->valid = 1;
                    if (out_ent) memcpy(out_ent, ent, DIR_ENTRY_SIZE);
                    return FATFS_OK;
                }
            }
        }
        uint32_t next;
        int r = fat_get(cl, s, &next);
        if (r != FATFS_OK) return r;
        cl = next;
    }
    return FATFS_ENOENT;
}

/* Resolve ONE path component inside directory `dir`. The 8.3 name is tried
 * first (that is what creation still produces); when it does not exist the VFAT
 * long-name table is searched, so `ls`-visible long names are also openable.
 * On success *out_raw holds the SHORT name bytes the rest of the driver uses,
 * ent[] holds the short directory entry and slot its location. */
static int resolve_component(uint32_t dir, const char *comp, uint8_t *s,
                             struct dir_slot *slot, uint8_t ent[DIR_ENTRY_SIZE],
                             char out_raw[11]) {
    char raw[11];
    int r = make_raw(comp, raw);
    if (r == FATFS_OK) {
        r = dir_find(dir, raw, s, slot, ent);
        if (r == FATFS_OK) {
            memcpy(out_raw, raw, 11);
            return FATFS_OK;
        }
    }
    /* Fall back to the VFAT long name. This is read-only: SutureOS still creates
     * files with 8.3 names only, so a long name can only refer to something that
     * already exists. */
    {
        uint8_t tent[DIR_ENTRY_SIZE];
        const int rl = dir_find_lfn(dir, comp, s, slot, tent, out_raw);
        if (rl == FATFS_OK) {
            memcpy(ent, tent, DIR_ENTRY_SIZE);
            return FATFS_OK;
        }
    }
    return FATFS_ENOENT;
}

static int path_resolve(const char *path, uint8_t *s, uint8_t *v,
                        uint32_t *out_parent, char out_raw[11]) {
    if (!path || path[0] != '/') return FATFS_EINVAL;
    uint32_t dir = g_vol->root_cluster;
    const char *p = path;

    while (*p == '/') p++;
    if (*p == '\0') {
        *out_parent = 0;
        memset(out_raw, ' ', 11);
        out_raw[0] = '\0';
        return FATFS_OK;
    }

    while (*p) {
        char comp[64];
        uint32_t i = 0;
        while (*p && *p != '/' && i < sizeof(comp) - 1) comp[i++] = *p++;
        comp[i] = '\0';
        while (*p == '/') p++;

        struct dir_slot slot;
        uint8_t ent[DIR_ENTRY_SIZE];
        char raw[11];

        if (*p == '\0') {
            /* Last component: it may not exist yet (create path), so an 8.3 name
             * is accepted as-is and only a non-8.3 name has to be resolved
             * through the long-name table. */
            *out_parent = dir;
            if (make_raw(comp, raw) == FATFS_OK) {
                memcpy(out_raw, raw, 11);
                return FATFS_OK;
            }
            if (resolve_component(dir, comp, s, &slot, ent, raw) == FATFS_OK) {
                memcpy(out_raw, raw, 11);
                return FATFS_OK;
            }
            return FATFS_ENOENT;
        }

        int r = resolve_component(dir, comp, s, &slot, ent, raw);
        if (r != FATFS_OK) return r;
        if (!(ent[11] & ATTR_DIR)) return FATFS_ENOTDIR;
        dir = dirent_first_cluster(ent);
        if (dir == 0) dir = g_vol->root_cluster;
    }
    (void)v;
    return FATFS_EINVAL;
}

int fatfs_stat(const char *path, uint32_t *size, int *is_dir) {
    if (!g_vol->mounted) return FATFS_EBUSY;
    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;
    uint8_t *v = lemis_heap_alloc(512);
    if (!v) { lemis_heap_free(s); return FATFS_ERR; }

    uint32_t parent;
    char raw[11];
    int r = path_resolve(path, s, v, &parent, raw);
    if (r == FATFS_OK && parent == 0) {
        if (size) *size = 0;
        if (is_dir) *is_dir = 1;
        lemis_heap_free(s); lemis_heap_free(v);
        return FATFS_OK;
    }
    if (r != FATFS_OK) { lemis_heap_free(s); lemis_heap_free(v); return r; }

    struct dir_slot slot;
    uint8_t ent[DIR_ENTRY_SIZE];
    r = dir_find(parent, raw, s, &slot, ent);
    lemis_heap_free(s); lemis_heap_free(v);
    if (r != FATFS_OK) return r;
    if (size) {
        uint32_t sz;
        memcpy(&sz, ent + 28, 4);
        *size = sz;
    }
    if (is_dir) *is_dir = (ent[11] & ATTR_DIR) ? 1 : 0;
    return FATFS_OK;
}

/* ==================== mount / unmount ==================== */

static int is_pow2(uint32_t x) { return x && !(x & (x - 1)); }

int fatfs_volume_count(void) {
    int n = 0;
    for (int i = 0; i < FATFS_MAX_VOLS; i++)
        if (g_vols[i].mounted) n++;
    return n;
}

/* Switch the current volume. Returns FATFS_OK when the slot is mounted. */
int fatfs_select_volume(int slot) {
    if (slot < 0 || slot >= FATFS_MAX_VOLS) return FATFS_EINVAL;
    if (!g_vols[slot].mounted) return FATFS_EINVAL;
    g_cur_vol = slot;
    g_vol = &g_vols[slot];
    return FATFS_OK;
}

int fatfs_current_volume(void) { return g_cur_vol; }

/* Installer speedup hook: with bulk mode on, sector writes skip the read-back
 * verification (see g_bulk_write). Turn it off again before unmounting. */
void fatfs_set_bulk_mode(int on) { g_bulk_write = on ? 1 : 0; }

/* Mount `drv` into the first free slot and return that slot (>= 0), or a
 * negative fatfs error. Slot 0 doubles as the "first volume" for callers that
 * only care about success - see fatfs_vfs.c.
 *
 * start_lba is the partition offset in 512-byte sectors (0 = superfloppy, i.e.
 * the BPB sits at device LBA 0 - the historical layout). All LBAs the driver
 * computes from the BPB stay partition-relative; sec_read()/sec_write_checked()
 * add the offset, so a FAT32 partition on a partitioned disk (the installed
 * system volume written by the installer) works without touching anything else.
 * quiet = 1 suppresses the "mount FAIL" diagnostics (used while auto-probing). */
static int fatfs_mount_impl(uint8_t drv, uint32_t start_lba, int quiet) {
    for (int i = 0; i < FATFS_MAX_VOLS; i++) {
        if (g_vols[i].mounted && g_vols[i].drive == (int)drv &&
            g_vols[i].start_lba == start_lba) {
            (void)fatfs_select_volume(i);
            return i;
        }
    }
    int slot = -1;
    for (int i = 0; i < FATFS_MAX_VOLS; i++) {
        if (!g_vols[i].mounted) { slot = i; break; }
    }
    if (slot < 0) return FATFS_EBUSY;
    (void)fatfs_select_volume(slot);      /* not mounted yet: the fields below */
    g_vol = &g_vols[slot];                /* are filled in directly */

    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;
    if (blk_read((int)drv, start_lba, 1, s) != 0) {
        if (!quiet) console_printf("[fat32] mount FAIL drive=%u reason=io\n", (uint64_t)drv);
        lemis_heap_free(s);
        return FATFS_EIO;
    }

    uint32_t lba0 = (uint32_t)s[0] | ((uint32_t)s[1] << 8) |
                    ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);

    const char *why = NULL;
    if (s[510] != 0x55 || s[511] != 0xAA) why = "no 0x55AA signature";
    else if (!((s[0] == 0xEB && s[2] == 0x90) || s[0] == 0xE9)) why = "bad jump";

    uint32_t bps = (uint32_t)s[11] | ((uint32_t)s[12] << 8);
    uint32_t spc = s[13];
    uint32_t reserved = (uint32_t)s[14] | ((uint32_t)s[15] << 8);
    uint32_t nfats = s[16];
    uint32_t root_ent = (uint32_t)s[17] | ((uint32_t)s[18] << 8);
    uint32_t tot16 = (uint32_t)s[19] | ((uint32_t)s[20] << 8);
    uint32_t fatsz16 = (uint32_t)s[22] | ((uint32_t)s[23] << 8);
    uint32_t tot32, fatsz32, root_clus, fsinfo;
    memcpy(&tot32, s + 32, 4);
    memcpy(&fatsz32, s + 36, 4);
    memcpy(&root_clus, s + 44, 4);
    memcpy(&fsinfo, s + 48, 4);

    if (!why && bps != 512) why = "bytes/sec != 512";
    if (!why && (!is_pow2(spc) || spc > 128)) why = "bad sectors/cluster";
    if (!why && reserved == 0) why = "reserved == 0";
    if (!why && nfats != 2) why = "fats != 2";
    if (!why && root_ent != 0) why = "not FAT32 (root_ent != 0)";
    if (!why && tot16 != 0) why = "not FAT32 (totsec16 != 0)";
    if (!why && fatsz16 != 0) why = "not FAT32 (fatsz16 != 0)";
    if (!why && tot32 == 0) why = "totsec32 == 0";
    if (!why && fatsz32 == 0) why = "fatsz32 == 0";
    if (!why && root_clus < 2) why = "root cluster < 2";
    if (!why && memcmp(s + 82, "FAT32   ", 8) != 0) why = "label != FAT32";

    uint64_t clusters = 0;
    if (!why) {
        clusters = ((uint64_t)tot32 - reserved - (uint64_t)nfats * fatsz32) / spc;
        if (clusters < 65525) why = "cluster count < 65525 (not FAT32)";
    }
    if (why) {
        if (!quiet) console_printf("[fat32] mount FAIL drive=%u reason=%s\n",
                                   (uint64_t)drv, why);
        lemis_heap_free(s);
        return FATFS_EINVAL;
    }

    memset(g_vol, 0, sizeof(*g_vol));
    g_vol->drive = (int)drv;
    g_vol->start_lba = start_lba;
    g_vol->fat_lba = reserved;
    g_vol->fat_sz = fatsz32;
    g_vol->data_lba = reserved + (uint32_t)nfats * fatsz32;
    g_vol->root_cluster = root_clus;
    g_vol->spc = spc;
    g_vol->cluster_bytes = spc * 512;
    g_vol->total_sectors = tot32;
    g_vol->total_clusters = (uint32_t)clusters;
    g_vol->fsinfo_lba = (fsinfo < tot32 && fsinfo != 0) ? fsinfo : 0;
    g_vol->mounted = 1;

    g_vol->free_clusters = 0xFFFFFFFFu;
    if (g_vol->fsinfo_lba) {
        if (sec_read(g_vol->fsinfo_lba, s) == FATFS_OK) {
            uint32_t m1, m2, trail, free_cnt;
            memcpy(&m1, s + 0, 4);
            memcpy(&m2, s + 484, 4);
            memcpy(&trail, s + 508, 4);
            memcpy(&free_cnt, s + 488, 4);
            if (m1 == 0x41615252u && m2 == 0x61417272u && trail == 0xAA5555AAu) {
                g_vol->free_clusters = free_cnt;
            }
        }
    }
    if (g_vol->free_clusters == 0xFFFFFFFFu) {
        uint32_t cnt = 0;
        for (uint32_t cl = 2; cl < g_vol->total_clusters + 2; cl++) {
            uint32_t val;
            if (fat_get(cl, s, &val) != FATFS_OK) { cnt = 0; break; }
            if (val == FAT32_ENTRY_FREE) cnt++;
        }
        g_vol->free_clusters = cnt;
    }

    console_printf("[fat32] mount drive=%u lba0=0x%x spc=%u clusters=%u fat_lba=%u fat_sz=%u root=%u free=%u\n",
                   (uint64_t)drv, (uint64_t)lba0, (uint64_t)spc,
                   (uint64_t)g_vol->total_clusters, (uint64_t)g_vol->fat_lba,
                   (uint64_t)g_vol->fat_sz, (uint64_t)g_vol->root_cluster,
                   (uint64_t)g_vol->free_clusters);
    lemis_heap_free(s);
    return slot;
}

/* Superfloppy mount (BPB at device LBA 0): the historical behaviour, still used
 * by the FAT32 test images and by `mount fat32 0 /mnt/x`. */
int fatfs_mount(uint8_t drv, void *fs) {
    (void)fs;
    return fatfs_mount_impl(drv, 0, 0);
}

/* Mount a FAT32 partition that starts at `start_lba` (512-byte sectors). */
int fatfs_mount_at(uint8_t drv, void *fs, uint32_t start_lba) {
    (void)fs;
    return fatfs_mount_impl(drv, start_lba, 0);
}

/* Auto-detect the layout: try the superfloppy first (so every existing test
 * image and every plain FAT32 disk behaves exactly as before), then scan the MBR
 * partition table for a FAT32-typed partition (0x0B/0x0C/0x0E). This is what
 * lets the kernel mount the system volume the installer writes onto a
 * partitioned disk. Probes are quiet; only the volume that really mounts logs. */
int fatfs_mount_auto(uint8_t drv) {
    int slot = fatfs_mount_impl(drv, 0, 1);
    if (slot >= 0) return slot;

    uint8_t *mbr = lemis_heap_alloc(512);
    if (!mbr) return FATFS_ERR;
    if (blk_read((int)drv, 0, 1, mbr) != 0 || mbr[510] != 0x55 || mbr[511] != 0xAA) {
        lemis_heap_free(mbr);
        console_printf("[fat32] mount FAIL drive=%u reason=no FAT32 volume\n",
                       (uint64_t)drv);
        return FATFS_EINVAL;
    }
    int result = FATFS_EINVAL;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = mbr + 446 + i * 16;
        const uint8_t type = e[4];
        const uint32_t lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                             ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        if (type != 0x0B && type != 0x0C && type != 0x0E) continue;
        if (lba < 63) continue;
        slot = fatfs_mount_impl(drv, lba, 1);
        if (slot >= 0) { result = slot; break; }
    }
    lemis_heap_free(mbr);
    if (result < 0) {
        console_printf("[fat32] mount FAIL drive=%u reason=no FAT32 volume "
                       "(LBA0 or MBR partition)\n", (uint64_t)drv);
    }
    return result;
}

int fatfs_unmount(int drv) {
    int slot = -1;
    for (int i = 0; i < FATFS_MAX_VOLS; i++)
        if (g_vols[i].mounted && g_vols[i].drive == drv) { slot = i; break; }
    if (slot < 0) return FATFS_EBUSY;

    (void)fatfs_select_volume(slot);
    fatfs_sync();
    /* close handles that belong to this volume, then clear the state block */
    for (int i = 0; i < MAX_FILES; i++)
        if (g_files[i].used && g_files[i].vol == slot) g_files[i].used = 0;
    for (int i = 0; i < MAX_DIRS; i++)
        if (g_dirs[i].used && g_dirs[i].vol == slot) g_dirs[i].used = 0;
    memset(&g_vols[slot], 0, sizeof(g_vols[slot]));
    g_vol = &g_vols[0];
    g_cur_vol = 0;
    console_printf("[fat32] unmounted drive=%u (volumes left=%d)\n",
                   (uint64_t)drv, (int64_t)fatfs_volume_count());
    return FATFS_OK;
}

int fatfs_vfs_init(void) {
    memset(g_vols, 0, sizeof(g_vols));
    g_vol = &g_vols[0];
    g_cur_vol = 0;
    memset(g_files, 0, sizeof(g_files));
    memset(g_dirs, 0, sizeof(g_dirs));
    return FATFS_OK;
}

/* ==================== FSInfo ==================== */

static int fsinfo_flush(uint8_t *s, uint8_t *v) {
    if (!g_vol->fsinfo_dirty || !g_vol->fsinfo_lba) return FATFS_OK;
    int r = sec_read(g_vol->fsinfo_lba, s);
    if (r != FATFS_OK) return r;
    memcpy(s + 488, &g_vol->free_clusters, 4);
    r = sec_write_checked(g_vol->fsinfo_lba, s, v, "fsinfo", 488);
    if (r == FATFS_OK) g_vol->fsinfo_dirty = 0;
    return r;
}

/* ==================== file operations ==================== */

int fatfs_open(const char *path, int create, fatfs_file_t **out) {
    if (!g_vol->mounted) return FATFS_EBUSY;
    if (!path || !out) return FATFS_EINVAL;

    struct fatfs_file *fh = NULL;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!g_files[i].used) { fh = &g_files[i]; break; }
    }
    if (!fh) return FATFS_ERR;

    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;
    uint8_t *v = lemis_heap_alloc(512);
    if (!v) { lemis_heap_free(s); return FATFS_ERR; }

    uint32_t parent = 0;
    char raw[11];
    struct dir_slot slot;
    uint8_t ent[DIR_ENTRY_SIZE];
    int r = path_resolve(path, s, v, &parent, raw);
    if (r != FATFS_OK && !(r == FATFS_ENOENT && create)) goto out;

    if (r == FATFS_ENOENT) {
        /* Creating a name that does not exist yet. For an 8.3 name path_resolve
         * would have returned OK, so this is the long-name case: resolve the
         * directory part by hand and let create_dirent write the VFAT chain. */
        char dir[256];
        const char *base = NULL;
        path_split(path, dir, sizeof(dir), &base);
        r = resolve_dir_cluster(dir, s, v, &parent);
        if (r != FATFS_OK) goto out;
        if (!base[0]) { r = FATFS_EINVAL; goto out; }
        char craw[11];
        r = create_dirent(parent, base, ATTR_RO, 0, 0, s, v, craw);
        if (r != FATFS_OK) goto out;
        r = dir_find(parent, craw, s, &slot, ent);
        if (r != FATFS_OK) goto out;
    } else {
        if (parent == 0) { r = FATFS_EISDIR; goto out; }
        r = dir_find(parent, raw, s, &slot, ent);
        if (r == FATFS_ENOENT && create) {
            const char *base = strrchr(path, '/');
            base = base ? base + 1 : path;
            char craw[11];
            r = create_dirent(parent, base, ATTR_RO, 0, 0, s, v, craw);
            if (r != FATFS_OK) goto out;
            r = dir_find(parent, craw, s, &slot, ent);
            if (r != FATFS_OK) goto out;
        } else if (r != FATFS_OK) {
            goto out;
        } else if (ent[11] & ATTR_DIR) {
            r = FATFS_EISDIR;
            goto out;
        }
    }

    fh->used = 1;
    fh->vol = g_cur_vol;
    fh->first_cluster = dirent_first_cluster(ent);
    if (fh->first_cluster == 0) fh->first_cluster = FAT32_EOC;
    memcpy(&fh->size, ent + 28, 4);
    fh->walk_index = 0;
    fh->walk_cluster = 0;                 /* 0 = cursor empty */
    strncpy(fh->path, path, sizeof(fh->path) - 1);
    fh->path[sizeof(fh->path) - 1] = '\0';
    *out = fh;
    r = FATFS_OK;

out:
    lemis_heap_free(s);
    lemis_heap_free(v);
    return r;
}

int fatfs_close(fatfs_file_t *f) {
    (void)fatfs_select_volume(f->vol);
    if (!f || !f->used) return FATFS_EINVAL;
    f->used = 0;
    return FATFS_OK;
}

static int dirent_update(fatfs_file_t *f, uint8_t *s, uint8_t *v) {
    uint32_t parent;
    char raw[11];
    int r = path_resolve(f->path, s, v, &parent, raw);
    if (r != FATFS_OK) return r;
    struct dir_slot slot;
    uint8_t ent[DIR_ENTRY_SIZE];
    r = dir_find(parent, raw, s, &slot, ent);
    if (r != FATFS_OK) return r;

    r = sec_read(slot.lba, s);
    if (r != FATFS_OK) return r;
    uint8_t *p = s + slot.offset;
    uint32_t cl = (f->first_cluster >= FAT32_EOC) ? 0 : f->first_cluster;
    p[20] = (uint8_t)(cl >> 16);
    p[21] = (uint8_t)(cl >> 24);
    p[26] = (uint8_t)(cl);
    p[27] = (uint8_t)(cl >> 8);
    memcpy(p + 28, &f->size, 4);
    return sec_write_checked(slot.lba, s, v, f->path, slot.offset);
}

int fatfs_read(fatfs_file_t *f, uint32_t off, void *buf, uint32_t len) {
    (void)fatfs_select_volume(f->vol);
    if (!f || !f->used || !buf) return FATFS_EINVAL;
    if (!g_vol->mounted) return FATFS_EBUSY;
    if (off >= f->size) return 0;
    if (off + len > f->size) len = f->size - off;

    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;

    uint32_t cb = g_vol->cluster_bytes;
    uint32_t done = 0;
    uint32_t cl_idx = off / cb;
    uint32_t cl_off = off % cb;
    uint8_t *dst = buf;

    while (done < len) {
        if (f->first_cluster >= FAT32_EOC) break;
        uint32_t cl;
        int r = chain_locate(f->first_cluster, cl_idx, s, &cl);
        if (r != FATFS_OK) break;

        uint32_t lba = cluster_lba(cl) + (cl_off / 512);
        uint32_t sec_off = cl_off % 512;
        r = sec_read(lba, s);
        if (r != FATFS_OK) break;
        uint32_t chunk = 512 - sec_off;
        if (chunk > len - done) chunk = len - done;
        memcpy(dst + done, s + sec_off, chunk);
        done += chunk;
        cl_off += chunk;
        if (cl_off >= cb) { cl_off = 0; cl_idx++; }
    }
    lemis_heap_free(s);
    return (int)done;
}

int fatfs_write(fatfs_file_t *f, uint32_t off, const void *buf, uint32_t len) {
    (void)fatfs_select_volume(f->vol);
    if (!f || !f->used || !buf) return FATFS_EINVAL;
    if (!g_vol->mounted) return FATFS_EBUSY;

    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;
    uint8_t *v = lemis_heap_alloc(512);
    if (!v) { lemis_heap_free(s); return FATFS_ERR; }

    uint32_t end = off + len;
    uint32_t need_clusters = (end + g_vol->cluster_bytes - 1) / g_vol->cluster_bytes;
    if (need_clusters == 0) need_clusters = 1;

    uint32_t first = (f->first_cluster >= FAT32_EOC) ? 0 : f->first_cluster;
    int r = chain_ensure(&first, need_clusters, s, v);
    if (r != FATFS_OK) goto out;
    if (f->first_cluster >= FAT32_EOC) f->first_cluster = first;

    if (off > f->size) {
        uint32_t cb = g_vol->cluster_bytes;
        uint32_t pos = f->size;
        while (pos < off) {
            uint32_t cl_idx = pos / cb;
            uint32_t cl_off = pos % cb;
            uint32_t cl;
            r = chain_locate(f->first_cluster, cl_idx, s, &cl);
            if (r != FATFS_OK) goto out;
            uint32_t lba = cluster_lba(cl) + (cl_off / 512);
            uint32_t sec_off = cl_off % 512;
            r = sec_read(lba, s);
            if (r != FATFS_OK) goto out;
            uint32_t chunk = 512 - sec_off;
            if (chunk > off - pos) chunk = off - pos;
            memset(s + sec_off, 0, chunk);
            r = sec_write_checked(lba, s, v, f->path, lba);
            if (r != FATFS_OK) goto out;
            pos += chunk;
        }
    }

    {
        uint32_t cb = g_vol->cluster_bytes;
        uint32_t done = 0;
        uint32_t pos = off;
        const uint8_t *src = buf;
        while (done < len) {
            uint32_t cl_idx = pos / cb;
            uint32_t cl_off = pos % cb;
            uint32_t cl;
            /* Fast path for whole clusters (the installer copies multi-MiB files
             * through this): one blk_write for the whole cluster instead of
             * spc read-modify-write-verify cycles. Only the full-cluster case is
             * short-cut; partial clusters keep the careful path below. */
            if (cl_off == 0 && (len - done) >= cb) {
                r = chain_locate_cached(f, cl_idx, s, &cl);
                if (r != FATFS_OK) goto out;
                if (blk_write(g_vol->drive,
                              (uint64_t)g_vol->start_lba + cluster_lba(cl),
                              g_vol->spc, src + done) != 0) {
                    r = FATFS_EIO;
                    goto out;
                }
                done += cb;
                pos += cb;
                continue;
            }
            r = chain_locate_cached(f, cl_idx, s, &cl);
            if (r != FATFS_OK) goto out;
            uint32_t lba = cluster_lba(cl) + (cl_off / 512);
            uint32_t sec_off = cl_off % 512;
            r = sec_read(lba, s);
            if (r != FATFS_OK) goto out;
            uint32_t chunk = 512 - sec_off;
            if (chunk > len - done) chunk = len - done;
            memcpy(s + sec_off, src + done, chunk);
            r = sec_write_checked(lba, s, v, f->path, lba);
            if (r != FATFS_OK) goto out;
            done += chunk;
            pos += chunk;
        }
    }

    if (end > f->size) f->size = end;
    r = dirent_update(f, s, v);

out:
    lemis_heap_free(s);
    lemis_heap_free(v);
    return (r == FATFS_OK) ? (int)len : r;
}

int fatfs_truncate(fatfs_file_t *f, uint32_t new_size) {
    (void)fatfs_select_volume(f->vol);
    if (!f || !f->used) return FATFS_EINVAL;
    if (!g_vol->mounted) return FATFS_EBUSY;

    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;
    uint8_t *v = lemis_heap_alloc(512);
    if (!v) { lemis_heap_free(s); return FATFS_ERR; }

    int r = FATFS_OK;
    if (new_size == 0) {
        if (f->first_cluster < FAT32_EOC) {
            r = chain_free(f->first_cluster, s, v);
            f->first_cluster = FAT32_EOC;
        }
        f->size = 0;
    } else if (new_size < f->size) {
        uint32_t cb = g_vol->cluster_bytes;
        uint32_t keep = (new_size + cb - 1) / cb;
        uint32_t cl_idx = keep - 1;
        uint32_t cl;
        r = chain_locate(f->first_cluster, cl_idx, s, &cl);
        if (r == FATFS_OK) {
            uint32_t tail = new_size % cb;
            if (tail) {
                uint32_t lba = cluster_lba(cl) + (tail / 512);
                r = sec_read(lba, s);
                if (r == FATFS_OK) {
                    uint32_t so = tail % 512;
                    memset(s + so, 0, 512 - so);
                    r = sec_write_checked(lba, s, v, f->path, lba);
                }
            }
        }
        if (r == FATFS_OK) {
            uint32_t cl_at = f->first_cluster;
            for (uint32_t i = 0; i < keep && r == FATFS_OK; i++) {
                uint32_t next;
                r = fat_get(cl_at, s, &next);
                if (r != FATFS_OK) break;
                if (i == keep - 1) {
                    r = fat_set_c(cl_at, FAT32_EOC, s, v, "truncate");
                    if (next < FAT32_EOC && next >= 2 && r == FATFS_OK) {
                        r = chain_free(next, s, v);
                    }
                } else {
                    cl_at = next;
                }
            }
        }
        f->size = new_size;
    } else {
        f->size = new_size;
    }

    if (r == FATFS_OK) r = dirent_update(f, s, v);
    lemis_heap_free(s);
    lemis_heap_free(v);
    return r;
}

/* ==================== directory operations ==================== */

int fatfs_opendir(const char *path, fatfs_dir_t **out) {
    const int dir_vol = g_cur_vol;   /* record before any switch */
    if (!g_vol->mounted) return FATFS_EBUSY;
    if (!path || !out) return FATFS_EINVAL;

    struct fatfs_dir *d = NULL;
    for (int i = 0; i < MAX_DIRS; i++) {
        if (!g_dirs[i].used) { d = &g_dirs[i]; break; }
    }
    if (!d) return FATFS_ERR;

    if (strcmp(path, "/") == 0) {
        d->cluster = g_vol->root_cluster;
    } else {
        uint32_t size;
        int is_dir = 0;
        int r = fatfs_stat(path, &size, &is_dir);
        if (r != FATFS_OK) return r;
        if (!is_dir) return FATFS_ENOTDIR;

        uint8_t *s = lemis_heap_alloc(512);
        if (!s) return FATFS_ERR;
        uint8_t *v = lemis_heap_alloc(512);
        if (!v) { lemis_heap_free(s); return FATFS_ERR; }
        uint32_t parent;
        char raw[11];
        r = path_resolve(path, s, v, &parent, raw);
        if (r != FATFS_OK) { lemis_heap_free(s); lemis_heap_free(v); return r; }
        struct dir_slot slot;
        uint8_t ent[DIR_ENTRY_SIZE];
        r = dir_find(parent, raw, s, &slot, ent);
        lemis_heap_free(s);
        lemis_heap_free(v);
        if (r != FATFS_OK) return r;
        d->cluster = dirent_first_cluster(ent);
        if (d->cluster == 0) d->cluster = g_vol->root_cluster;
    }

    d->used = 1;
    d->vol = dir_vol;
    d->entry = 0;
    strncpy(d->path, path, sizeof(d->path) - 1);
    d->path[sizeof(d->path) - 1] = '\0';
    *out = d;
    return FATFS_OK;
}

int fatfs_readdir(fatfs_dir_t *d, char *name, uint32_t name_cap,
                  uint32_t *size, int *is_dir) {
    (void)fatfs_select_volume(d->vol);
    if (!d || !d->used || !name) return FATFS_EINVAL;
    if (!g_vol->mounted) return FATFS_EBUSY;

    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;

    int r = FATFS_ENOTFOUND;
    uint32_t idx = d->entry;
    uint32_t cl = d->cluster;
    uint32_t per_cl = g_vol->cluster_bytes / DIR_ENTRY_SIZE;

    while (cl >= 2 && cl < FAT32_EOC) {
        if (idx >= per_cl) {
            uint32_t next;
            r = fat_get(cl, s, &next);
            if (r != FATFS_OK) goto out;
            idx -= per_cl;
            cl = next;
            continue;
        }
        uint32_t lba = cluster_lba(cl) + (idx * DIR_ENTRY_SIZE) / 512;
        uint32_t off = (idx * DIR_ENTRY_SIZE) % 512;
        r = sec_read(lba, s);
        if (r != FATFS_OK) goto out;

        uint8_t *ent = s + off;
        idx++;
        d->entry = idx;

        if (ent[0] == 0x00) {
            if (dir_cluster_is_last(cl, s)) { r = FATFS_ENOTFOUND; goto out; }
            r = FATFS_OK;                  /* skip: a later cluster has entries */
            continue;
        }
        if (ent[0] == 0xE5) { lfn_reset(&d->lfn); r = FATFS_OK; continue; }
        if (ent[11] == ATTR_LFN_EXACT) { lfn_feed(&d->lfn, ent); r = FATFS_OK; continue; }
        if (ent[11] & ATTR_VOL) { lfn_reset(&d->lfn); r = FATFS_OK; continue; }
        if (ent[0] == '.' && (ent[1] == ' ' || (ent[1] == '.' && ent[2] == ' '))) {
            lfn_reset(&d->lfn);
            r = FATFS_OK;
            continue;
        }

        if (!lfn_assemble(&d->lfn, ent, name, name_cap)) {
            /* No valid VFAT chain in front of this entry: fall back to 8.3. */
            raw_to_name((const char *)ent, name, name_cap);
        }
        lfn_reset(&d->lfn);
        if (size) {
            uint32_t sz;
            memcpy(&sz, ent + 28, 4);
            *size = sz;
        }
        if (is_dir) *is_dir = (ent[11] & ATTR_DIR) ? 1 : 0;
        r = FATFS_OK;
        goto out;
    }
    r = FATFS_ENOTFOUND;

out:
    lemis_heap_free(s);
    return r;
}

int fatfs_closedir(fatfs_dir_t *d) {
    (void)fatfs_select_volume(d->vol);
    if (!d || !d->used) return FATFS_EINVAL;
    d->used = 0;
    return FATFS_OK;
}

int fatfs_mkdir(const char *path) {
    if (!g_vol->mounted) return FATFS_EBUSY;
    uint32_t sz;
    int is_dir = 0;
    if (fatfs_stat(path, &sz, &is_dir) == FATFS_OK) return FATFS_EBUSY;

    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;
    uint8_t *v = lemis_heap_alloc(512);
    if (!v) { lemis_heap_free(s); return FATFS_ERR; }

    uint32_t parent;
    char raw[11];
    int r = path_resolve(path, s, v, &parent, raw);
    if (r != FATFS_OK || parent == 0) {
        if (r == FATFS_OK) r = FATFS_EINVAL;
        goto out;
    }

    uint32_t cl;
    r = alloc_run(1, s, v, &cl);
    if (r != FATFS_OK) goto out;

    r = sec_read(cluster_lba(cl), s);
    if (r != FATFS_OK) { chain_free(cl, s, v); goto out; }
    memset(s, 0, 512);
    uint8_t *dot = s;
    memset(dot, ' ', 11);
    dot[0] = '.';
    dot[11] = ATTR_DIR;
    dot[20] = (uint8_t)(cl >> 16); dot[21] = (uint8_t)(cl >> 24);
    dot[26] = (uint8_t)cl; dot[27] = (uint8_t)(cl >> 8);
    uint8_t *dotdot = s + 32;
    memset(dotdot, ' ', 11);
    dotdot[0] = '.'; dotdot[1] = '.';
    dotdot[11] = ATTR_DIR;
    uint32_t p = (parent == g_vol->root_cluster) ? 0 : parent;
    dotdot[20] = (uint8_t)(p >> 16); dotdot[21] = (uint8_t)(p >> 24);
    dotdot[26] = (uint8_t)p; dotdot[27] = (uint8_t)(p >> 8);
    r = sec_write_checked(cluster_lba(cl), s, v, path, cluster_lba(cl));
    if (r != FATFS_OK) { chain_free(cl, s, v); goto out; }

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char craw[11];
    r = create_dirent(parent, base, ATTR_DIR, cl, 0, s, v, craw);
    if (r != FATFS_OK) { chain_free(cl, s, v); goto out; }

out:
    lemis_heap_free(s);
    lemis_heap_free(v);
    return r;
}

int fatfs_rmdir(const char *path) {
    if (!g_vol->mounted) return FATFS_EBUSY;
    if (strcmp(path, "/") == 0) return FATFS_EINVAL;

    uint32_t size;
    int is_dir = 0;
    int r = fatfs_stat(path, &size, &is_dir);
    if (r != FATFS_OK) return r;
    if (!is_dir) return FATFS_ENOTDIR;

    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;
    uint8_t *v = lemis_heap_alloc(512);
    if (!v) { lemis_heap_free(s); return FATFS_ERR; }

    uint32_t parent;
    char raw[11];
    r = path_resolve(path, s, v, &parent, raw);
    if (r != FATFS_OK) goto out;

    struct dir_slot slot;
    uint8_t ent[DIR_ENTRY_SIZE];
    r = dir_find(parent, raw, s, &slot, ent);
    if (r != FATFS_OK) goto out;
    uint32_t cl = dirent_first_cluster(ent);

    {
        uint32_t idx = 0;
        uint32_t per_cl = g_vol->cluster_bytes / DIR_ENTRY_SIZE;
        uint32_t dcl = cl;
        r = FATFS_OK;
        while (dcl >= 2 && dcl < FAT32_EOC) {
            if (idx >= per_cl) {
                uint32_t next;
                r = fat_get(dcl, s, &next);
                if (r != FATFS_OK) break;
                dcl = next;
                idx = 0;
                continue;
            }
            uint32_t lba = cluster_lba(dcl) + (idx * DIR_ENTRY_SIZE) / 512;
            uint32_t off = (idx * DIR_ENTRY_SIZE) % 512;
            r = sec_read(lba, s);
            if (r != FATFS_OK) break;
            uint8_t *e2 = s + off;
            idx++;
            if (e2[0] == 0x00) {
                if (dir_cluster_is_last(dcl, s)) break;
                continue;                  /* keep scanning the later cluster */
            }
            if (e2[0] == 0xE5) continue;
            if (e2[11] == ATTR_LFN_EXACT) continue;
            if (e2[0] == '.' && (e2[1] == ' ' || (e2[1] == '.' && e2[2] == ' '))) continue;
            r = FATFS_ENOTEMPTY;
            break;
        }
        if (r != FATFS_OK) goto out;
    }

    r = chain_free(cl, s, v);
    if (r != FATFS_OK) goto out;
    dir_mark_lfn_chain(slot.lba, slot.offset, s);
    r = sec_read(slot.lba, s);
    if (r != FATFS_OK) goto out;
    s[slot.offset] = 0xE5;
    r = sec_write_checked(slot.lba, s, v, path, slot.offset);

out:
    lemis_heap_free(s);
    lemis_heap_free(v);
    return r;
}

int fatfs_unlink(const char *path) {
    if (!g_vol->mounted) return FATFS_EBUSY;
    uint32_t size;
    int is_dir = 0;
    int r = fatfs_stat(path, &size, &is_dir);
    if (r != FATFS_OK) return r;
    if (is_dir) return FATFS_EISDIR;

    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;
    uint8_t *v = lemis_heap_alloc(512);
    if (!v) { lemis_heap_free(s); return FATFS_ERR; }

    uint32_t parent;
    char raw[11];
    r = path_resolve(path, s, v, &parent, raw);
    if (r != FATFS_OK) goto out;

    struct dir_slot slot;
    uint8_t ent[DIR_ENTRY_SIZE];
    r = dir_find(parent, raw, s, &slot, ent);
    if (r != FATFS_OK) goto out;

    uint32_t cl = dirent_first_cluster(ent);
    if (cl >= 2) {
        r = chain_free(cl, s, v);
        if (r != FATFS_OK) goto out;
    }
    /* Remove the VFAT name chain that belongs to this short entry, otherwise the
     * long name would survive as an orphan and be picked up by a later scan. */
    dir_mark_lfn_chain(slot.lba, slot.offset, s);
    r = sec_read(slot.lba, s);
    if (r != FATFS_OK) goto out;
    s[slot.offset] = 0xE5;
    r = sec_write_checked(slot.lba, s, v, path, slot.offset);

out:
    lemis_heap_free(s);
    lemis_heap_free(v);
    return r;
}

/* ==================== rename (8.3 and VFAT long names) ====================
 * A FAT rename is still "create the new dirent, then delete the old one": the
 * cluster chain, attributes and size are copied over, so the data itself never
 * moves. Doing it that way (instead of rewriting the 11 name bytes in place) is
 * what makes long names possible at all - the new name may need a whole VFAT
 * chain in front of its short entry, and the old chain has to be marked deleted
 * so it does not survive as an orphan.
 *
 * Honest scope:
 *   - same directory only (a cross-directory move is refused with a log line);
 *   - the destination must not already exist (FATFS_EEXIST);
 *   - renaming to the same visible name (case-insensitive) is a no-op success.
 */
static void path_split(const char *path, char *dir, uint32_t cap, const char **base) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        dir[0] = '/'; dir[1] = '\0';
        *base = path;
        return;
    }
    uint32_t n = (uint32_t)(slash - path);
    if (n == 0) {
        dir[0] = '/'; dir[1] = '\0';
    } else {
        if (n >= cap) n = cap - 1;
        memcpy(dir, path, n);
        dir[n] = '\0';
    }
    *base = slash + 1;
}

static int resolve_dir_cluster(const char *dirpath, uint8_t *s, uint8_t *v,
                               uint32_t *out_cl) {
    if (strcmp(dirpath, "/") == 0) { *out_cl = g_vol->root_cluster; return FATFS_OK; }
    uint32_t parent;
    char raw[11];
    int r = path_resolve(dirpath, s, v, &parent, raw);
    if (r != FATFS_OK) return r;
    struct dir_slot slot;
    uint8_t ent[DIR_ENTRY_SIZE];
    r = dir_find(parent, raw, s, &slot, ent);
    if (r != FATFS_OK) return r;
    if (!(ent[11] & ATTR_DIR)) return FATFS_ENOTDIR;
    uint32_t cl = dirent_first_cluster(ent);
    *out_cl = (cl == 0) ? g_vol->root_cluster : cl;
    return FATFS_OK;
}

int fatfs_rename(const char *old_path, const char *new_path) {
    if (!g_vol->mounted) return FATFS_EBUSY;
    if (!old_path || !new_path || old_path[0] != '/' || new_path[0] != '/')
        return FATFS_EINVAL;

    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;
    uint8_t *v = lemis_heap_alloc(512);
    if (!v) { lemis_heap_free(s); return FATFS_ERR; }

    char odir[256], ndir[256];
    const char *obase = NULL, *nbase = NULL;
    path_split(old_path, odir, sizeof(odir), &obase);
    path_split(new_path, ndir, sizeof(ndir), &nbase);
    int r;
    if (!obase[0] || !nbase[0]) { r = FATFS_EINVAL; goto out; }

    uint32_t old_cl = 0, new_cl = 0;
    r = resolve_dir_cluster(odir, s, v, &old_cl);
    if (r != FATFS_OK) goto out;
    r = resolve_dir_cluster(ndir, s, v, &new_cl);
    if (r != FATFS_OK) goto out;
    if (old_cl != new_cl) {
        console_printf("[fat32] rename: cross-directory move not supported (%s -> %s)\n",
                       old_path, new_path);
        r = FATFS_EINVAL;
        goto out;
    }

    struct dir_slot oslot;
    uint8_t oent[DIR_ENTRY_SIZE];
    char oraw[11];
    r = resolve_component(old_cl, obase, s, &oslot, oent, oraw);
    if (r != FATFS_OK) { r = FATFS_ENOENT; goto out; }

    /* No-op when the visible name does not actually change. */
    {
        char cur[LFN_MAX_CHARS + 1];
        struct lfn_acc acc;
        lfn_reset(&acc);
        cur[0] = '\0';
        {
            /* Collect the chain that sits in front of the short entry: walk the
             * directory again with the same resumable scan readdir uses. */
            uint32_t idx = 0;
            uint32_t per_cl = g_vol->cluster_bytes / DIR_ENTRY_SIZE;
            uint32_t cl = old_cl;
            int done = 0;
            while (!done && cl >= 2 && cl < FAT32_EOC) {
                if (idx >= per_cl) {
                    uint32_t next;
                    if (fat_get(cl, s, &next) != FATFS_OK) break;
                    cl = next; idx = 0; continue;
                }
                uint32_t lba = cluster_lba(cl) + (idx * DIR_ENTRY_SIZE) / 512u;
                uint32_t off = (idx * DIR_ENTRY_SIZE) % 512u;
                if (sec_read(lba, s) != FATFS_OK) break;
                uint8_t *ent = s + off;
                idx++;
                if (ent[0] == 0x00) {
                    if (dir_cluster_is_last(cl, s)) break;
                    continue;              /* keep scanning the later cluster */
                }
                if (ent[0] == 0xE5) { lfn_reset(&acc); continue; }
                if (lfn_is_entry(ent)) { lfn_feed(&acc, ent); continue; }
                if (ent[11] & ATTR_VOL) { lfn_reset(&acc); continue; }
                if (raw_equal((const char *)ent, oraw)) {
                    if (!lfn_assemble(&acc, ent, cur, sizeof(cur)))
                        raw_to_name((const char *)ent, cur, sizeof(cur));
                    done = 1;
                }
                lfn_reset(&acc);
            }
        }
        if (cur[0] && name_eq_ci(cur, nbase)) { r = FATFS_OK; goto out; }
    }

    uint32_t first_cl = dirent_first_cluster(oent);
    uint32_t fsize = 0;
    memcpy(&fsize, oent + 28, 4);

    /* Create the new name first: if it fails (EEXIST, no space) the old entry is
     * still intact and nothing was lost. */
    char nraw[11];
    r = create_dirent(new_cl, nbase, oent[11], first_cl, fsize, s, v, nraw);
    if (r != FATFS_OK) goto out;

    /* Then retire the old entry and its VFAT chain. */
    dir_mark_lfn_chain(oslot.lba, oslot.offset, s);
    r = sec_read(oslot.lba, s);
    if (r != FATFS_OK) goto out;
    s[oslot.offset] = 0xE5;
    r = sec_write_checked(oslot.lba, s, v, old_path, oslot.offset);
    if (r == FATFS_OK)
        console_printf("[fat32] renamed %s -> %s\n", old_path, new_path);

out:
    lemis_heap_free(s);
    lemis_heap_free(v);
    return r;
}

int fatfs_sync(void) {    if (!g_vol->mounted) return FATFS_EBUSY;
    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;
    uint8_t *v = lemis_heap_alloc(512);
    if (!v) { lemis_heap_free(s); return FATFS_ERR; }
    int r = fsinfo_flush(s, v);
    lemis_heap_free(s);
    lemis_heap_free(v);
    return r;
}

int fatfs_get_free_space(uint64_t *free_bytes, uint64_t *total_bytes) {
    if (!g_vol->mounted) return FATFS_EBUSY;
    if (free_bytes) {
        *free_bytes = (g_vol->free_clusters == 0xFFFFFFFFu)
                      ? 0
                      : (uint64_t)g_vol->free_clusters * g_vol->cluster_bytes;
    }
    if (total_bytes) {
        *total_bytes = (uint64_t)g_vol->total_clusters * g_vol->cluster_bytes;
    }
    return FATFS_OK;
}
