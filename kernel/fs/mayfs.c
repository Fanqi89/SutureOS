/* SutureOS: MayFS core - snapshot table, dual-slot commit, path layer.
 *
 * PROVENANCE / LICENCE
 *   The on-disk format and the algorithms here are MayOS' MayFS, read from
 *   MayOS_0.3.0-pre1/kernel/fs.c + kernel/core.h.  The MayOS repository root
 *   LICENSE was read while porting: **MIT License, Copyright (c) 2024 MayOS
 *   Project** (the porting list had assumed GPL-3.0; the real licence is MIT,
 *   which is compatible with this GPL-3.0-or-later tree).  The code below is
 *   written from scratch for SutureOS - data structures and the checksum /
 *   generation semantics follow the original, the implementation does not.
 *   Byte-level spec: docs/mayfs-format-notes.md.
 *
 * MAYFS IN ONE PARAGRAPH
 *   512-sector (256 KiB) snapshots at LBA 0 (primary) and LBA 512 (backup).
 *   A snapshot is: 8-byte magic, FNV-1a checksum of the record table, a
 *   "MAY2" generation tag with a complement copy, then the flat table of
 *   48 records (4 x u32, char name[48], u8 data[4096]).  A 32-byte prefs
 *   record lives at offset 500*512.  Mount validates BOTH slots and uses the
 *   newer valid generation; commit rewrites the backup slot first and the
 *   primary slot second, each after invalidating the slot header, so a power
 *   cut can only ever leave one torn (unreadable) slot next to a valid one.
 *
 * This file is the kernel core.  kernel/fs/mayfs_vfs.c adapts it to
 * vfs_fs_driver_t ("mayfs").
 */
#include "mayfs.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "block.h"

/* The in-memory record must be byte-identical to the on-disk record; the
 * whole table is memcpy'd both ways (MayOS does the same with File files[FS_MAX]). */
_Static_assert(sizeof(struct mayfs_entry) == MAYFS_ENTRY_BYTES,
               "mayfs_entry must be exactly 4160 bytes");

/* ==================== state ==================== */

static struct mayfs_entry g_entries[MAYFS_MAX_FILES];
/* Snapshot scratch: slot image and the "other" slot while mounting. */
static uint8_t g_snapshot[MAYFS_SLOT_BYTES];
static uint8_t g_other[MAYFS_SLOT_BYTES];

static uint8_t g_prefs[MAYFS_PREFS_LENGTH];
static int g_prefs_ready;

static int g_mounted;
static int g_drive = -1;
static uint32_t g_start_lba;
static uint32_t g_generation;
static int g_active_slot;      /* 0 = primary is newest, 1 = backup is newest */
static int g_dirty;

static struct mayfs_file g_files[MAYFS_MAX_OPEN_FILES];
static struct mayfs_dir g_dirs[MAYFS_MAX_OPEN_DIRS];

/* ==================== little-endian helpers ==================== */

static uint32_t mayfs_rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void mayfs_wr32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

/* ==================== checksum (MayFS uses FNV-1a 32) ==================== */

uint32_t mayfs_checksum(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ (uint32_t)(*p++)) * 16777619u;
    return h;
}

/* ==================== snapshot validation (mirrors MayOS fs.c) ==================== */

static int mayfs_snapshot_blank(const uint8_t *img) {
    for (uint32_t i = 0; i < MAYFS_SLOT_BYTES; i++)
        if (img[i]) return 0;
    return 1;
}

static int mayfs_snapshot_owned(const uint8_t *img) {
    for (int i = 0; i < 8; i++)
        if (img[i] != (uint8_t)MAYFS_MAGIC[i]) return 0;
    return 1;
}

/* Legacy snapshots carry no MAY2 tag and count as generation zero. */
static uint32_t mayfs_snapshot_generation(const uint8_t *img) {
    if (img[12] != 'M' || img[13] != 'A' || img[14] != 'Y' || img[15] != '2') return 0;
    {
        uint32_t value = mayfs_rd32(img + 16);
        return (mayfs_rd32(img + 20) == ~value) ? value : 0;
    }
}

/* Full structural validation: magic, table checksum, per-record sanity and the
 * directory graph (every parent chain terminates at the root record 0). */
