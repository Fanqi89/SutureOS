/* SutureOS OS67 inode-filesystem port (Minix v1 on-disk layout) - core logic.
 *
 * The algorithms ported from OS67 (inc/minix.h, inc/inode.h, fs/inode.c,
 * fs/bitmap.c, fs/dir.c) are: the inode cache with reference counting and the
 * I_VALID / I_BUSY flags, inode number 1 = root, 16-byte directory entries
 * with a 14-byte name, the block map (7 direct zones + one single-indirect
 * table of 512 zone numbers) and bitmap numbering (inode bit = inode number,
 * zone bit = zone - fst_data_zone + 1).
 *
 * Read-only by design (see os67fs.h): no on-disk write path is enabled.
 */
#include "os67fs.h"
#include "block.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>

#define OS67FS_MAX_VOLUMES 2
#define OS67FS_SECTORS_PER_BLOCK 2   /* 1024-byte blocks on 512-byte sectors */

struct os67fs_volume {
    int      mounted;
    int      drive;
    struct os67fs_super sb;
    struct os67fs_inode icache[OS67FS_NINODE_CACHE];
};

static struct os67fs_volume g_vols[OS67FS_MAX_VOLUMES];
static int g_cur_vol = -1;

/* ---- block / inode primitives ------------------------------------------- */

static int os67fs_read_block(int vol, uint32_t block, void *buf)
{
    if (vol < 0 || vol >= OS67FS_MAX_VOLUMES || !g_vols[vol].mounted)
        return VFS_EIO;
    if (blk_read(g_vols[vol].drive,
                 (uint64_t)block * OS67FS_SECTORS_PER_BLOCK,
                 OS67FS_SECTORS_PER_BLOCK, buf) != 0)
        return VFS_EIO;
    return VFS_OK;
}

static uint32_t os67fs_iblk(const struct os67fs_super *sb, uint32_t ino)
{
    /* OS67: IBLK(sb, i) = 2 + imap_blk + zmap_blk + (i - 1) / IPB */
    return 2u + sb->imap_blk + sb->zmap_blk + (ino - 1u) / OS67FS_IPB;
}

static uint32_t os67fs_imap_blk(uint32_t ino)
{
    return 2u + (ino - 1u) / (OS67FS_BSIZE * 8u);
}

static uint32_t os67fs_zmap_blk(const struct os67fs_super *sb, uint32_t bit)
{
    return 2u + sb->imap_blk + bit / (OS67FS_BSIZE * 8u);
}

/* Load inode `ino` into the cache (or return the cached entry). */
static struct os67fs_inode *os67fs_iget(int vol, uint32_t ino)
{
    struct os67fs_volume *v = &g_vols[vol];
    if (ino == 0 || ino >= v->sb.ninodes) return NULL;

    for (int i = 0; i < OS67FS_NINODE_CACHE; i++) {
        if (v->icache[i].used && v->icache[i].ino == ino) {
            v->icache[i].ref++;
            return &v->icache[i];
        }
    }
    struct os67fs_inode *slot = NULL;
    for (int i = 0; i < OS67FS_NINODE_CACHE; i++) {
        if (!v->icache[i].used) { slot = &v->icache[i]; break; }
    }
    if (!slot) {
        /* recycle the least-referenced entry (OS67 does the same on a full cache) */
        for (int i = 0; i < OS67FS_NINODE_CACHE; i++) {
            if (v->icache[i].ref == 0) { slot = &v->icache[i]; break; }
        }
    }
    if (!slot) return NULL;

    uint8_t blk[OS67FS_BSIZE];
    if (os67fs_read_block(vol, os67fs_iblk(&v->sb, ino), blk) != VFS_OK) return NULL;
    const uint32_t off = ((ino - 1u) % OS67FS_IPB) * OS67FS_INODE_SIZE;

    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->ino = ino;
    slot->ref = 1;
    slot->mode = (uint16_t)(blk[off] | (blk[off + 1] << 8));
    /* uid at off+2 (unused), size/inode word at off+4, mtime at off+8 */
    slot->size = (uint32_t)blk[off + 4] | ((uint32_t)blk[off + 5] << 8) |
                 ((uint32_t)blk[off + 6] << 16) | ((uint32_t)blk[off + 7] << 24);
    slot->nlinks = blk[off + 13];
    for (int k = 0; k < 9; k++)
        slot->zone[k] = (uint16_t)(blk[off + 14 + 2 * k] |
                                   (blk[off + 15 + 2 * k] << 8));
    slot->valid = 1;
    return slot;
}

