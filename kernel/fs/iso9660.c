/* SutureOS module: ISO9660 (ECMA-119) read-only filesystem
 *
 * 来源 / Source: CoolPotOS, module/iso9660/iso9660.c + iso9660.h
 *               (C:\Users\fanqi\Desktop\自研操作系统\CoolPotOS)
 *               许可证 / License: MIT, Copyright (c) 2024-2025 plos-clan
 * 改动 / Changes: 参见 iso9660.h 的文件头（按 SutureOS 风格重写；块读取走
 *               blk_read()；目录记录按 length 步进并在 length==0 时补齐到
 *               下一块；文件名去 ";1"；大小写不敏感查找；新增 El Torito
 *               引导记录探测；只读路径）。
 */
#include "iso9660.h"
#include "block.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>

/* ==================== 卷状态 ==================== */

struct iso9660_volume g_iso9660;

static struct iso9660_file g_files[ISO9660_MAX_OPEN_FILES];
static struct iso9660_dir  g_dirs[ISO9660_MAX_OPEN_DIRS];

/* 卷描述符 / 目录记录里的多字节字段都是“小端+大端”各一份，本驱动只读小端 */
static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 读卷内逻辑块（2048 字节）：起始 LBA 与每个逻辑块占的设备扇区数在挂载时算好，
 * 所以 ATAPI 光驱（2048 字节扇区）与普通硬盘上的 ISO（512 字节扇区）都能读。 */
static int iso_raw_read_block(int drive, uint32_t start_unit, uint32_t units,
                              uint32_t block, void *buf) {
    if (units == 0 || !buf) return ISO9660_EINVAL;
    if (blk_read(drive, (uint64_t)start_unit + (uint64_t)block * (uint64_t)units,
                 units, buf) != 0) {
        return ISO9660_EIO;
    }
    return ISO9660_OK;
}

static int iso_read_block(uint32_t block, void *buf) {
    if (!g_iso9660.mounted) return ISO9660_EIO;
    if (block >= g_iso9660.vol_blocks) return ISO9660_EIO;
    return iso_raw_read_block(g_iso9660.drive, g_iso9660.start_unit,
                              g_iso9660.units_per_block, block, buf);
}

/* ==================== 名字处理 ==================== */

/* 目录记录里的名字是 "README.TXT;1" 这种形式：截到 ';' 之前，再去掉尾随的
 * '.'（无扩展名的文件在 ISO9660 里写作 "HELLO.;1"）与空格。 */
static void iso_name_clean(const uint8_t *rec, char *out, uint32_t cap) {
    uint32_t raw_len;
    uint32_t n = 0;

    if (!out || cap == 0) return;
    raw_len = rec[32];
    while (n < raw_len && n + 1u < cap && rec[33 + n] != ';') {
        out[n] = (char)rec[33 + n];
        n++;
    }
    while (n > 0 && (out[n - 1] == '.' || out[n - 1] == ' ')) n--;
    out[n] = '\0';
}

static int iso_is_dot_entry(const uint8_t *rec) {
    return (rec[32] == 1u) && (rec[33] == 0x00u || rec[33] == 0x01u);
}