static int mayfs_snapshot_valid(const uint8_t *img) {
    const struct mayfs_entry *records = (const struct mayfs_entry *)(img + MAYFS_TABLE_OFFSET);
    int i;

    if (!mayfs_snapshot_owned(img)) return 0;
    if (mayfs_rd32(img + 8) != mayfs_checksum(img + MAYFS_TABLE_OFFSET, MAYFS_TABLE_BYTES))
        return 0;

    for (i = 0; i < (int)MAYFS_MAX_FILES; i++) {
        const struct mayfs_entry *f = &records[i];
        if (!f->used) continue;
        if (f->used != 1u || f->dir > 1u || f->parent >= MAYFS_MAX_FILES ||
            f->size >= MAYFS_FILE_CAP || f->name[MAYFS_NAME_BYTES - 1] ||
            f->data[f->size])
            return 0;
    }
    /* MayFS always keeps root record 0 plus three top-level directories in
     * slots 1..3; an image without them is not a MayFS volume. */
    for (i = 0; i < (int)MAYFS_FIXED_DIRS; i++) {
        if (!records[i].used || !records[i].dir || records[i].parent) return 0;
    }
    for (i = 1; i < (int)MAYFS_MAX_FILES; i++) {
        uint32_t p, depth = 0;
        if (!records[i].used) continue;
        p = records[i].parent;
        while (p) {
            if (p >= MAYFS_MAX_FILES || !records[p].used || !records[p].dir) return 0;
            if (++depth >= MAYFS_MAX_FILES) return 0;
            p = records[p].parent;
        }
    }
    return 1;
}

static int mayfs_snapshot_has_prefs(const uint8_t *img) {
    const uint8_t *rec = img + MAYFS_PREFS_OFFSET;
    for (int i = 0; i < 8; i++)
        if (rec[i] != (uint8_t)MAYFS_PREFS_MAGIC[i]) return 0;
    return mayfs_rd32(rec + 8) == mayfs_checksum(rec + 12, MAYFS_PREFS_LENGTH);
}

/* ==================== block I/O ==================== */

static int mayfs_read_slot(uint32_t slot, uint8_t *dst) {
    uint32_t base = g_start_lba + (slot ? MAYFS_BACKUP_LBA : MAYFS_PRIMARY_LBA);
    return blk_read(g_drive, base, MAYFS_SLOT_SECTORS, dst) == 0 ? MAYFS_OK : MAYFS_EIO;
}

/* Write one slot in MayOS' torn-write-safe order: invalidate the header first,
 * stream the payload, then publish the new header.  A power cut anywhere after
 * step 1 leaves this slot readable-but-invalid (magic zeroed) while the other
 * slot still holds a complete generation. */
static int mayfs_write_slot(uint32_t slot) {
    static const uint8_t zero[MAYFS_SECTOR_SIZE];
    uint32_t base = g_start_lba + (slot ? MAYFS_BACKUP_LBA : MAYFS_PRIMARY_LBA);

    if (blk_write(g_drive, base, 1, zero) != 0) return MAYFS_EIO;
    if (blk_write(g_drive, base + 1, MAYFS_SLOT_SECTORS - 1,
                  g_snapshot + MAYFS_SECTOR_SIZE) != 0)
        return MAYFS_EIO;
    if (blk_write(g_drive, base, 1, g_snapshot) != 0) return MAYFS_EIO;
    (void)blk_flush(g_drive);
    return MAYFS_OK;
}

/* Build the next snapshot image in g_snapshot (header, table, generation,
 * prefs) without writing it. */
static void mayfs_build_snapshot(uint32_t generation) {
    memset(g_snapshot, 0, MAYFS_SLOT_BYTES);
    memcpy(g_snapshot, MAYFS_MAGIC, 8);
    memcpy(g_snapshot + MAYFS_TABLE_OFFSET, g_entries, MAYFS_TABLE_BYTES);
    mayfs_wr32(g_snapshot + 8,
               mayfs_checksum(g_snapshot + MAYFS_TABLE_OFFSET, MAYFS_TABLE_BYTES));
    memcpy(g_snapshot + 12, MAYFS_GEN_TAG, 4);
    mayfs_wr32(g_snapshot + 16, generation);
    mayfs_wr32(g_snapshot + 20, ~generation);
    if (g_prefs_ready) {
        memcpy(g_snapshot + MAYFS_PREFS_OFFSET, MAYFS_PREFS_MAGIC, 8);
        memcpy(g_snapshot + MAYFS_PREFS_OFFSET + 12, g_prefs, MAYFS_PREFS_LENGTH);
        mayfs_wr32(g_snapshot + MAYFS_PREFS_OFFSET + 8,
                   mayfs_checksum(g_prefs, MAYFS_PREFS_LENGTH));
    }
}

