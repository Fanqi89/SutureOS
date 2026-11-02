/* SutureOS module: ISO9660 (ECMA-119) 只读文件系统 - 数据光盘读取
 *
 * 来源 / Source: CoolPotOS, module/iso9660/iso9660.c + iso9660.h
 *               (C:\Users\fanqi\Desktop\自研操作系统\CoolPotOS)
 *               许可证 / License: MIT, Copyright (c) 2024-2025 plos-clan
 * 改动 / Changes: 按 SutureOS 风格重写（不复制其 VFS 回调层）：
 *                 * 用 SutureOS 的 blk_read() 读 2048 字节块（设备扇区大小
 *                   在挂载时换算，因此 ATAPI 光驱(2048) 与硬盘上的 ISO(512)
 *                   都能读）；
 *                 * 目录记录按 length 字段步进、length==0 时补齐到下一块，
 *                   支持任意深度子目录（逐段解析路径）；
 *                 * 文件名去掉 ";1" 版本后缀与无扩展名的尾随 '.'，查找时
 *                   大小写不敏感（ISO9660 传统行为）；
 *                 * 新增 El Torito 引导记录探测（用于区分“启动盘”与“数据盘”）；
 *                 * 只实现只读路径：open/read/stat/opendir/readdir/readdir。
 *               未实现（如实记录）：Joliet 长名、Rock Ridge（POSIX 属性/长名/
 *               符号链接）、多扩展段文件（multi-extent）、UDF、写路径。
 *
 * 数据来源约定：路径都是“卷内相对路径”，以 '/' 开头，例如 "/"、"/SUB"、
 * "/SUB/NESTED.TXT"。VFS 适配层 kernel/fs/iso9660_vfs.c 负责去掉挂载点前缀。
 */
#ifndef STITCH_ISO9660_H
#define STITCH_ISO9660_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ISO9660_BLOCK_SIZE      2048u
#define ISO9660_MAX_NAME        255u
#define ISO9660_MAX_OPEN_FILES  8
#define ISO9660_MAX_OPEN_DIRS   8

/* 返回码（与 ext2_* 同风格：0 = 成功，负值 = 错误） */
#define ISO9660_OK         0
#define ISO9660_EIO      (-1)   /* 块设备读失败 / 设备扇区大小不支持 */
#define ISO9660_EBADFS   (-2)   /* 卷结构不合法（无 PVD、记录长度越界等） */
#define ISO9660_ENOENT   (-3)   /* 路径或目录项不存在 */
#define ISO9660_ENOTDIR  (-4)   /* 路径中间成分不是目录 */
#define ISO9660_ENOTFILE (-5)   /* 目标是目录，不能当文件打开 */
#define ISO9660_EINVAL   (-6)   /* 参数不合法 */
#define ISO9660_ENOSPC   (-7)   /* 句柄池耗尽 */

/* 一个已挂载的卷（单卷：光驱只有一张盘） */
struct iso9660_volume {
    int      drive;            /* 块设备驱动号（ATAPI 为 12..15） */
    uint32_t start_unit;       /* 卷起始 LBA，单位 = 设备扇区 */
    uint32_t units_per_block;  /* 每个 2048 字节逻辑块占几个设备扇区 */
    uint32_t device_sector;    /* 设备扇区大小（ATAPI=2048, 硬盘=512） */
    uint32_t vol_blocks;       /* PVD 的逻辑块总数 */
    uint32_t block_size;       /* PVD 的逻辑块大小（必须 2048） */
    uint32_t root_lba;         /* 根目录数据起始逻辑块号 */
    uint32_t root_size;        /* 根目录数据字节数 */
    uint32_t root_entries;     /* 根目录项数（不含 . / ..），仅用于挂载日志 */
    char     vol_id[33];       /* 卷标（PVD 的 32 字节空格填充字段） */
    int      has_boot_record;  /* 卷描述符里存在 El Torito 引导记录（启动盘） */
    int      mounted;
};

extern struct iso9660_volume g_iso9660;

struct iso9660_stat {
    uint32_t lba;              /* 数据起始逻辑块号 */
    uint32_t size;             /* 文件/目录数据字节数 */
    int      is_dir;
    char     name[ISO9660_MAX_NAME + 1];
};

struct iso9660_file {
    int      used;
    uint32_t lba;
    uint32_t size;
};

struct iso9660_dir {
    int      used;
    uint32_t lba;              /* 目录数据起始逻辑块号 */
    uint32_t size;
    uint32_t pos;              /* 目录数据内字节偏移 */
    uint32_t cached_block;     /* 已缓存的目录块号（相对起点），0xFFFFFFFF = 无 */
    uint8_t  block[ISO9660_BLOCK_SIZE];
};

/* ==================== 挂载 ==================== */

/* 只探测不挂载：读卷描述符集，成功返回 ISO9660_OK 并把信息填进 *out
 * （out 可为 NULL）。out->has_boot_record 用来区分启动盘/数据盘。 */
int iso9660_probe(int drive, uint32_t start_lba, struct iso9660_volume *out);

/* 探测 + 提交为当前卷并打印 `[iso9660] mount drive=.. vol=.. files=..`。 */
int iso9660_mount(int drive, uint32_t start_lba);
int iso9660_unmount(void);
int iso9660_is_mounted(void);

/* ==================== 路径操作（卷内相对路径） ==================== */

int iso9660_lookup(const char *path, struct iso9660_stat *out);
int iso9660_read_file(uint32_t lba, uint32_t size, void *buf,
                      uint32_t offset, uint32_t len);

/* 文件句柄（静态池）：open 之后按 offset 随机读，close 归还。 */
int iso9660_open(const char *path, struct iso9660_file **out);
int iso9660_file_read(struct iso9660_file *f, void *buf, uint32_t size, uint32_t offset);
int iso9660_close(struct iso9660_file *f);

/* 目录句柄：readdir 每个目录项返回一次，目录结束返回 ISO9660_ENOENT；
 * "." 与 ".." 被跳过（与 FAT32/ext2 驱动一致）。 */
int iso9660_opendir(const char *path, struct iso9660_dir **out);
int iso9660_readdir(struct iso9660_dir *d, char *name, uint32_t name_cap,
                    uint32_t *name_len, int *is_dir);
int iso9660_closedir(struct iso9660_dir *d);

/* 开机自检：/README.TXT 存在时校验其内容与大小（测试盘标记文件）；
 * 不存在时退化为“读根目录第一个普通文件并核对读到的字节数 == 目录记录长度”。
 * 打印 [iso9660] selftest PASS/FAIL/SKIP 行；PASS 返回 0，否则返回 -1。 */
int iso9660_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_ISO9660_H */