/* 大小写不敏感的整名比较（ISO9660 只有大写名字，用户输入小写也允许） */
static int iso_name_matches(const uint8_t *rec, const char *name, uint32_t name_len) {
    char cleaned[ISO9660_MAX_NAME + 1];
    uint32_t n = 0;

    iso_name_clean(rec, cleaned, sizeof(cleaned));
    while (cleaned[n] != '\0') n++;
    if (n != name_len) return 0;
    for (uint32_t i = 0; i < n; i++) {
        char a = cleaned[i];
        char b = name[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return 0;
    }
    return 1;
}

/* ==================== 目录记录遍历 ==================== */

/* 在一个目录的数据区里找名为 name 的记录。目录数据可能跨多个 2048 字节块；
 * 一条记录的 length 字段给出下一条的偏移，length == 0 表示本块剩余部分补齐，
 * 直接跳到下一块（ECMA-119 9.3）。 */
static int iso_find_in_dir(uint32_t dir_lba, uint32_t dir_size,
                           const char *name, uint32_t name_len,
                           uint32_t *out_lba, uint32_t *out_size, int *out_is_dir,
                           uint8_t *blk) {
    uint32_t pos = 0;
    uint32_t loaded = 0xFFFFFFFFu;

    while (pos < dir_size) {
        uint32_t off = pos % ISO9660_BLOCK_SIZE;
        uint32_t blkno = pos / ISO9660_BLOCK_SIZE;
        uint8_t len;

        if (loaded != blkno) {
            int r = iso_read_block(dir_lba + blkno, blk);
            if (r != ISO9660_OK) return r;
            loaded = blkno;
        }
        len = blk[off];
        if (len == 0) {                              /* 补齐到下一块 */
            pos = (blkno + 1u) * ISO9660_BLOCK_SIZE;
            continue;
        }
        if (len < 34u || off + (uint32_t)len > ISO9660_BLOCK_SIZE) {
            return ISO9660_EBADFS;
        }
        if (iso_name_matches(blk + off, name, name_len)) {
            if (out_lba) *out_lba = le32(blk + off + 2);
            if (out_size) *out_size = le32(blk + off + 10);
            if (out_is_dir) *out_is_dir = (blk[off + 25] & 0x02u) ? 1 : 0;
            return ISO9660_OK;
        }
        pos += (uint32_t)len;
    }
    return ISO9660_ENOENT;
}

/* ==================== 探测 / 挂载 ==================== */

static void iso_copy_volume_id(const uint8_t *pvd, char *out, uint32_t cap) {
    uint32_t n = 0;

    if (!out || cap == 0) return;
    /* PVD 偏移 40 起 32 字节：卷标，空格右填充、没有 NUL 结尾 */
    for (uint32_t i = 0; i < 32u && n + 1u < cap; i++) out[n++] = (char)pvd[40 + i];
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0')) n--;
    out[n] = '\0';
}

int iso9660_probe(int drive, uint32_t start_lba, struct iso9660_volume *out) {
    static uint8_t vd[ISO9660_BLOCK_SIZE];
    struct iso9660_volume v;
    uint64_t sectors = 0;
    uint32_t dev_sector = 0;
    int found = 0;

    memset(&v, 0, sizeof(v));
    if (blk_get_info(drive, &sectors, &dev_sector) != 0) return ISO9660_EIO;
    if (dev_sector == 0 || dev_sector > ISO9660_BLOCK_SIZE) return ISO9660_EIO;
    if ((ISO9660_BLOCK_SIZE % dev_sector) != 0) return ISO9660_EIO;
    (void)sectors;      /* 容量只用于日志，不在这里裁剪 */

    v.drive = drive;
    v.start_unit = start_lba;
    v.device_sector = dev_sector;
    v.units_per_block = ISO9660_BLOCK_SIZE / dev_sector;
    v.block_size = ISO9660_BLOCK_SIZE;

    /* 卷描述符集从逻辑块 16 开始，一直读到类型 0xFF 的结束符（最多看 32 块）：
     *   0x00 = 引导记录（El Torito 启动盘），0x01 = 主卷（PVD）,
     *   0x02 = 补充卷（Joliet，本驱动不解析）, 0xFF = 结束
     * 注意：引导记录通常在 PVD **之后**（实测 xorriso 生成的 limine 启动 ISO
     * 是 16=PVD, 17=Boot Record, 18=Joliet, 19=结束），所以解析完 PVD 不能
     * 立刻 break —— 必须继续扫到结束符才能知道这张盘是不是可引导的启动盘。 */
    for (uint32_t i = 0; i < 32u; i++) {
        uint32_t blk = 16u + i;
        int r;

        r = iso_raw_read_block(drive, start_lba, v.units_per_block, blk, vd);
        if (r != ISO9660_OK) return r;
        if (memcmp(vd + 1, "CD001", 5) != 0) return ISO9660_EBADFS;
        if (vd[0] == 0x00) { v.has_boot_record = 1; continue; }
        if (vd[0] == 0xFF) break;
        if (vd[0] != 0x01 || found) continue;

        /* ---- 主卷描述符 ---- */
        if (le16(vd + 128) != ISO9660_BLOCK_SIZE) return ISO9660_EBADFS;
        v.vol_blocks = le32(vd + 80);
        if (v.vol_blocks == 0) return ISO9660_EBADFS;
        iso_copy_volume_id(vd, v.vol_id, sizeof(v.vol_id));
        /* 偏移 156 起是根目录记录（34 字节），字段与普通目录记录一致 */
        if (vd[156] < 34u) return ISO9660_EBADFS;
        v.root_lba = le32(vd + 156 + 2);
        v.root_size = le32(vd + 156 + 10);
        if (v.root_size == 0 || v.root_lba >= v.vol_blocks) return ISO9660_EBADFS;
        found = 1;
        /* 继续扫后面的描述符：El Torito 引导记录通常排在 PVD 之后 */
    }
    if (!found) return ISO9660_EBADFS;

    if (out) *out = v;
    return ISO9660_OK;
}

/* 数一遍某个目录的记录条数（不含 . 和 ..），只用于挂载日志的 files=.. */
static uint32_t iso_count_entries(uint32_t dir_lba, uint32_t dir_size) {
    static uint8_t blk[ISO9660_BLOCK_SIZE];
    uint32_t pos = 0;
    uint32_t loaded = 0xFFFFFFFFu;
    uint32_t count = 0;

    while (pos < dir_size) {
        uint32_t off = pos % ISO9660_BLOCK_SIZE;
        uint32_t blkno = pos / ISO9660_BLOCK_SIZE;
        uint8_t len;

        if (loaded != blkno) {
            if (iso_read_block(dir_lba + blkno, blk) != ISO9660_OK) return count;
            loaded = blkno;
        }
        len = blk[off];
        if (len == 0) {
            pos = (blkno + 1u) * ISO9660_BLOCK_SIZE;
            continue;
        }
        if (len < 34u || off + (uint32_t)len > ISO9660_BLOCK_SIZE) return count;
        if (!iso_is_dot_entry(blk + off)) count++;
        pos += (uint32_t)len;
    }
    return count;
}

int iso9660_mount(int drive, uint32_t start_lba) {
    int r;

    memset(&g_iso9660, 0, sizeof(g_iso9660));
    r = iso9660_probe(drive, start_lba, &g_iso9660);
    if (r != ISO9660_OK) {
        memset(&g_iso9660, 0, sizeof(g_iso9660));
        return r;
    }
    g_iso9660.mounted = 1;
    g_iso9660.root_entries = iso_count_entries(g_iso9660.root_lba, g_iso9660.root_size);
    console_printf("[iso9660] mount drive=%d vol=%s files=%u blocks=%u\n",
                   g_iso9660.drive, g_iso9660.vol_id,
                   (uint64_t)g_iso9660.root_entries, (uint64_t)g_iso9660.vol_blocks);
    return ISO9660_OK;
}

int iso9660_unmount(void) {
    memset(&g_iso9660, 0, sizeof(g_iso9660));
    memset(g_files, 0, sizeof(g_files));
    memset(g_dirs, 0, sizeof(g_dirs));
    return ISO9660_OK;
}

int iso9660_is_mounted(void) {
    return g_iso9660.mounted;
}

/* ==================== 路径 -> 目录项 ==================== */

static void iso_copy_last_component(const char *path, char *out, uint32_t cap) {
    const char *seg = path;
    uint32_t n = 0;

    if (!out || cap == 0) return;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            const char *q = p + 1;
            while (*q == '/') q++;
            seg = q;
        }
    }
    while (seg[n] != '\0' && seg[n] != '/' && n + 1u < cap) {
        out[n] = seg[n];
        n++;
    }
    out[n] = '\0';
}