/* ==================== commit ==================== */

int mayfs_commit(void) {
    uint32_t gen;

    if (!g_mounted) return MAYFS_EBUSY;
    gen = g_generation + 1;
    mayfs_build_snapshot(gen);

    /* Backup slot first, primary second: the primary keeps the previous valid
     * generation for the whole time the backup is being rewritten, and the
     * backup keeps the new one while the primary is rewritten. */
    if (mayfs_write_slot(1) != MAYFS_OK) {
        console_printf("[mayfs] commit FAILED writing the backup slot (drive %d)\n",
                       (int64_t)g_drive);
        return MAYFS_EIO;
    }
    if (mayfs_write_slot(0) != MAYFS_OK) {
        console_printf("[mayfs] commit FAILED writing the primary slot (drive %d); "
                       "backup slot holds generation %u\n",
                       (int64_t)g_drive, (uint64_t)gen);
        g_generation = gen;
        g_active_slot = 1;
        return MAYFS_EIO;
    }
    g_generation = gen;
    g_active_slot = 0;
    g_dirty = 0;
    console_printf("[mayfs] committed generation %u (backup slot first, "
                   "then primary; both flushed)\n", (uint64_t)gen);
    return MAYFS_OK;
}

/* ==================== mount / unmount ==================== */

/* Fresh-volume tree: root record 0 plus the three mandatory top-level dirs in
 * records 1..3, exactly like MayOS fs_init() seeds a brand new disk. */
static void mayfs_fresh_tree(void) {
    memset(g_entries, 0, sizeof(g_entries));
    g_entries[0].used = 1;
    g_entries[0].dir = 1;
    g_entries[0].parent = 0;
    strncpy(g_entries[0].name, "disk1", MAYFS_NAME_BYTES - 1);
    g_entries[1].used = 1;
    g_entries[1].dir = 1;
    g_entries[1].parent = 0;
    strncpy(g_entries[1].name, "Documents", MAYFS_NAME_BYTES - 1);
    g_entries[2].used = 1;
    g_entries[2].dir = 1;
    g_entries[2].parent = 0;
    strncpy(g_entries[2].name, "Desktop", MAYFS_NAME_BYTES - 1);
    g_entries[3].used = 1;
    g_entries[3].dir = 1;
    g_entries[3].parent = 0;
    strncpy(g_entries[3].name, "Trash", MAYFS_NAME_BYTES - 1);
    g_dirty = 1;
}

static int mayfs_used_count_i(const struct mayfs_entry *t) {
    int n = 0;
    for (int i = 0; i < (int)MAYFS_MAX_FILES; i++)
        if (t[i].used) n++;
    return n;
}

int mayfs_used_count(void) {
    return g_mounted ? mayfs_used_count_i(g_entries) : 0;
}

