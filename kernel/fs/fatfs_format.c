/* SutureOS: minimal FAT32 formatter (installer support)
 *
 * Source: 自己的实现，几何算法与 scripts/mkfat32_testimg.py（本仓库）一致：
 *   512 字节扇区 / 32 保留扇区 / 2 个 FAT / 根目录簇 2 / FSInfo 扇区 1 /
 *   备份引导扇区 6 / 介质字节 0xF8 / 卷标 "FAT32   "。宿主侧
 *   scripts/verify_fat32_write.py 的解析逻辑因此可以直接复用来验收。
 * License: GPL-3.0（SutureOS 本体）。
 *
 * 它只做“把一块分区变成空 FAT32 卷”这一件事：写引导扇区（含备份）、FSInfo、
 * 两个 FAT（前 3 项 + 其余清零）、根目录簇清零，然后按写入路径读回校验。
 * 不写引导代码（MBR / Limine 由安装器另外写）、不做坏簇标记、不分配簇。
 *
 * 簇大小：优先 4 KiB（8 扇区），但 FAT32 要求簇数 >= 65525，否则退回
 * 2 KiB / 1 KiB / 512 B；都达不到就如实失败（调用方打印原因）。
 */
#include "fatfs.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "block.h"
#include "heap.h"

#define FMT_SECTOR        512u
#define FMT_RESERVED      32u
#define FMT_NFATS         2u
#define FMT_ROOT_CLUSTER  2u
#define FMT_FSINFO_SECTOR 1u
#define FMT_BACKUP_SECTOR 6u
#define FMT_MEDIA_BYTE    0xF8u
#define FMT_ZERO_CHUNK    (64u * 1024u)          /* 128 sectors per zero write */
#define FMT_MIN_CLUSTERS  65525u                 /* genuine FAT32 floor */

static void fmt_put16(uint8_t *p, uint32_t off, uint32_t v) {
    p[off]     = (uint8_t)(v & 0xFF);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFF);
}