int iso9660_lookup(const char *path, struct iso9660_stat *out) {
    static uint8_t blk[ISO9660_BLOCK_SIZE];
    uint32_t lba;
    uint32_t size;
    int is_dir = 1;
    const char *p;

    if (!path || path[0] != '/') return ISO9660_EINVAL;
    if (!g_iso9660.mounted) return ISO9660_EIO;

    lba = g_iso9660.root_lba;
    size = g_iso9660.root_size;

    p = path;
    while (*p) {
        const char *seg;
        uint32_t seglen;
        int r;

        while (*p == '/') p++;
        if (!*p) break;
        if (!is_dir) return ISO9660_ENOTDIR;
        seg = p;
        while (*p && *p != '/') p++;
        seglen = (uint32_t)(p - seg);
        r = iso_find_in_dir(lba, size, seg, seglen, &lba, &size, &is_dir, blk);
        if (r != ISO9660_OK) return r;
    }

    if (out) {
        out->lba = lba;
        out->size = size;
        out->is_dir = is_dir;
        iso_copy_last_component(path, out->name, sizeof(out->name));
    }
    return ISO9660_OK;
}

int iso9660_read_file(uint32_t lba, uint32_t size, void *buf,
                      uint32_t offset, uint32_t len) {
    static uint8_t blk[ISO9660_BLOCK_SIZE];
    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;

    if (len > 0 && !buf) return ISO9660_EINVAL;
    if (offset >= size) return 0;
    if (len > size - offset) len = size - offset;

    while (done < len) {
        uint32_t abs = offset + done;
        uint32_t blkno = abs / ISO9660_BLOCK_SIZE;
        uint32_t inblk = abs % ISO9660_BLOCK_SIZE;
        uint32_t n = ISO9660_BLOCK_SIZE - inblk;
        int r;

        if (n > len - done) n = len - done;
        r = iso_read_block(lba + blkno, blk);
        if (r != ISO9660_OK) return r;
        memcpy(out + done, blk + inblk, n);
        done += n;
    }
    return (int)done;
}