static void os67fs_iput(struct os67fs_inode *ip)
{
    if (ip && ip->ref > 0) ip->ref--;
}

/* Map logical block `lbn` of `ip` to a disk zone number (0 = hole). */
static uint32_t os67fs_bmap(int vol, struct os67fs_inode *ip, uint32_t lbn)
{
    if (lbn < OS67FS_NDIRECT) return ip->zone[lbn];
    lbn -= OS67FS_NDIRECT;
    if (lbn >= OS67FS_NINDIRECT) return 0;      /* double indirect unused in OS67 */
    if (!ip->zone[OS67FS_NDIRECT]) return 0;
    uint8_t table[OS67FS_BSIZE];
    if (os67fs_read_block(vol, ip->zone[OS67FS_NDIRECT], table) != VFS_OK) return 0;
    return (uint32_t)(table[lbn * 2] | (table[lbn * 2 + 1] << 8));
}

/* ---- path resolution ----------------------------------------------------- */

static int os67fs_name_eq(const char *want, uint32_t want_len,
                          const uint8_t *entry_name)
{
    uint32_t n = 0;
    while (n < OS67FS_NAME_LEN && entry_name[n]) n++;
    if (n != want_len) return 0;
    for (uint32_t i = 0; i < n; i++)
        if ((uint8_t)want[i] != entry_name[i]) return 0;
    return 1;
}

/* dir_lookup: walk the 16-byte entries of a directory inode. */
static int os67fs_dir_lookup(int vol, struct os67fs_inode *dir,
                             const char *name, uint32_t name_len,
                             uint32_t *out_ino, vfs_node_type_t *out_type)
{
    if ((dir->mode & OS67FS_S_IFMT) != OS67FS_S_IFDIR) return VFS_ENOTDIR;
    uint8_t blk[OS67FS_BSIZE];
    for (uint32_t off = 0; off < dir->size; off += 16) {
        const uint32_t lbn = off / OS67FS_BSIZE;
        const uint32_t boff = off % OS67FS_BSIZE;
        if (boff == 0) {
            uint32_t zone = os67fs_bmap(vol, dir, lbn);
            if (!zone) return VFS_ENOTFOUND;
            if (os67fs_read_block(vol, zone, blk) != VFS_OK) return VFS_EIO;
        }
        const uint32_t ino = (uint32_t)(blk[boff] | (blk[boff + 1] << 8));
        if (!ino) continue;
        if (!os67fs_name_eq(name, name_len, &blk[boff + 2])) continue;

        struct os67fs_inode *child = os67fs_iget(vol, ino);
        if (!child) return VFS_EIO;
        if (out_ino) *out_ino = ino;
        if (out_type)
            *out_type = ((child->mode & OS67FS_S_IFMT) == OS67FS_S_IFDIR)
                        ? VFS_NODE_DIR : VFS_NODE_FILE;
        os67fs_iput(child);
        return VFS_OK;
    }
    return VFS_ENOTFOUND;
}

int os67fs_resolve(int vol, const char *path, uint32_t *out_ino)
{
    if (vol < 0 || vol >= OS67FS_MAX_VOLUMES || !g_vols[vol].mounted)
        return VFS_EINVAL;
    uint32_t ino = 1;                              /* ROOT_INO = 1 */
    struct os67fs_inode *cur = os67fs_iget(vol, ino);
    if (!cur) return VFS_EIO;

    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        const uint32_t len = (uint32_t)(p - start);
        while (*p == '/') p++;
        if (len == 0) continue;

        uint32_t next = 0;
        const int rc = os67fs_dir_lookup(vol, cur, start, len, &next, NULL);
        os67fs_iput(cur);
        if (rc != VFS_OK) { g_cur_vol = vol; return rc; }
        ino = next;
        cur = os67fs_iget(vol, ino);
        if (!cur) return VFS_EIO;
    }
    os67fs_iput(cur);
    if (out_ino) *out_ino = ino;
    return VFS_OK;
}

int os67fs_inode_stat(int vol, uint32_t ino, struct vfs_stat *st)
{
    if (!st) return VFS_EINVAL;
    struct os67fs_inode *ip = os67fs_iget(vol, ino);
    if (!ip) return VFS_ENOTFOUND;
    memset(st, 0, sizeof(*st));
    st->type = ((ip->mode & OS67FS_S_IFMT) == OS67FS_S_IFDIR)
               ? VFS_NODE_DIR : VFS_NODE_FILE;
    st->size = ip->size;
    st->mode = ip->mode & 0777u;
    os67fs_iput(ip);
    return VFS_OK;
}

