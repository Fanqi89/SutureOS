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

/* ==================== volume state ==================== */

struct fat32_vol {
    int      drive;
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
    uint32_t first_cluster;
    uint32_t size;
    char     path[64];
};

struct fatfs_dir {
    int      used;
    uint32_t cluster;
    uint32_t entry;
    char     path[64];
};

static struct fat32_vol g_vol;
static struct fatfs_file g_files[MAX_FILES];
static struct fatfs_dir  g_dirs[MAX_DIRS];

/* ==================== sector I/O ==================== */

static int sec_read(uint32_t lba, uint8_t *buf) {
    if (blk_read(g_vol.drive, lba, 1, buf) != 0) return FATFS_EIO;
    return FATFS_OK;
}

static int sec_write_checked(uint32_t lba, const uint8_t *buf, uint8_t *vbuf,
                             const char *what, uint32_t at) {
    if (blk_write(g_vol.drive, lba, 1, buf) != 0) return FATFS_EIO;
    if (blk_read(g_vol.drive, lba, 1, vbuf) != 0) return FATFS_EIO;
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
    return g_vol.data_lba + (cl - 2) * g_vol.spc;
}

static int cluster_valid(uint32_t cl) {
    return cl >= 2 && cl < g_vol.total_clusters + 2;
}

/* ==================== FAT access ==================== */