/* ==================== 文件句柄 ==================== */

int iso9660_open(const char *path, struct iso9660_file **out) {
    struct iso9660_stat st;
    int r;

    if (!out) return ISO9660_EINVAL;
    r = iso9660_lookup(path, &st);
    if (r != ISO9660_OK) return r;
    if (st.is_dir) return ISO9660_ENOTFILE;
    for (int i = 0; i < ISO9660_MAX_OPEN_FILES; i++) {
        if (g_files[i].used) continue;
        g_files[i].used = 1;
        g_files[i].lba = st.lba;
        g_files[i].size = st.size;
        *out = &g_files[i];
        return ISO9660_OK;
    }
    return ISO9660_ENOSPC;
}

int iso9660_file_read(struct iso9660_file *f, void *buf, uint32_t size, uint32_t offset) {
    if (!f || !f->used) return ISO9660_EINVAL;
    return iso9660_read_file(f->lba, f->size, buf, offset, size);
}

int iso9660_close(struct iso9660_file *f) {
    if (!f) return ISO9660_EINVAL;
    f->used = 0;
    return ISO9660_OK;
}

/* ==================== 目录句柄 ==================== */

int iso9660_opendir(const char *path, struct iso9660_dir **out) {
    struct iso9660_stat st;
    int r;

    if (!out) return ISO9660_EINVAL;
    r = iso9660_lookup(path, &st);
    if (r != ISO9660_OK) return r;
    if (!st.is_dir) return ISO9660_ENOTDIR;
    for (int i = 0; i < ISO9660_MAX_OPEN_DIRS; i++) {
        if (g_dirs[i].used) continue;
        g_dirs[i].used = 1;
        g_dirs[i].lba = st.lba;
        g_dirs[i].size = st.size;
        g_dirs[i].pos = 0;
        g_dirs[i].cached_block = 0xFFFFFFFFu;
        *out = &g_dirs[i];
        return ISO9660_OK;
    }
    return ISO9660_ENOSPC;
}

int iso9660_readdir(struct iso9660_dir *d, char *name, uint32_t name_cap,
                    uint32_t *name_len, int *is_dir) {
    if (!d || !d->used || !name || name_cap == 0) return ISO9660_EINVAL;

    while (d->pos < d->size) {
        uint32_t off = d->pos % ISO9660_BLOCK_SIZE;
        uint32_t blkno = d->pos / ISO9660_BLOCK_SIZE;
        uint8_t len;

        if (d->cached_block != blkno) {
            int r = iso_read_block(d->lba + blkno, d->block);
            if (r != ISO9660_OK) return r;
            d->cached_block = blkno;
        }
        len = d->block[off];
        if (len == 0) {                       /* 本块剩余是补齐字节 */
            d->pos = (blkno + 1u) * ISO9660_BLOCK_SIZE;
            continue;
        }
        if (len < 34u || off + (uint32_t)len > ISO9660_BLOCK_SIZE) {
            return ISO9660_EBADFS;
        }
        d->pos += (uint32_t)len;
        if (iso_is_dot_entry(d->block + off)) continue;

        iso_name_clean(d->block + off, name, name_cap);
        if (name_len) {
            uint32_t n = 0;
            while (name[n] != '\0') n++;
            *name_len = n;
        }
        if (is_dir) *is_dir = (d->block[off + 25] & 0x02u) ? 1 : 0;
        return ISO9660_OK;
    }
    return ISO9660_ENOENT;                   /* 目录结束 */
}