int os67fs_read(int vol, uint32_t ino, void *buf, uint32_t size, uint32_t offset)
{
    struct os67fs_inode *ip = os67fs_iget(vol, ino);
    if (!ip) return VFS_ENOTFOUND;
    if ((ip->mode & OS67FS_S_IFMT) == OS67FS_S_IFDIR) {
        os67fs_iput(ip);
        return VFS_EISDIR;
    }
    if (offset >= ip->size) { os67fs_iput(ip); return 0; }
    if (size > ip->size - offset) size = ip->size - offset;

    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < size) {
        const uint32_t pos = offset + done;
        const uint32_t lbn = pos / OS67FS_BSIZE;
        const uint32_t boff = pos % OS67FS_BSIZE;
        uint32_t chunk = OS67FS_BSIZE - boff;
        if (chunk > size - done) chunk = size - done;
        const uint32_t zone = os67fs_bmap(vol, ip, lbn);
        if (!zone) {
            memset(out + done, 0, chunk);       /* hole */
        } else {
            uint8_t blk[OS67FS_BSIZE];
            if (os67fs_read_block(vol, zone, blk) != VFS_OK) {
                os67fs_iput(ip);
                return VFS_EIO;
            }
            memcpy(out + done, blk + boff, chunk);
        }
        done += chunk;
    }
    os67fs_iput(ip);
    return (int)size;
}

int os67fs_dir_entry(int vol, uint32_t dir_ino, int index, char *name,
                     uint32_t cap, uint32_t *out_ino, vfs_node_type_t *type)
{
    struct os67fs_inode *dir = os67fs_iget(vol, dir_ino);
    if (!dir) return VFS_ENOTFOUND;
    if ((dir->mode & OS67FS_S_IFMT) != OS67FS_S_IFDIR) {
        os67fs_iput(dir);
        return VFS_ENOTDIR;
    }
    uint8_t blk[OS67FS_BSIZE];
    int seen = 0;
    for (uint32_t off = 0; off < dir->size; off += 16) {
        const uint32_t lbn = off / OS67FS_BSIZE;
        const uint32_t boff = off % OS67FS_BSIZE;
        if (boff == 0) {
            const uint32_t zone = os67fs_bmap(vol, dir, lbn);
            if (!zone) break;
            if (os67fs_read_block(vol, zone, blk) != VFS_OK) break;
        }
        const uint32_t ino = (uint32_t)(blk[boff] | (blk[boff + 1] << 8));
        if (!ino) continue;
        uint32_t n = 0;
        while (n < OS67FS_NAME_LEN && blk[boff + 2 + n]) n++;
        if (n == 1 && blk[boff + 2] == '.') continue;
        if (n == 2 && blk[boff + 3] == '.') continue;
        if (seen == index) {
            if (n + 1 > cap) { os67fs_iput(dir); return VFS_ENOSPC; }
            memcpy(name, &blk[boff + 2], n);
            name[n] = '\0';
            if (out_ino) *out_ino = ino;
            if (type) {
                struct os67fs_inode *child = os67fs_iget(vol, ino);
                *type = (child && (child->mode & OS67FS_S_IFMT) == OS67FS_S_IFDIR)
                        ? VFS_NODE_DIR : VFS_NODE_FILE;
                os67fs_iput(child);
            }
            os67fs_iput(dir);
            return 1;
        }
        seen++;
    }
    os67fs_iput(dir);
    return 0;
}

const char *os67fs_volume_name(int vol)
{
    return (vol >= 0 && vol < OS67FS_MAX_VOLUMES && g_vols[vol].mounted)
           ? "os67fs (minix v1, read-only)" : "os67fs";
}

int os67fs_volume_drive(int vol)
{
    return (vol >= 0 && vol < OS67FS_MAX_VOLUMES && g_vols[vol].mounted)
           ? g_vols[vol].drive : -1;
}

int os67fs_ready(void)
{
    for (int i = 0; i < OS67FS_MAX_VOLUMES; i++)
        if (g_vols[i].mounted) return 1;
    return 0;
}

/* ---- mount / probe ------------------------------------------------------- */