int mayfs_mount(int drive, uint32_t start_lba) {
    int main_valid, backup_valid, main_blank, backup_blank;
    const uint8_t *chosen;
    uint32_t main_gen = 0, backup_gen = 0;

    if (g_mounted) return MAYFS_EBUSY;
    if (drive < 0 || !blk_find_by_drive(drive)) {
        console_printf("[mayfs] mount: no block device for drive %d\n", (int64_t)drive);
        return MAYFS_ENOENT;
    }
    if (start_lba > 0x0FFFFFFFu) return MAYFS_EINVAL;

    g_drive = drive;
    g_start_lba = start_lba;
    g_prefs_ready = 0;
    memset(g_prefs, 0, sizeof(g_prefs));
    memset(g_entries, 0, sizeof(g_entries));

    if (mayfs_read_slot(0, g_snapshot) != MAYFS_OK ||
        mayfs_read_slot(1, g_other) != MAYFS_OK) {
        console_printf("[mayfs] mount FAILED drive=%d: I/O error reading the "
                       "primary/backup slots\n", (int64_t)drive);
        g_drive = -1;
        return MAYFS_EIO;
    }
    main_valid = mayfs_snapshot_valid(g_snapshot);
    backup_valid = mayfs_snapshot_valid(g_other);
    main_blank = mayfs_snapshot_blank(g_snapshot);
    backup_blank = mayfs_snapshot_blank(g_other);

    if (!main_valid && !backup_valid) {
        if (main_blank && backup_blank) {
            /* Brand new disk: create the default tree and commit it (backup
             * slot first), which is what MayOS fs_init() does too. */
            mayfs_fresh_tree();
            g_mounted = 1;
            g_generation = 0;
            g_active_slot = 1;
            console_printf("[mayfs] blank disk on drive %d: initialised a fresh "
                           "MayFS volume (reserved records %u)\n",
                           (int64_t)drive, (uint64_t)MAYFS_FIXED_DIRS);
            if (mayfs_commit() != MAYFS_OK) {
                g_mounted = 0;
                g_drive = -1;
                return MAYFS_EIO;
            }
            return MAYFS_OK;
        }
        /* Never reformat or "repair" an unknown disk: keep it untouched. */
        console_printf("[mayfs] mount FAILED drive=%d: no valid MayFS snapshot "
                       "(magic=%s primary=%s backup=%s); disk left untouched\n",
                       (int64_t)drive,
                       mayfs_snapshot_owned(g_snapshot) ? "yes" : "no",
                       main_valid ? "valid" : "invalid/blank",
                       backup_valid ? "valid" : "invalid/blank");
        g_drive = -1;
        return MAYFS_EIO;
    }

    if (main_valid) main_gen = mayfs_snapshot_generation(g_snapshot);
    if (backup_valid) backup_gen = mayfs_snapshot_generation(g_other);

    /* Newer generation wins; the unsigned wrap-safe comparison is MayOS'. */
    if (backup_valid && (!main_valid || (int32_t)(backup_gen - main_gen) > 0)) {
        chosen = g_other;
        g_active_slot = 1;
        g_generation = backup_gen;
        if (main_valid)
            console_printf("[mayfs] recovered from the backup slot "
                           "(backup gen %u newer than primary gen %u)\n",
                           (uint64_t)backup_gen, (uint64_t)main_gen);
    } else {
        chosen = g_snapshot;
        g_active_slot = 0;
        g_generation = main_gen;
        if (backup_valid && backup_gen != main_gen)
            console_printf("[mayfs] primary slot is newest (primary gen %u, "
                           "backup gen %u)\n", (uint64_t)main_gen,
                           (uint64_t)backup_gen);
    }

    memcpy(g_entries, chosen + MAYFS_TABLE_OFFSET, MAYFS_TABLE_BYTES);
    if (mayfs_snapshot_has_prefs(chosen)) {
        memcpy(g_prefs, chosen + MAYFS_PREFS_OFFSET + 12, MAYFS_PREFS_LENGTH);
        g_prefs_ready = 1;
    }
    g_mounted = 1;
    g_dirty = 0;
    console_printf("[mayfs] mount drive=%d generation=%u slot=%s records=%u "
                   "used=%u name-max=%u file-max=%u prefs=%s\n",
                   (int64_t)drive, (uint64_t)g_generation,
                   g_active_slot ? "backup" : "main",
                   (uint64_t)MAYFS_MAX_FILES,
                   (uint64_t)mayfs_used_count_i(g_entries),
                   (uint64_t)MAYFS_NAME_MAX, (uint64_t)MAYFS_FILE_MAX,
                   g_prefs_ready ? "yes" : "no");
    return MAYFS_OK;
}

int mayfs_unmount(void) {
    int r = MAYFS_OK;
    if (!g_mounted) return MAYFS_EBUSY;
    if (g_dirty) r = mayfs_commit();
    memset(g_entries, 0, sizeof(g_entries));
    memset(g_files, 0, sizeof(g_files));
    memset(g_dirs, 0, sizeof(g_dirs));
    g_mounted = 0;
    g_dirty = 0;
    g_drive = -1;
    g_generation = 0;
    g_active_slot = 0;
    g_prefs_ready = 0;
    return r;
}

int mayfs_is_mounted(void) { return g_mounted; }
int mayfs_drive(void) { return g_drive; }
uint32_t mayfs_generation(void) { return g_generation; }
int mayfs_active_slot(void) { return g_active_slot; }

/* ==================== read-only boot probe ==================== */

