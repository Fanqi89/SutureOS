/* SutureOS module: Block device layer - 统一块设备接口
 * Source: 基于 SpiritFoxOS AHCI、Cinux NVMe、VimtuOS VirtIO-blk 移植
 * Original licenses: GPL-3.0 (SpiritFoxOS), MIT (Cinux), GPL-3.0 (VimtuOS)
 * Changes: Adapted to SutureOS; prefix blk_; unified IBlockDevice interface
 */
#ifndef STITCH_BLOCK_H
#define STITCH_BLOCK_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 块设备类型 */
#define BLK_TYPE_NONE     0
#define BLK_TYPE_NVME     1
#define BLK_TYPE_AHCI     2
#define BLK_TYPE_VIRTIO   3
#define BLK_TYPE_ATA      4
#define BLK_TYPE_RAM      5

/* 驱动器号分配（全树必须保持互不重叠——blk_register() 不会拒绝重复的
 * 驱动器号，blk_find_by_drive() 返回最后注册的那个，所以冲突只能靠约定避免）：
 *   0,  1   ATA PIO 主/从盘            (block.c blk_ata_probe)
 *   2,  3   保留给 ATA 次通道
 *   4..7    AHCI/SATA 盘 sata0..sata3  (AHCI_DRIVE_BASE + 序号)
 *   8       NVMe 命名空间 nvme0        (NVME_DRIVE_NVME0)
 *   9       RAM 磁盘 ram0              (BLK_DRIVE_RAM)
 *   10      VirtIO-blk 盘 vda          (VIRTIO_BLK_DRIVE)
 */
#define BLK_DRIVE_RAM     9
#define BLK_DRIVE_AHCI_BASE 4
#define BLK_DRIVE_NVME0     8
#define BLK_DRIVE_VIRTIO0   10

/* 块设备标志 */
#define BLK_FLAG_READONLY  (1 << 0)
#define BLK_FLAG_REMOVABLE (1 << 1)

/* 块设备操作 */
struct blk_device;

/* 块设备操作表 */
struct blk_ops {
    int (*read)(struct blk_device* dev, uint64_t lba, uint32_t count, void* buf);
    int (*write)(struct blk_device* dev, uint64_t lba, uint32_t count, const void* buf);
    int (*flush)(struct blk_device* dev);
    int (*ioctl)(struct blk_device* dev, unsigned long cmd, void* arg);
};

/* 块设备结构 */
struct blk_device {
    const char* name;
    int type;
    int drive;
    uint32_t flags;
    uint64_t sector_count;
    uint32_t sector_size;
    uint32_t block_size;
    const struct blk_ops* ops;
    void* driver_data;
    struct blk_device* next;
};

/* 全局块设备链表 */
extern struct blk_device* g_blk_devices;

/* 块设备层初始化 */
int blk_init(void);

/* 注册块设备 */
int blk_register(struct blk_device* dev);

/* 注销块设备 */
int blk_unregister(struct blk_device* dev);

/* 按驱动器号查找块设备 */
struct blk_device* blk_find_by_drive(int drive);

/* 按名称查找块设备 */
struct blk_device* blk_find_by_name(const char* name);

/* 读取块 */
int blk_read(int drive, uint64_t lba, uint32_t count, void* buf);

/* 写入块 */
int blk_write(int drive, uint64_t lba, uint32_t count, const void* buf);

/* 刷新块 */
int blk_flush(int drive);

/* 获取块设备信息 */
int blk_get_info(int drive, uint64_t* sector_count, uint32_t* sector_size);

/* 列出所有块设备 */
void blk_list_all(void);

/* NVMe 驱动 */
int blk_nvme_init(void);
int blk_nvme_probe(void);

/* AHCI 驱动 */
int blk_ahci_init(void);
int blk_ahci_probe(void);

/* VirtIO-blk 驱动 */
int blk_virtio_init(void);
int blk_virtio_probe(void);

/* ATA 驱动 (legacy) */
int blk_ata_init(void);
int blk_ata_probe(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_BLOCK_H */