int os67fs_mount_drive(int drive)
{
    uint8_t blk[OS67FS_BSIZE];
    if (blk_read(drive, 1 * OS67FS_SECTORS_PER_BLOCK, OS67FS_SECTORS_PER_BLOCK, blk) != 0) {
        console_printf("[os67fs] no block device at drive %d\n", drive);
        return VFS_EIO;
    }
    const struct os67fs_super *sb = (const struct os67fs_super *)blk;
    if (sb->magic != OS67FS_MAGIC_MINIX1 && sb->magic != OS67FS_MAGIC_OS67) {
        console_printf("[os67fs] drive %d is not a minix v1 volume (magic=0x%x)\n",
                       drive, (unsigned)sb->magic);
        return VFS_EINVAL;
    }
    if (sb->log_zone_size != 0) {
        console_printf("[os67fs] unsupported log_zone_size=%u (only 1024-byte zones)\n",
                       (unsigned)sb->log_zone_size);
        return VFS_EINVAL;
    }
    if (sb->ninodes == 0 || sb->nzones == 0 || sb->fst_data_zone <= 2 ||
        sb->fst_data_zone >= sb->nzones) {
        console_printf("[os67fs] bad geometry ninodes=%u nzones=%u fst=%u\n",
                       (unsigned)sb->ninodes, (unsigned)sb->nzones,
                       (unsigned)sb->fst_data_zone);
        return VFS_EINVAL;
    }
    int slot = -1;
    for (int i = 0; i < OS67FS_MAX_VOLUMES; i++) {
        if (g_vols[i].mounted && g_vols[i].drive == drive) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < OS67FS_MAX_VOLUMES; i++)
            if (!g_vols[i].mounted) { slot = i; break; }
    }
    if (slot < 0) return VFS_ENOSPC;

    memset(&g_vols[slot], 0, sizeof(g_vols[slot]));
    g_vols[slot].mounted = 1;
    g_vols[slot].drive = drive;
    memcpy(&g_vols[slot].sb, blk, sizeof(struct os67fs_super));

    struct os67fs_inode *root = os67fs_iget(slot, 1);
    if (!root) {
        console_printf("[os67fs] mount failed: root inode 1 unreadable\n");
        g_vols[slot].mounted = 0;
        return VFS_EIO;
    }
    const int root_is_dir = ((root->mode & OS67FS_S_IFMT) == OS67FS_S_IFDIR);
    const uint32_t root_size = root->size;
    os67fs_iput(root);
    if (!root_is_dir) {
        console_printf("[os67fs] mount failed: inode 1 is not a directory\n");
        g_vols[slot].mounted = 0;
        return VFS_EINVAL;
    }
    g_cur_vol = slot;
    console_printf("[os67fs] mount drive=%d ninodes=%u zones=%u fst=%u inode=%uB root=%uB (read-only)\n",
                   drive, (unsigned)g_vols[slot].sb.ninodes,
                   (unsigned)g_vols[slot].sb.nzones,
                   (unsigned)g_vols[slot].sb.fst_data_zone,
                   (unsigned)OS67FS_INODE_SIZE, (unsigned)root_size);
    return VFS_OK;
}

int os67fs_boot_probe(int drive)
{
    uint8_t blk[OS67FS_BSIZE];
    /* Check the device first: a missing disk must not make the block layer
     * print its own "[blk] Read failed" line for every probe at boot. */
    if (drive < 0 || !blk_find_by_drive(drive)) {
        console_printf("[os67fs] no volume on drive %d (no such block device)\n", drive);
        return -1;
    }
    if (blk_read(drive, 1 * OS67FS_SECTORS_PER_BLOCK, OS67FS_SECTORS_PER_BLOCK, blk) != 0) {
        console_printf("[os67fs] no volume on drive %d (read failed)\n", drive);
        return -1;
    }
    const struct os67fs_super *sb = (const struct os67fs_super *)blk;
    if (sb->magic != OS67FS_MAGIC_MINIX1 && sb->magic != OS67FS_MAGIC_OS67) {
        console_printf("[os67fs] no volume on drive %d (magic=0x%x)\n",
                       drive, (unsigned)sb->magic);
        return -1;
    }
    console_printf("[os67fs] volume on drive %d: minix v1 ninodes=%u zones=%u fst=%u "
                   "(read-only; mount with: mount os67fs %d /mnt/o67)\n",
                   drive, (unsigned)sb->ninodes, (unsigned)sb->nzones,
                   (unsigned)sb->fst_data_zone, drive);
    return 0;
}