int mayfs_boot_probe(int drive) {
    uint8_t header[MAYFS_SECTOR_SIZE];
    int main_valid, backup_valid;
    uint32_t main_gen = 0, backup_gen = 0;
    int slot = 0;                       /* 0 = primary, 1 = backup */
    int save_drive = g_drive;
    uint32_t save_lba = g_start_lba;
    const char *slot_name = "main";

    if (drive < 0 || !blk_find_by_drive(drive)) {
        console_printf("[mayfs] no volume on drive %d (no such block device)\n",
                       (int64_t)drive);
        return MAYFS_ENOENT;
    }
    /* Cheap first look: one sector.  Only a snapshot that claims to be MayFS
     * pays for the full 256 KiB read. */
    if (blk_read(drive, MAYFS_PRIMARY_LBA, 1, header) != 0) {
        console_printf("[mayfs] no volume on drive %d (primary slot unreadable)\n",
                       (int64_t)drive);
        return MAYFS_EIO;
    }
    {
        int owned = 1;
        for (int i = 0; i < 8; i++)
            if (header[i] != (uint8_t)MAYFS_MAGIC[i]) owned = 0;
        if (!owned) {
            console_printf("[mayfs] no volume on drive %d (no MAYFS001 magic)\n",
                           (int64_t)drive);
            return MAYFS_ENOENT;
        }
    }
    /* The probe reuses the snapshot scratch buffers, so park the live volume's
     * drive/LBA and restore them afterwards (a mounted volume must not care). */
    g_drive = drive;
    g_start_lba = 0;
    if (mayfs_read_slot(0, g_snapshot) != MAYFS_OK) {
        g_drive = save_drive;
        g_start_lba = save_lba;
        console_printf("[mayfs] no volume on drive %d (primary slot unreadable)\n",
                       (int64_t)drive);
        return MAYFS_EIO;
    }
    main_valid = mayfs_snapshot_valid(g_snapshot);
    backup_valid = 0;
    if (mayfs_read_slot(1, g_other) == MAYFS_OK) backup_valid = mayfs_snapshot_valid(g_other);
    g_drive = save_drive;
    g_start_lba = save_lba;

    if (!main_valid && !backup_valid) {
        console_printf("[mayfs] no volume on drive %d (MAYFS001 magic present but "
                       "both snapshots fail the checksum/table validation)\n",
                       (int64_t)drive);
        return MAYFS_EIO;
    }
    if (main_valid) main_gen = mayfs_snapshot_generation(g_snapshot);
    if (backup_valid) backup_gen = mayfs_snapshot_generation(g_other);
    if (backup_valid && (!main_valid || (int32_t)(backup_gen - main_gen) > 0)) {
        slot = 1;
        slot_name = "backup";
    }

    console_printf("[mayfs] volume ready on drive %d: generation=%u slot=%s "
                   "primary-gen=%u backup-gen=%u records=%u name-max=%u file-max=%u "
                   "(read-only probe; mount with \"mount mayfs %d /mnt/may\")\n",
                   (int64_t)drive,
                   (uint64_t)(slot ? backup_gen : main_gen), slot_name,
                   (uint64_t)main_gen, (uint64_t)backup_gen,
                   (uint64_t)MAYFS_MAX_FILES, (uint64_t)MAYFS_NAME_MAX,
                   (uint64_t)MAYFS_FILE_MAX, (int64_t)drive);
    return MAYFS_OK;
}

/* ==================== path layer ==================== */

/* Copy one path component into out; returns the component length (0 = the
 * path ended) and advances *pp past the component and its separator. */
static uint32_t mayfs_next_component(const char **pp, char *out, uint32_t cap) {
    const char *p = *pp;
    uint32_t n = 0;
    while (*p == '/') p++;
    while (*p && *p != '/') {
        if (n + 1 < cap) out[n] = *p;
        n++;
        p++;
    }
    while (*p == '/') p++;
    *pp = p;
    if (n + 1 < cap) out[n] = '\0';
    else if (cap) out[cap - 1] = '\0';
    return n;
}