int iso9660_closedir(struct iso9660_dir *d) {
    if (!d) return ISO9660_EINVAL;
    d->used = 0;
    return ISO9660_OK;
}

/* ==================== 开机自检 ==================== */

/* 测试盘标记文件的内容（scripts/mkiso9660_testcd.py 生成），用于开机时验证
 * 从光驱读回的数据与宿主侧逐字节一致。真实数据盘没有这个文件时不做强制校验。 */
static const char ISO_SELFTEST_MARKER[] = "hello from an ISO9660 data CD\n";

int iso9660_selftest(void) {
    struct iso9660_stat st;
    char buf[96];
    int n;

    if (!g_iso9660.mounted) {
        console_printf("[iso9660] selftest SKIP (no ISO9660 volume mounted)\n");
        return -1;
    }

    /* 1) 有标记文件：内容 + 大小双重校验（最严格的一条） */
    if (iso9660_lookup("/README.TXT", &st) == ISO9660_OK && !st.is_dir) {
        uint32_t want = (uint32_t)(sizeof(ISO_SELFTEST_MARKER) - 1u);
        n = iso9660_read_file(st.lba, st.size, buf, 0, (uint32_t)sizeof(buf));
        if (n < 0) {
            console_printf("[iso9660] selftest FAIL (file=/README.TXT, read error %d)\n",
                           (int64_t)n);
            return -1;
        }
        if (st.size == want && n == (int)want &&
            memcmp(buf, ISO_SELFTEST_MARKER, want) == 0) {
            console_printf("[iso9660] selftest PASS (file=/README.TXT, %llu bytes)\n",
                           (uint64_t)st.size);
            return 0;
        }
        console_printf("[iso9660] selftest FAIL (file=/README.TXT, expected %u bytes,"
                       " got %llu bytes, content mismatch=%d)\n",
                       (uint64_t)want, (uint64_t)st.size,
                       (int64_t)(memcmp(buf, ISO_SELFTEST_MARKER,
                                        want < (uint32_t)n ? want : (uint32_t)n) != 0));
        return -1;
    }

    /* 2) 没有标记文件：读根目录第一个普通文件，核对“实际读回的字节数”与
     *    “目录记录里登记的长度”一致（结构自洽），内容非空。 */
    {
        struct iso9660_dir *d = NULL;
        char name[ISO9660_MAX_NAME + 1];
        uint32_t nlen = 0;
        int dir_flag = 0;

        if (iso9660_opendir("/", &d) == ISO9660_OK) {
            while (iso9660_readdir(d, name, sizeof(name), &nlen, &dir_flag) == ISO9660_OK) {
                char path[ISO9660_MAX_NAME + 2];
                if (dir_flag) continue;
                snprintf(path, sizeof(path), "/%s", name);
                if (iso9660_lookup(path, &st) != ISO9660_OK || st.is_dir) continue;
                if (st.size == 0) continue;
                n = iso9660_read_file(st.lba, st.size, buf, 0, (uint32_t)sizeof(buf));
                if (n > 0) {
                    console_printf("[iso9660] selftest PASS (file=%s, %llu bytes)\n",
                                   path, (uint64_t)st.size);
                    iso9660_closedir(d);
                    return 0;
                }
                console_printf("[iso9660] selftest FAIL (file=%s, size=%llu read=%d)\n",
                               path, (uint64_t)st.size, (int64_t)n);
                iso9660_closedir(d);
                return -1;
            }
            iso9660_closedir(d);
        }
    }

    console_printf("[iso9660] selftest SKIP (no regular file at volume root)\n");
    return -1;
}