static int fat_get(uint32_t cl, uint8_t *scratch, uint32_t *out) {
    if (!cluster_valid(cl)) return FATFS_EINVAL;
    uint32_t off = cl * 4;
    uint32_t lba = g_vol.fat_lba + off / 512;
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
    uint32_t lba1 = g_vol.fat_lba + rel;
    uint32_t lba2 = g_vol.fat_lba + g_vol.fat_sz + rel;
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
    if (g_vol.free_clusters == 0xFFFFFFFFu) return;
    if (old_val == FAT32_ENTRY_FREE && new_val != FAT32_ENTRY_FREE) {
        if (g_vol.free_clusters > 0) g_vol.free_clusters--;
    } else if (old_val != FAT32_ENTRY_FREE && new_val == FAT32_ENTRY_FREE) {
        g_vol.free_clusters++;
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
    g_vol.fsinfo_dirty = 1;
    return FATFS_OK;
}

/* Allocate n CONTIGUOUS free clusters. Links are written with the EOC
 * LAST so a failure leaves a reclaimable prefix chain. */
static int alloc_run(uint32_t n, uint8_t *s, uint8_t *v, uint32_t *out_first) {
    if (n == 0) { *out_first = FAT32_EOC; return FATFS_OK; }
    if (g_vol.free_clusters != 0xFFFFFFFFu && g_vol.free_clusters < n) {
        return FATFS_ENOSPC;
    }

    uint32_t run_start = 0, run_len = 0;
    for (uint32_t cl = 2; cl < g_vol.total_clusters + 2; cl++) {
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
        for (uint32_t k = 0; k < g_vol.spc; k++) {
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
    g_vol.fsinfo_dirty = 1;
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

/* ==================== directory entries ==================== */

struct dir_slot {
    uint32_t lba;
    uint32_t offset;
    int      valid;
};

static int dir_find(uint32_t dir_cl, const char raw[11], uint8_t *s,
                    struct dir_slot *out, uint8_t *out_ent) {
    uint32_t cl = dir_cl;
    while (cl >= 2 && cl < FAT32_EOC) {
        for (uint32_t k = 0; k < g_vol.spc; k++) {
            uint32_t lba = cluster_lba(cl) + k;
            int r = sec_read(lba, s);
            if (r != FATFS_OK) return r;
            for (uint32_t e = 0; e < ENTRIES_PER_SEC; e++) {
                uint8_t *ent = s + e * DIR_ENTRY_SIZE;
                if (ent[0] == 0x00) return FATFS_ENOENT;
                if (ent[0] == 0xE5) continue;
                if ((ent[11] & ATTR_LFN) == ATTR_LFN) continue;
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

static int dir_free_slot(uint32_t dir_cl, uint8_t *s, uint8_t *v,
                         struct dir_slot *out, uint8_t *out_ent) {
    uint32_t cl = dir_cl;
    uint32_t prev_cl = 0;
    while (cl >= 2 && cl < FAT32_EOC) {
        for (uint32_t k = 0; k < g_vol.spc; k++) {
            uint32_t lba = cluster_lba(cl) + k;
            int r = sec_read(lba, s);
            if (r != FATFS_OK) return r;
            for (uint32_t e = 0; e < ENTRIES_PER_SEC; e++) {
                uint8_t *ent = s + e * DIR_ENTRY_SIZE;
                if (ent[0] == 0xE5) {
                    out->lba = lba;
                    out->offset = e * DIR_ENTRY_SIZE;
                    out->valid = 1;
                    memset(out_ent, 0, DIR_ENTRY_SIZE);
                    out_ent[0] = 0xE5;
                    return FATFS_OK;
                }
                if (ent[0] == 0x00) {
                    memset(ent, 0, DIR_ENTRY_SIZE);
                    for (uint32_t z = e + 1; z < ENTRIES_PER_SEC; z++) {
                        memset(s + z * DIR_ENTRY_SIZE, 0, DIR_ENTRY_SIZE);
                    }
                    r = sec_write_checked(lba, s, v, "dir term", lba);
                    if (r != FATFS_OK) return r;
                    out->lba = lba;
                    out->offset = e * DIR_ENTRY_SIZE;
                    out->valid = 1;
                    memset(out_ent, 0, DIR_ENTRY_SIZE);
                    return FATFS_OK;
                }
            }
        }
        prev_cl = cl;
        uint32_t next;
        int r = fat_get(cl, s, &next);
        if (r != FATFS_OK) return r;
        if (next >= FAT32_EOC || next < 2) break;
        cl = next;
    }

    uint32_t new_cl;
    int r = alloc_run(1, s, v, &new_cl);
    if (r != FATFS_OK) return r;
    if (prev_cl == 0) return FATFS_EIO;
    r = fat_set_c(prev_cl, new_cl, s, v, "dir grow");
    if (r != FATFS_OK) {
        chain_free(new_cl, s, v);
        return r;
    }
    out->lba = cluster_lba(new_cl);
    out->offset = 0;
    out->valid = 1;
    memset(out_ent, 0, DIR_ENTRY_SIZE);
    return FATFS_OK;
}

static int path_resolve(const char *path, uint8_t *s, uint8_t *v,
                        uint32_t *out_parent, char out_raw[11]) {
    if (!path || path[0] != '/') return FATFS_EINVAL;
    uint32_t dir = g_vol.root_cluster;
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

        char raw[11];
        int r = make_raw(comp, raw);
        if (r != FATFS_OK) return r;

        if (*p == '\0') {
            *out_parent = dir;
            memcpy(out_raw, raw, 11);
            return FATFS_OK;
        }

        struct dir_slot slot;
        uint8_t ent[DIR_ENTRY_SIZE];
        r = dir_find(dir, raw, s, &slot, ent);
        if (r != FATFS_OK) return FATFS_ENOENT;
        if (!(ent[11] & ATTR_DIR)) return FATFS_ENOTDIR;
        dir = ((uint32_t)ent[20] << 16) | ent[26];
        if (dir == 0) dir = g_vol.root_cluster;
    }
    (void)v;
    return FATFS_EINVAL;
}

int fatfs_stat(const char *path, uint32_t *size, int *is_dir) {
    if (!g_vol.mounted) return FATFS_EBUSY;
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

int fatfs_mount(uint8_t drv, void *fs) {
    (void)fs;
    if (g_vol.mounted && g_vol.drive == (int)drv) return FATFS_OK;
    if (g_vol.mounted) return FATFS_EBUSY;

    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;
    if (blk_read((int)drv, 0, 1, s) != 0) {
        console_printf("[fat32] mount FAIL drive=%u reason=io\n", (uint64_t)drv);
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
        console_printf("[fat32] mount FAIL drive=%u reason=%s\n",
                       (uint64_t)drv, why);
        lemis_heap_free(s);
        return FATFS_EINVAL;
    }

    memset(&g_vol, 0, sizeof(g_vol));
    g_vol.drive = (int)drv;
    g_vol.fat_lba = reserved;
    g_vol.fat_sz = fatsz32;
    g_vol.data_lba = reserved + (uint32_t)nfats * fatsz32;
    g_vol.root_cluster = root_clus;
    g_vol.spc = spc;
    g_vol.cluster_bytes = spc * 512;
    g_vol.total_sectors = tot32;
    g_vol.total_clusters = (uint32_t)clusters;
    g_vol.fsinfo_lba = (fsinfo < tot32 && fsinfo != 0) ? fsinfo : 0;
    g_vol.mounted = 1;

    g_vol.free_clusters = 0xFFFFFFFFu;
    if (g_vol.fsinfo_lba) {
        if (sec_read(g_vol.fsinfo_lba, s) == FATFS_OK) {
            uint32_t m1, m2, trail, free_cnt;
            memcpy(&m1, s + 0, 4);
            memcpy(&m2, s + 484, 4);
            memcpy(&trail, s + 508, 4);
            memcpy(&free_cnt, s + 488, 4);
            if (m1 == 0x41615252u && m2 == 0x61417272u && trail == 0xAA5555AAu) {
                g_vol.free_clusters = free_cnt;
            }
        }
    }
    if (g_vol.free_clusters == 0xFFFFFFFFu) {
        uint32_t cnt = 0;
        for (uint32_t cl = 2; cl < g_vol.total_clusters + 2; cl++) {
            uint32_t val;
            if (fat_get(cl, s, &val) != FATFS_OK) { cnt = 0; break; }
            if (val == FAT32_ENTRY_FREE) cnt++;
        }
        g_vol.free_clusters = cnt;
    }

    console_printf("[fat32] mount drive=%u lba0=0x%x spc=%u clusters=%u fat_lba=%u fat_sz=%u root=%u free=%u\n",
                   (uint64_t)drv, (uint64_t)lba0, (uint64_t)spc,
                   (uint64_t)g_vol.total_clusters, (uint64_t)g_vol.fat_lba,
                   (uint64_t)g_vol.fat_sz, (uint64_t)g_vol.root_cluster,
                   (uint64_t)g_vol.free_clusters);
    lemis_heap_free(s);
    return FATFS_OK;
}

int fatfs_unmount(int drv) {
    if (!g_vol.mounted || g_vol.drive != drv) return FATFS_EBUSY;
    fatfs_sync();
    memset(&g_vol, 0, sizeof(g_vol));
    memset(g_files, 0, sizeof(g_files));
    memset(g_dirs, 0, sizeof(g_dirs));
    console_printf("[fat32] unmounted drive=%u\n", (uint64_t)drv);
    return FATFS_OK;
}

int fatfs_vfs_init(void) {
    memset(&g_vol, 0, sizeof(g_vol));
    memset(g_files, 0, sizeof(g_files));
    memset(g_dirs, 0, sizeof(g_dirs));
    return FATFS_OK;
}

/* ==================== FSInfo ==================== */

static int fsinfo_flush(uint8_t *s, uint8_t *v) {
    if (!g_vol.fsinfo_dirty || !g_vol.fsinfo_lba) return FATFS_OK;
    int r = sec_read(g_vol.fsinfo_lba, s);
    if (r != FATFS_OK) return r;
    memcpy(s + 488, &g_vol.free_clusters, 4);
    r = sec_write_checked(g_vol.fsinfo_lba, s, v, "fsinfo", 488);
    if (r == FATFS_OK) g_vol.fsinfo_dirty = 0;
    return r;
}

/* ==================== file operations ==================== */

int fatfs_open(const char *path, int create, fatfs_file_t **out) {
    if (!g_vol.mounted) return FATFS_EBUSY;
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

    uint32_t parent;
    char raw[11];
    int r = path_resolve(path, s, v, &parent, raw);
    if (r != FATFS_OK) goto out;
    if (parent == 0) { r = FATFS_EISDIR; goto out; }

    struct dir_slot slot;
    uint8_t ent[DIR_ENTRY_SIZE];
    r = dir_find(parent, raw, s, &slot, ent);
    if (r == FATFS_ENOENT && create) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        char craw[11];
        r = make_raw(base, craw);
        if (r != FATFS_OK) goto out;
        r = dir_free_slot(parent, s, v, &slot, ent);
        if (r != FATFS_OK) goto out;
        memset(ent, 0, DIR_ENTRY_SIZE);
        memcpy(ent, craw, 11);
        ent[11] = ATTR_RO;
        r = sec_read(slot.lba, s);
        if (r != FATFS_OK) goto out;
        memcpy(s + slot.offset, ent, DIR_ENTRY_SIZE);
        r = sec_write_checked(slot.lba, s, v, "create dirent", slot.offset);
        if (r != FATFS_OK) goto out;
    } else if (r != FATFS_OK) {
        goto out;
    } else if (ent[11] & ATTR_DIR) {
        r = FATFS_EISDIR;
        goto out;
    }

    fh->used = 1;
    fh->first_cluster = ((uint32_t)ent[20] << 16) | ent[26];
    if (fh->first_cluster == 0) fh->first_cluster = FAT32_EOC;
    memcpy(&fh->size, ent + 28, 4);
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
    if (!f || !f->used || !buf) return FATFS_EINVAL;
    if (!g_vol.mounted) return FATFS_EBUSY;
    if (off >= f->size) return 0;
    if (off + len > f->size) len = f->size - off;

    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;

    uint32_t cb = g_vol.cluster_bytes;
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
    if (!f || !f->used || !buf) return FATFS_EINVAL;
    if (!g_vol.mounted) return FATFS_EBUSY;

    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;
    uint8_t *v = lemis_heap_alloc(512);
    if (!v) { lemis_heap_free(s); return FATFS_ERR; }

    uint32_t end = off + len;
    uint32_t need_clusters = (end + g_vol.cluster_bytes - 1) / g_vol.cluster_bytes;
    if (need_clusters == 0) need_clusters = 1;

    uint32_t first = (f->first_cluster >= FAT32_EOC) ? 0 : f->first_cluster;
    int r = chain_ensure(&first, need_clusters, s, v);
    if (r != FATFS_OK) goto out;
    if (f->first_cluster >= FAT32_EOC) f->first_cluster = first;

    if (off > f->size) {
        uint32_t cb = g_vol.cluster_bytes;
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
        uint32_t cb = g_vol.cluster_bytes;
        uint32_t done = 0;
        uint32_t pos = off;
        const uint8_t *src = buf;
        while (done < len) {
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
    if (!f || !f->used) return FATFS_EINVAL;
    if (!g_vol.mounted) return FATFS_EBUSY;

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
        uint32_t cb = g_vol.cluster_bytes;
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
    if (!g_vol.mounted) return FATFS_EBUSY;
    if (!path || !out) return FATFS_EINVAL;

    struct fatfs_dir *d = NULL;
    for (int i = 0; i < MAX_DIRS; i++) {
        if (!g_dirs[i].used) { d = &g_dirs[i]; break; }
    }
    if (!d) return FATFS_ERR;

    if (strcmp(path, "/") == 0) {
        d->cluster = g_vol.root_cluster;
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
        d->cluster = ((uint32_t)ent[20] << 16) | ent[26];
        if (d->cluster == 0) d->cluster = g_vol.root_cluster;
    }

    d->used = 1;
    d->entry = 0;
    strncpy(d->path, path, sizeof(d->path) - 1);
    d->path[sizeof(d->path) - 1] = '\0';
    *out = d;
    return FATFS_OK;
}

int fatfs_readdir(fatfs_dir_t *d, char *name, uint32_t name_cap,
                  uint32_t *size, int *is_dir) {
    if (!d || !d->used || !name) return FATFS_EINVAL;
    if (!g_vol.mounted) return FATFS_EBUSY;

    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;

    int r = FATFS_ENOTFOUND;
    uint32_t idx = d->entry;
    uint32_t cl = d->cluster;
    uint32_t per_cl = g_vol.cluster_bytes / DIR_ENTRY_SIZE;

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

        if (ent[0] == 0x00) { r = FATFS_ENOTFOUND; goto out; }
        if (ent[0] == 0xE5) { r = FATFS_OK; continue; }
        if ((ent[11] & ATTR_LFN) == ATTR_LFN) { r = FATFS_OK; continue; }
        if (ent[11] & ATTR_VOL) { r = FATFS_OK; continue; }
        if (ent[0] == '.' && (ent[1] == ' ' || (ent[1] == '.' && ent[2] == ' '))) {
            r = FATFS_OK;
            continue;
        }

        raw_to_name((const char *)ent, name, name_cap);
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
    if (!d || !d->used) return FATFS_EINVAL;
    d->used = 0;
    return FATFS_OK;
}

int fatfs_mkdir(const char *path) {
    if (!g_vol.mounted) return FATFS_EBUSY;
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
    uint32_t p = (parent == g_vol.root_cluster) ? 0 : parent;
    dotdot[20] = (uint8_t)(p >> 16); dotdot[21] = (uint8_t)(p >> 24);
    dotdot[26] = (uint8_t)p; dotdot[27] = (uint8_t)(p >> 8);
    r = sec_write_checked(cluster_lba(cl), s, v, path, cluster_lba(cl));
    if (r != FATFS_OK) { chain_free(cl, s, v); goto out; }

    struct dir_slot slot;
    uint8_t ent[DIR_ENTRY_SIZE];
    r = dir_free_slot(parent, s, v, &slot, ent);
    if (r != FATFS_OK) { chain_free(cl, s, v); goto out; }
    memset(ent, 0, DIR_ENTRY_SIZE);
    memcpy(ent, raw, 11);
    ent[11] = ATTR_DIR;
    ent[20] = (uint8_t)(cl >> 16); ent[21] = (uint8_t)(cl >> 24);
    ent[26] = (uint8_t)cl; ent[27] = (uint8_t)(cl >> 8);
    r = sec_read(slot.lba, s);
    if (r == FATFS_OK) {
        memcpy(s + slot.offset, ent, DIR_ENTRY_SIZE);
        r = sec_write_checked(slot.lba, s, v, path, slot.offset);
    }

out:
    lemis_heap_free(s);
    lemis_heap_free(v);
    return r;
}

int fatfs_rmdir(const char *path) {
    if (!g_vol.mounted) return FATFS_EBUSY;
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
    uint32_t cl = ((uint32_t)ent[20] << 16) | ent[26];

    {
        uint32_t idx = 0;
        uint32_t per_cl = g_vol.cluster_bytes / DIR_ENTRY_SIZE;
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
            if (e2[0] == 0x00) break;
            if (e2[0] == 0xE5) continue;
            if ((e2[11] & ATTR_LFN) == ATTR_LFN) continue;
            if (e2[0] == '.' && (e2[1] == ' ' || (e2[1] == '.' && e2[2] == ' '))) continue;
            r = FATFS_ENOTEMPTY;
            break;
        }
        if (r != FATFS_OK) goto out;
    }

    r = chain_free(cl, s, v);
    if (r != FATFS_OK) goto out;
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
    if (!g_vol.mounted) return FATFS_EBUSY;
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

    uint32_t cl = ((uint32_t)ent[20] << 16) | ent[26];
    if (cl >= 2) {
        r = chain_free(cl, s, v);
        if (r != FATFS_OK) goto out;
    }
    r = sec_read(slot.lba, s);
    if (r != FATFS_OK) goto out;
    s[slot.offset] = 0xE5;
    r = sec_write_checked(slot.lba, s, v, path, slot.offset);

out:
    lemis_heap_free(s);
    lemis_heap_free(v);
    return r;
}

/* Rename an entry inside one directory (8.3 names, no LFN).
 *
 * A FAT rename is a name-field rewrite: attributes, timestamps, first cluster
 * and size all stay where they are, so the cluster chain (and therefore the
 * data) is untouched. This implementation is honest about its scope:
 *   - same directory only; a cross-directory move would need a fresh dirent in
 *     the destination plus the source's first-cluster/size copied over and is
 *     refused with a log instead of being faked;
 *   - the destination must not already exist (FATFS_EEXIST);
 *   - renaming to the same 8.3 name (case-insensitive) is a no-op success.
 * The rewritten sector is read back and compared byte-for-byte before the call
 * is considered successful, like every other write in this driver. */
int fatfs_rename(const char *old_path, const char *new_path) {
    if (!g_vol.mounted) return FATFS_EBUSY;
    if (!old_path || !new_path || old_path[0] != '/' || new_path[0] != '/')
        return FATFS_EINVAL;

    uint8_t *s = lemis_heap_alloc(512);
    if (!s) return FATFS_ERR;
    uint8_t *v = lemis_heap_alloc(512);
    if (!v) { lemis_heap_free(s); return FATFS_ERR; }

    uint32_t old_parent = 0, new_parent = 0;
    char old_raw[11], new_raw[11];
    struct dir_slot oslot;
    uint8_t oent[DIR_ENTRY_SIZE];
    int r = path_resolve(old_path, s, v, &old_parent, old_raw);
    if (r != FATFS_OK) goto out;
    r = path_resolve(new_path, s, v, &new_parent, new_raw);
    if (r != FATFS_OK) goto out;

    if (old_raw[0] == '\0' || new_raw[0] == '\0') {   /* "/" is not renameable */
        r = FATFS_EINVAL;
        goto out;
    }
    if (old_parent != new_parent) {
        console_printf("[fat32] rename: cross-directory move not supported (%s -> %s)\n",
                       old_path, new_path);
        r = FATFS_EINVAL;
        goto out;
    }

    r = dir_find(old_parent, old_raw, s, &oslot, oent);
    if (r != FATFS_OK) {
        r = FATFS_ENOENT;
        goto out;
    }
    if (raw_equal(old_raw, new_raw)) {                /* already that name */
        r = FATFS_OK;
        goto out;
    }
    {
        struct dir_slot nslot;
        uint8_t nent[DIR_ENTRY_SIZE];
        if (dir_find(new_parent, new_raw, s, &nslot, nent) == FATFS_OK) {
            console_printf("[fat32] rename: target '%s' already exists\n", new_path);
            r = FATFS_EEXIST;
            goto out;
        }
    }

    r = sec_read(oslot.lba, s);
    if (r != FATFS_OK) goto out;
    memcpy(s + oslot.offset, new_raw, 11);
    r = sec_write_checked(oslot.lba, s, v, new_path, oslot.offset);
    if (r == FATFS_OK)
        console_printf("[fat32] renamed %s -> %s\n", old_path, new_path);

out:
    lemis_heap_free(s);
    lemis_heap_free(v);
    return r;
}

int fatfs_sync(void) {    if (!g_vol.mounted) return FATFS_EBUSY;
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
    if (!g_vol.mounted) return FATFS_EBUSY;
    if (free_bytes) {
        *free_bytes = (g_vol.free_clusters == 0xFFFFFFFFu)
                      ? 0
                      : (uint64_t)g_vol.free_clusters * g_vol.cluster_bytes;
    }
    if (total_bytes) {
        *total_bytes = (uint64_t)g_vol.total_clusters * g_vol.cluster_bytes;
    }
    return FATFS_OK;
}