int mayfs_resolve(const char *path, int *out_index) {
    char comp[MAYFS_NAME_BYTES];
    const char *p = path;
    uint32_t n;
    int cur = MAYFS_ROOT_INDEX;

    if (!g_mounted) return MAYFS_EBUSY;
    if (!path || !out_index) return MAYFS_EINVAL;
    if (path[0] != '/') return MAYFS_EINVAL;

    while (1) {
        int found = -1;
        n = mayfs_next_component(&p, comp, sizeof(comp));
        if (n == 0) break;
        if (n > MAYFS_NAME_MAX) return MAYFS_ENOENT;
        if (!g_entries[cur].used || !g_entries[cur].dir) return MAYFS_ENOTDIR;
        for (int i = 1; i < (int)MAYFS_MAX_FILES; i++) {
            if (!g_entries[i].used) continue;
            if (g_entries[i].parent != (uint32_t)cur) continue;
            if (strcmp(g_entries[i].name, comp) == 0) { found = i; break; }
        }
        if (found < 0) return MAYFS_ENOENT;
        cur = found;
    }
    *out_index = cur;
    return MAYFS_OK;
}

int mayfs_stat(const char *path, struct mayfs_stat *st) {
    int idx, r;
    if (!st) return MAYFS_EINVAL;
    r = mayfs_resolve(path, &idx);
    if (r != MAYFS_OK) return r;
    st->index = idx;
    st->is_dir = g_entries[idx].dir;
    st->size = g_entries[idx].size;
    return MAYFS_OK;
}

/* Split "/a/b/name" into "/a/b" and "name". */
static int mayfs_split(const char *path, char *parent, uint32_t parent_cap,
                       char *name, uint32_t name_cap) {
    const char *slash;
    uint32_t plen, nlen;

    if (!path || path[0] != '/') return MAYFS_EINVAL;
    slash = strrchr(path, '/');
    if (!slash) return MAYFS_EINVAL;
    nlen = (uint32_t)strlen(slash + 1);
    if (nlen == 0 || nlen >= name_cap) return MAYFS_EINVAL;
    plen = (uint32_t)(slash - path);
    if (plen == 0) {
        if (parent_cap < 2) return MAYFS_EINVAL;
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        if (plen >= parent_cap) return MAYFS_EINVAL;
        memcpy(parent, path, plen);
        parent[plen] = '\0';
    }
    memcpy(name, slash + 1, nlen);
    name[nlen] = '\0';
    return MAYFS_OK;
}

static int mayfs_name_ok(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n > MAYFS_NAME_MAX) return 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '/' || c < 0x20) return 0;
    }
    return 1;
}

static int mayfs_find_child(int parent, const char *name) {
    for (int i = 1; i < (int)MAYFS_MAX_FILES; i++) {
        if (!g_entries[i].used) continue;
        if (g_entries[i].parent != (uint32_t)parent) continue;
        if (strcmp(g_entries[i].name, name) == 0) return i;
    }
    return -1;
}

/* Create a file or directory at path.  Records 0..3 are the format's fixed
 * root directories and are never handed out again. */
int mayfs_create(const char *path, int dir) {
    char parent_path[MAYFS_PATH_MAX];
    char name[MAYFS_NAME_BYTES];
    int pidx, r, free_idx = -1;

    if (!g_mounted) return MAYFS_EBUSY;
    r = mayfs_split(path, parent_path, sizeof(parent_path), name, sizeof(name));
    if (r != MAYFS_OK) return r;
    if (!mayfs_name_ok(name)) return MAYFS_EINVAL;
    r = mayfs_resolve(parent_path, &pidx);
    if (r != MAYFS_OK) return r;
    if (!g_entries[pidx].dir) return MAYFS_ENOTDIR;
    if (mayfs_find_child(pidx, name) >= 0) return MAYFS_EEXIST;

    for (int i = (int)MAYFS_FIXED_DIRS; i < (int)MAYFS_MAX_FILES; i++)
        if (!g_entries[i].used) { free_idx = i; break; }
    if (free_idx < 0) return MAYFS_ENOSPC;   /* 48-record table is full */

    memset(&g_entries[free_idx], 0, sizeof(g_entries[free_idx]));
    g_entries[free_idx].used = 1;
    g_entries[free_idx].dir = dir ? 1u : 0u;
    g_entries[free_idx].parent = (uint32_t)pidx;
    g_entries[free_idx].size = 0;
    strncpy(g_entries[free_idx].name, name, MAYFS_NAME_BYTES - 1);
    g_entries[free_idx].name[MAYFS_NAME_BYTES - 1] = '\0';
    g_dirty = 1;
    return free_idx;
}

/* MayOS fs_remove(): recursively drop every child, then the record itself. */
static void mayfs_remove_recursive(int idx) {
    for (int i = 1; i < (int)MAYFS_MAX_FILES; i++) {
        if (!g_entries[i].used) continue;
        if (g_entries[i].parent != (uint32_t)idx) continue;
        mayfs_remove_recursive(i);
    }
    memset(&g_entries[idx], 0, sizeof(g_entries[idx]));
}