static void fmt_put32(uint8_t *p, uint32_t off, uint32_t v) {
    p[off]     = (uint8_t)(v & 0xFF);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    p[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    p[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}

/* Pick sectors/cluster + FAT size so that the volume really is FAT32. */
static int fmt_choose(uint32_t total_sectors, struct fat32_geometry *g) {
    static const uint32_t spc_try[] = { 8, 4, 2, 1 };
    uint32_t spc, fat_sz, data_lba, clusters;

    for (unsigned i = 0; i < sizeof(spc_try) / sizeof(spc_try[0]); i++) {
        spc = spc_try[i];
        fat_sz = 512;
        clusters = 0;
        for (int iter = 0; iter < 8; iter++) {
            data_lba = FMT_RESERVED + FMT_NFATS * fat_sz;
            if (total_sectors <= data_lba + spc) return FATFS_EINVAL;
            clusters = (total_sectors - data_lba) / spc;
            uint32_t need = ((clusters + 2u) * 4u + FMT_SECTOR - 1u) / FMT_SECTOR;
            if (need <= fat_sz) break;
            fat_sz = need;
        }
        if (clusters >= FMT_MIN_CLUSTERS) {
            g->start_lba = 0;                    /* filled by the caller */
            g->total_sectors = total_sectors;
            g->spc = spc;
            g->fat_sz = fat_sz;
            g->clusters = clusters;
            g->data_lba = FMT_RESERVED + FMT_NFATS * fat_sz;
            g->root_cluster = FMT_ROOT_CLUSTER;
            return FATFS_OK;
        }
    }
    return FATFS_EINVAL;
}

/* Write `sectors` zeroed sectors at partition-relative `lba`. */
static int fmt_zero(uint8_t *zero, int drive, uint32_t start_lba,
                    uint32_t lba, uint32_t sectors) {
    while (sectors > 0) {
        uint32_t n = (sectors > (FMT_ZERO_CHUNK / FMT_SECTOR))
                   ? (FMT_ZERO_CHUNK / FMT_SECTOR) : sectors;
        if (blk_write(drive, (uint64_t)start_lba + lba, n, zero) != 0) return FATFS_EIO;
        lba += n;
        sectors -= n;
    }
    return FATFS_OK;
}

int fat32_format_partition(int drive, uint32_t start_lba, uint32_t total_sectors,
                           struct fat32_geometry *out) {
    struct fat32_geometry g;
    uint8_t *bs = NULL, *zero = NULL;
    int r = FATFS_ERR;

    if (!total_sectors) return FATFS_EINVAL;
    if (fmt_choose(total_sectors, &g) != FATFS_OK) {
        console_printf("[fat32fmt] volume too small for FAT32 (need >= %u sectors)\n",
                       (uint64_t)(FMT_MIN_CLUSTERS + 1024u));
        return FATFS_EINVAL;
    }
    g.start_lba = start_lba;

    bs = lemis_heap_alloc(FMT_SECTOR);
    zero = lemis_heap_alloc(FMT_ZERO_CHUNK);
    if (!bs || !zero) {
        console_printf("[fat32fmt] out of memory (heap)\n");
        goto done;
    }
    memset(bs, 0, FMT_SECTOR);
    memset(zero, 0, FMT_ZERO_CHUNK);

    /* ---- boot sector ---- */
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;             /* jmp / nop */
    memcpy(bs + 3, "SUTUREOS", 8);                        /* OEM */
    fmt_put16(bs, 11, FMT_SECTOR);
    bs[13] = (uint8_t)g.spc;
    fmt_put16(bs, 14, FMT_RESERVED);
    bs[16] = (uint8_t)FMT_NFATS;
    fmt_put16(bs, 17, 0);                                 /* root entries: 0 = FAT32 */
    fmt_put16(bs, 19, 0);                                 /* totsec16: 0 */
    bs[21] = (uint8_t)FMT_MEDIA_BYTE;
    fmt_put16(bs, 22, 0);                                 /* fatsz16: 0 */
    fmt_put16(bs, 24, 63);                                /* sectors/track */
    fmt_put16(bs, 26, 255);                               /* heads */
    fmt_put32(bs, 28, start_lba);                         /* hidden = partition LBA */
    fmt_put32(bs, 32, total_sectors);                     /* totsec32 */
    fmt_put32(bs, 36, g.fat_sz);
    fmt_put16(bs, 40, 0);                                 /* ext flags */
    fmt_put16(bs, 42, 0);                                 /* fs version */
    fmt_put32(bs, 44, FMT_ROOT_CLUSTER);
    fmt_put32(bs, 48, FMT_FSINFO_SECTOR);
    fmt_put32(bs, 52, FMT_BACKUP_SECTOR);
    bs[64] = 0x80;                                        /* drive number */
    bs[66] = 0x29;                                        /* extended boot sig */
    fmt_put32(bs, 67, 0x5A7C0DE5u);                       /* volume id */
    memcpy(bs + 71, "SUTUREOS  ", 11);                    /* volume label */
    memcpy(bs + 82, "FAT32   ", 8);                       /* filesystem type */
    bs[510] = 0x55; bs[511] = 0xAA;

    if (blk_write(drive, (uint64_t)start_lba, 1, bs) != 0) {
        console_printf("[fat32fmt] boot sector write failed\n");
        goto done;
    }
    if (blk_write(drive, (uint64_t)start_lba + FMT_BACKUP_SECTOR, 1, bs) != 0) {
        console_printf("[fat32fmt] backup boot sector write failed\n");
        goto done;
    }

    /* ---- FSInfo ---- */
    memset(bs, 0, FMT_SECTOR);
    fmt_put32(bs, 0, 0x41615252u);                        /* lead signature */
    fmt_put32(bs, 484, 0x61417272u);                      /* structure signature */
    fmt_put32(bs, 488, g.clusters - 1u);                  /* free clusters hint */
    fmt_put32(bs, 492, 3u);                               /* next free hint */
    fmt_put32(bs, 508, 0xAA5555AAu);                      /* trail signature */
    if (blk_write(drive, (uint64_t)start_lba + FMT_FSINFO_SECTOR, 1, bs) != 0) {
        console_printf("[fat32fmt] FSInfo write failed\n");
        goto done;
    }

    /* ---- both FATs: zero, then entries 0/1 + EOC for the root cluster ---- */
    for (uint32_t f = 0; f < FMT_NFATS; f++) {
        const uint32_t fat_lba = FMT_RESERVED + f * g.fat_sz;
        uint32_t v;
        memset(zero, 0, FMT_ZERO_CHUNK);
        fmt_put32(zero, 0, 0x0FFFFFF8u);                  /* media descriptor */
        fmt_put32(zero, 4, 0x0FFFFFFFu);                  /* EOC / dirty clear */
        fmt_put32(zero, 4u * FMT_ROOT_CLUSTER, 0x0FFFFFFFu);  /* root dir: EOC */
        v = (g.fat_sz > (FMT_ZERO_CHUNK / FMT_SECTOR))
          ? (FMT_ZERO_CHUNK / FMT_SECTOR) : g.fat_sz;
        if (blk_write(drive, (uint64_t)start_lba + fat_lba, v, zero) != 0) {
            console_printf("[fat32fmt] FAT%u write failed\n", (uint64_t)f);
            goto done;
        }
        if (g.fat_sz > v) {
            int zr = fmt_zero(zero, drive, start_lba, fat_lba + v, g.fat_sz - v);
            if (zr != FATFS_OK) {
                console_printf("[fat32fmt] FAT%u zero fill failed\n", (uint64_t)f);
                goto done;
            }
        }
    }

    /* ---- root directory cluster: empty (all zero) ----
     * NOTE: re-zero the scratch buffer first. It still holds the FAT entries
     * written above, and writing that into the root cluster leaves a bogus
     * "0xF8 0xFF ..." slot that every LFN-aware reader (including this kernel's
     * driver and the host verifier) then treats as a broken long-name entry. */
    {
        int zr;
        memset(zero, 0, FMT_ZERO_CHUNK);
        zr = fmt_zero(zero, drive, start_lba, g.data_lba, g.spc);
        if (zr != FATFS_OK) {
            console_printf("[fat32fmt] root cluster write failed\n");
            goto done;
        }
    }

    /* ---- read back what the mounted driver will look at ---- */
    if (blk_read(drive, (uint64_t)start_lba, 1, bs) != 0) {
        console_printf("[fat32fmt] read back failed\n");
        goto done;
    }
    if (bs[510] != 0x55 || bs[511] != 0xAA || memcmp(bs + 82, "FAT32   ", 8) != 0) {
        console_printf("[fat32fmt] read back mismatch (boot sector)\n");
        goto done;
    }
    {
        uint8_t *fat = lemis_heap_alloc(FMT_SECTOR);
        if (!fat) goto done;
        if (blk_read(drive, (uint64_t)start_lba + FMT_RESERVED, 1, fat) != 0 ||
            fat[4u * FMT_ROOT_CLUSTER] != 0xFF || fat[4u * FMT_ROOT_CLUSTER + 3] != 0x0F) {
            console_printf("[fat32fmt] read back mismatch (FAT root entry)\n");
            lemis_heap_free(fat);
            goto done;
        }
        lemis_heap_free(fat);
    }
    (void)blk_flush(drive);

    console_printf("[fat32fmt] ok start_lba=%u sectors=%u spc=%u clusters=%u fat_sz=%u\n",
                   (uint64_t)start_lba, (uint64_t)total_sectors, (uint64_t)g.spc,
                   (uint64_t)g.clusters, (uint64_t)g.fat_sz);
    if (out) *out = g;
    r = FATFS_OK;

done:
    if (bs) lemis_heap_free(bs);
    if (zero) lemis_heap_free(zero);
    return r;
}