static int mayfs_has_children(int idx) {
    for (int i = 1; i < (int)MAYFS_MAX_FILES; i++)
        if (g_entries[i].used && g_entries[i].parent == (uint32_t)idx) return 1;
    return 0;
}

int mayfs_remove(const char *path) {
    int idx, r;

    if (!g_mounted) return MAYFS_EBUSY;
    r = mayfs_resolve(path, &idx);
    if (r != MAYFS_OK) return r;
    if (idx < (int)MAYFS_FIXED_DIRS) return MAYFS_EINVAL;   /* root records */
    if (g_entries[idx].dir) {
        if (mayfs_has_children(idx)) return MAYFS_ENOTEMPTY;
        memset(&g_entries[idx], 0, sizeof(g_entries[idx]));
    } else {
        mayfs_remove_recursive(idx);
    }
    g_dirty = 1;
    return MAYFS_OK;
}

int mayfs_rename(const char *oldpath, const char *newpath) {
    char parent_path[MAYFS_PATH_MAX];
    char name[MAYFS_NAME_BYTES];
    int idx, pidx, r, clash;

    if (!g_mounted) return MAYFS_EBUSY;
    r = mayfs_resolve(oldpath, &idx);
    if (r != MAYFS_OK) return r;
    if (idx < (int)MAYFS_FIXED_DIRS) return MAYFS_EINVAL;
    r = mayfs_split(newpath, parent_path, sizeof(parent_path), name, sizeof(name));
    if (r != MAYFS_OK) return r;
    if (!mayfs_name_ok(name)) return MAYFS_EINVAL;
    r = mayfs_resolve(parent_path, &pidx);
    if (r != MAYFS_OK) return r;
    if (!g_entries[pidx].dir) return MAYFS_ENOTDIR;
    clash = mayfs_find_child(pidx, name);
    if (clash >= 0 && clash != idx) return MAYFS_EEXIST;
    if (g_entries[idx].dir) {
        /* Refuse to move a directory into its own subtree. */
        for (uint32_t a = (uint32_t)pidx; a;) {
            if ((int)a == idx) return MAYFS_EINVAL;
            a = g_entries[a].parent;
        }
    }
    g_entries[idx].parent = (uint32_t)pidx;
    strncpy(g_entries[idx].name, name, MAYFS_NAME_BYTES - 1);
    g_entries[idx].name[MAYFS_NAME_BYTES - 1] = '\0';
    g_dirty = 1;
    return MAYFS_OK;
}

int mayfs_count_free(uint32_t *free_records, uint32_t *used_records) {
    uint32_t used = 0;
    if (!g_mounted) return MAYFS_EBUSY;
    used = (uint32_t)mayfs_used_count_i(g_entries);
    if (free_records) *free_records = MAYFS_MAX_FILES - used;
    if (used_records) *used_records = used;
    return MAYFS_OK;
}

static int mayfs_is_dirty(void) { return g_dirty; }

/* ==================== file handles ==================== */

int mayfs_file_open(const char *path, int create, int trunc, struct mayfs_file **out) {
    struct mayfs_file *fh;
    int idx, r;

    if (!g_mounted) return MAYFS_EBUSY;
    if (!out) return MAYFS_EINVAL;
    r = mayfs_resolve(path, &idx);
    if (r == MAYFS_ENOENT && create) {
        r = mayfs_create(path, 0);
        if (r < 0) return r;
        idx = r;
    } else if (r != MAYFS_OK) {
        return r;
    }
    if (g_entries[idx].dir) return MAYFS_EISDIR;

    for (int i = 0; i < MAYFS_MAX_OPEN_FILES; i++) {
        if (g_files[i].used) continue;
        fh = &g_files[i];
        memset(fh, 0, sizeof(*fh));
        fh->used = 1;
        fh->index = idx;
        if (trunc && g_entries[idx].size) {
            g_entries[idx].size = 0;
            memset(g_entries[idx].data, 0, sizeof(g_entries[idx].data));
            g_dirty = 1;
        }
        *out = fh;
        return MAYFS_OK;
    }
    return MAYFS_ENOSPC;
}

int mayfs_file_read(struct mayfs_file *f, void *buf, uint32_t size, uint32_t offset) {
    uint32_t avail;
    if (!g_mounted) return MAYFS_EBUSY;
    if (!f || !f->used || !buf) return MAYFS_EINVAL;
    if (offset >= g_entries[f->index].size) return 0;
    avail = g_entries[f->index].size - offset;
    if (size > avail) size = avail;
    memcpy(buf, g_entries[f->index].data + offset, size);
    return (int)size;
}

int mayfs_file_write(struct mayfs_file *f, const void *buf, uint32_t size, uint32_t offset) {
    struct mayfs_entry *e;
    uint32_t room, old;

    if (!g_mounted) return MAYFS_EBUSY;
    if (!f || !f->used || (!buf && size)) return MAYFS_EINVAL;
    e = &g_entries[f->index];
    if (e->dir) return MAYFS_EISDIR;
    if (size == 0) return 0;
    if (offset >= MAYFS_FILE_MAX) return MAYFS_ENOSPC;
    room = MAYFS_FILE_MAX - offset;
    /* Reject an oversized write instead of silently truncating it: vfs_write_file()
     * hands the whole buffer to one write() call, so ENOSPC here means "the file
     * did not change" rather than "half of it landed". */
    if (size > room) return MAYFS_ENOSPC;

    old = e->size;
    if (offset > old) {
        /* deterministic zero fill for the hole between the old and new size */
        memset(e->data + old, 0, offset - old);
    }
    memcpy(e->data + offset, buf, size);
    if (offset + size > old) e->size = offset + size;
    /* MayOS invariant: the byte right after the payload is a NUL. */
    if (e->size < MAYFS_FILE_CAP) e->data[e->size] = 0;
    g_dirty = 1;
    return (int)size;
}

int mayfs_file_size(struct mayfs_file *f) {
    if (!f || !f->used) return MAYFS_EINVAL;
    return (int)g_entries[f->index].size;
}

int mayfs_file_close(struct mayfs_file *f) {
    if (!f || !f->used) return MAYFS_EINVAL;
    memset(f, 0, sizeof(*f));
    return MAYFS_OK;
}

/* ==================== directory handles ==================== */

int mayfs_opendir(const char *path, struct mayfs_dir **out) {
    struct mayfs_dir *d = NULL;
    int idx, r;

    if (!g_mounted) return MAYFS_EBUSY;
    if (!out) return MAYFS_EINVAL;
    r = mayfs_resolve(path, &idx);
    if (r != MAYFS_OK) return r;
    if (!g_entries[idx].dir) return MAYFS_ENOTDIR;
    for (int i = 0; i < MAYFS_MAX_OPEN_DIRS; i++)
        if (!g_dirs[i].used) { d = &g_dirs[i]; break; }
    if (!d) return MAYFS_ENOSPC;
    memset(d, 0, sizeof(*d));
    d->used = 1;
    d->index = idx;
    d->next = 1;        /* record 0 is the volume root, never its own child */
    *out = d;
    return MAYFS_OK;
}

int mayfs_readdir(struct mayfs_dir *d, char *name, uint32_t cap,
                  uint32_t *out_len, int *is_dir) {
    if (!g_mounted) return MAYFS_EBUSY;
    if (!d || !d->used || !name || cap == 0) return MAYFS_EINVAL;
    while (d->next < MAYFS_MAX_FILES) {
        uint32_t i = d->next++;
        if (!g_entries[i].used) continue;
        if (i == (uint32_t)d->index) continue;
        if (g_entries[i].parent != (uint32_t)d->index) continue;
        strncpy(name, g_entries[i].name, cap - 1);
        name[cap - 1] = '\0';
        if (out_len) *out_len = (uint32_t)strlen(name);
        if (is_dir) *is_dir = (int)g_entries[i].dir;
        return MAYFS_OK;
    }
    return MAYFS_ENOENT;     /* end of directory, like the other drivers */
}

int mayfs_closedir(struct mayfs_dir *d) {
    if (!d || !d->used) return MAYFS_EINVAL;
    memset(d, 0, sizeof(*d));
    return MAYFS_OK;
}

/* Exposed for the adapter's sync path: commit only when something changed. */
int mayfs_sync(void) {
    if (!g_mounted) return MAYFS_EBUSY;
    if (!mayfs_is_dirty()) return MAYFS_OK;
    return mayfs_commit();
}

