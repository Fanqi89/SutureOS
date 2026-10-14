/* SutureOS module: Block device layer - 统一块设备接口
 * Source: 基于 SpiritFoxOS AHCI、Cinux NVMe、VimtuOS VirtIO-blk 移植;
 *         ATA PIO 驱动 Ported from Lemis pio/ata.c
 *         (C:\Users\fanqi\Desktop\自研操作系统\Lemis)
 * Original licenses: GPL-3.0 (SpiritFoxOS), MIT (Cinux), GPL-3.0 (VimtuOS),
 *         GPL-3.0 (Lemis - 按本树 heap.c/heap.h 早前移植记录)
 * Changes: Adapted to SutureOS; prefix blk_; unified IBlockDevice interface
 */
#include "block.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/io.h>
#include "heap.h"

/* ==================== 全局状态 ==================== */
struct blk_device* g_blk_devices = NULL;

/* ==================== RAM 磁盘 (ram0, 驱动器号 9) ====================
 * 1 MiB，由 lemis_heap_alloc 提供；分配失败只记日志、不注册（不阻塞启动）。
 * 读写 = 带边界检查的 memcpy；flush = 无操作（内存盘无需刷缓存）。
 */
#define RAMDISK_SECTOR_SIZE  512u
#define RAMDISK_SECTORS      2048u                                /* 2048 * 512 = 1 MiB */
#define RAMDISK_BYTES        ((uintptr_t)RAMDISK_SECTORS * RAMDISK_SECTOR_SIZE)

static void* g_ramdisk_buf = NULL;

static int ramdisk_read(struct blk_device* dev, uint64_t lba, uint32_t count, void* buf) {
    (void)dev;
    if (!g_ramdisk_buf || !buf) return -1;
    if (count == 0) return 0;
    if (lba >= RAMDISK_SECTORS || (uint64_t)count > (uint64_t)RAMDISK_SECTORS - lba) {
        console_printf("[blk] ram0 read out of range lba=%llu count=%u\n", lba, count);
        return -1;
    }
    memcpy(buf, (const uint8_t*)g_ramdisk_buf + (uintptr_t)lba * RAMDISK_SECTOR_SIZE,
           (uintptr_t)count * RAMDISK_SECTOR_SIZE);
    return 0;
}

static int ramdisk_write(struct blk_device* dev, uint64_t lba, uint32_t count, const void* buf) {
    (void)dev;
    if (!g_ramdisk_buf || !buf) return -1;
    if (count == 0) return 0;
    if (lba >= RAMDISK_SECTORS || (uint64_t)count > (uint64_t)RAMDISK_SECTORS - lba) {
        console_printf("[blk] ram0 write out of range lba=%llu count=%u\n", lba, count);
        return -1;
    }
    memcpy((uint8_t*)g_ramdisk_buf + (uintptr_t)lba * RAMDISK_SECTOR_SIZE, buf,
           (uintptr_t)count * RAMDISK_SECTOR_SIZE);
    return 0;
}

static int ramdisk_flush(struct blk_device* dev) {
    (void)dev;
    return 0;   /* 内存盘：数据已在内存，无需刷新 */
}

static const struct blk_ops g_ramdisk_ops = {
    .read  = ramdisk_read,
    .write = ramdisk_write,
    .flush = ramdisk_flush,
    .ioctl = NULL,
};

static struct blk_device g_ramdisk_dev = {
    .name         = "ram0",
    .type         = BLK_TYPE_RAM,
    .drive        = BLK_DRIVE_RAM,
    .flags        = 0,
    .sector_count = RAMDISK_SECTORS,
    .sector_size  = RAMDISK_SECTOR_SIZE,
    .block_size   = RAMDISK_SECTOR_SIZE,
    .ops          = &g_ramdisk_ops,
    .driver_data  = NULL,
    .next         = NULL,
};

/* ==================== 块设备层实现 ==================== */

int blk_init(void) {
    console_puts("[blk] Block device layer init\n");
    g_blk_devices = NULL;

    /* RAM 磁盘：1 MiB，失败则如实记录并继续（后续 ATA/其他设备照常注册） */
    g_ramdisk_buf = lemis_heap_alloc(RAMDISK_BYTES);
    if (g_ramdisk_buf) {
        memset(g_ramdisk_buf, 0, RAMDISK_BYTES);
        blk_register(&g_ramdisk_dev);
    } else {
        console_printf("[blk] ram0: heap alloc failed (%llu bytes), ramdisk skipped (free heap=%llu)\n",
                       (uint64_t)RAMDISK_BYTES, (uint64_t)lemis_heap_free_space());
    }
    return 0;
}

int blk_register(struct blk_device* dev) {
    if (!dev) return -1;
    
    /* 幂等保护：同一设备重复注册直接返回，避免链表成环 */
    if (blk_find_by_drive(dev->drive) == dev) return 0;

    /* 驱动器号冲突告警：blk_find_by_drive() 只返回最后注册的那个，另一个会
     * 变成"幽灵设备"（能被 ls 列出来但读不到），mount 失败时报的错也毫无线索。
     * 全树分配表见 block.h；这里只告警不拒绝，避免把历史行为变成硬失败。 */
    {
        struct blk_device* clash = blk_find_by_drive(dev->drive);
        if (clash)
            console_printf("[blk] WARNING: drive %d already taken by '%s', "
                           "'%s' shadows it (see the drive map in block.h)\n",
                           dev->drive, clash->name ? clash->name : "?",
                           dev->name ? dev->name : "?");
    }

    dev->next = g_blk_devices;
    g_blk_devices = dev;
    
    console_printf("[blk] Registered device: %s (type=%d, drive=%d)\n", 
                   dev->name ? dev->name : "unknown", dev->type, dev->drive);
    return 0;
}

int blk_unregister(struct blk_device* dev) {
    if (!dev) return -1;
    
    struct blk_device** curr = &g_blk_devices;
    while (*curr) {
        if (*curr == dev) {
            *curr = dev->next;
            dev->next = NULL;
            console_printf("[blk] Unregistered device: %s\n", dev->name ? dev->name : "unknown");
            return 0;
        }
        curr = &(*curr)->next;
    }
    return -1;
}

struct blk_device* blk_find_by_drive(int drive) {
    struct blk_device* dev = g_blk_devices;
    while (dev) {
        if (dev->drive == drive) return dev;
        dev = dev->next;
    }
    return NULL;
}

struct blk_device* blk_find_by_name(const char* name) {
    struct blk_device* dev = g_blk_devices;
    while (dev) {
        if (dev->name && strcmp(dev->name, name) == 0) return dev;
        dev = dev->next;
    }
    return NULL;
}

int blk_read(int drive, uint64_t lba, uint32_t count, void* buf) {
    struct blk_device* dev = blk_find_by_drive(drive);
    if (!dev || !dev->ops || !dev->ops->read) {
        console_printf("[blk] Read failed: no device or no read op (drive=%d)\n", drive);
        return -1;
    }
    return dev->ops->read(dev, lba, count, buf);
}

int blk_write(int drive, uint64_t lba, uint32_t count, const void* buf) {
    struct blk_device* dev = blk_find_by_drive(drive);
    if (!dev || !dev->ops || !dev->ops->write) {
        console_printf("[blk] Write failed: no device or no write op (drive=%d)\n", drive);
        return -1;
    }
    if (dev->flags & BLK_FLAG_READONLY) {
        console_printf("[blk] Write failed: device is read-only (drive=%d)\n", drive);
        return -1;
    }
    return dev->ops->write(dev, lba, count, buf);
}

int blk_flush(int drive) {
    struct blk_device* dev = blk_find_by_drive(drive);
    if (!dev || !dev->ops || !dev->ops->flush) return 0;
    return dev->ops->flush(dev);
}

int blk_get_info(int drive, uint64_t* sector_count, uint32_t* sector_size) {
    struct blk_device* dev = blk_find_by_drive(drive);
    if (!dev) return -1;
    if (sector_count) *sector_count = dev->sector_count;
    if (sector_size) *sector_size = dev->sector_size;
    return 0;
}

void blk_list_all(void) {
    struct blk_device* dev = g_blk_devices;
    console_puts("[blk] Block devices:\n");
    while (dev) {
        console_printf("  drive=%d type=%d name=%s sectors=%llu size=%u flags=0x%x\n",
                       dev->drive, dev->type, dev->name ? dev->name : "unnamed",
                       dev->sector_count, dev->sector_size, dev->flags);
        dev = dev->next;
    }
}

/* ==================== 独立驱动实现已不在本文件 ====================
 * NVMe / AHCI / VirtIO-blk 的 blk_*_init() / blk_*_probe() 曾经是这里的存根；
 * 现在三个驱动都是真实实现，分别位于：
 *   kernel/drivers/nvme.c       -> blk_nvme_init()   / blk_nvme_probe()
 *   kernel/drivers/ahci.c       -> blk_ahci_init()   / blk_ahci_probe()
 *   kernel/drivers/virtio_blk.c -> blk_virtio_init() / blk_virtio_probe()
 * 原型仍在 block.h。存根若保留会与真实实现重复定义导致链接失败，已删除。
 */

/* ==================== ATA PIO 驱动 (legacy PATA) ====================
 * Ported from Lemis pio/ata.c (C:\Users\fanqi\Desktop\自研操作系统\Lemis)
 * Original license: GPL-3.0（按本树 heap.c 对 Lemis 的移植记录；原仓库未随源码附许可证文件）
 * Changes: Lemis 的 ata_init/ata_read_sector/ata_write_sector 移植为
 *          blk_ata_init() 控制器级设置 + blk_ata_probe() IDENTIFY 探测注册；
 *          新增：多扇区按 256 扇区/命令分块（扇区计数寄存器 8 位，0=256）、
 *          所有等待均有界（~1e6 次轮询，绝不死循环，超时打 [blk] 日志并返回 -1）、
 *          LBA28 范围检查、IDENTIFY 容量 (word 60-61 / 100-101)、
 *          ATAPI 签名 (0x14 0xEB) 与 ERR 跳过、写后 CACHE FLUSH (0xE7)。
 *          选盘字节必须用 0xE0（LBA 模式）——0xA0 是 CHS，会让 READ/WRITE 被 ABRT 拒绝。
 * 中断：本驱动纯轮询。blk_ata_init() 把设备控制寄存器 nIEN 置 1，
 *       屏蔽设备中断输出（SutureOS 目前没有 IDE IRQ14/15 处理程序）。
 * 端口：主通道 0x1F0/0x3F6 = 驱动器号 0(主)/1(从)；次通道 0x170/0x376 保留给
 *       驱动器号 2/3（本版本只探测主通道 0/1）。
 */

/* ---- 端口基址 ---- */
#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6    /* alternate status (读) / device control (写) */
#define ATA_SECONDARY_IO    0x170
#define ATA_SECONDARY_CTRL  0x376

/* ---- 寄存器偏移（相对数据口 0x1F0/0x170） ---- */
#define ATA_REG_DATA        0x0   /* 数据口 (16 位) */
#define ATA_REG_ERROR       0x1   /* 读: 错误 / 写: 特性 */
#define ATA_REG_SECCOUNT    0x2   /* 扇区计数 (8 位, 0 = 256) */
#define ATA_REG_LBA0        0x3   /* LBA 位 0-7 */
#define ATA_REG_LBA1        0x4   /* LBA 位 8-15 / IDENTIFY 签名低字节 */
#define ATA_REG_LBA2        0x5   /* LBA 位 16-23 / IDENTIFY 签名高字节 */
#define ATA_REG_HDDEVSEL    0x6   /* 驱动器选择 */
#define ATA_REG_COMMAND     0x7   /* 写: 命令 */
#define ATA_REG_STATUS      0x7   /* 读: 主状态 */

/* ---- 命令 ---- */
#define ATA_CMD_IDENTIFY     0xEC
#define ATA_CMD_READ_PIO     0x20
#define ATA_CMD_WRITE_PIO    0x30
#define ATA_CMD_CACHE_FLUSH  0xE7

/* ---- 主状态寄存器位 ---- */
#define ATA_SR_ERR   0x01          /* 错误 */
#define ATA_SR_DRQ   0x08          /* 数据就绪 */
#define ATA_SR_DF    0x20          /* 设备故障 */
#define ATA_SR_BSY   0x80          /* 忙 */

/* ---- 超时与范围 ---- */
#define ATA_POLL_LIMIT          1000000u        /* 每次等待最多轮询 ~1e6 次 */
#define ATA_LBA28_LIMIT         0x10000000ULL   /* LBA28 可寻址 0..0x0FFFFFFF */
#define ATA_MAX_SECTORS_PER_CMD 256u            /* 单条命令最多 256 扇区 */

static int g_ata_init_done = 0;   /* blk_ata_init() 幂等标记 */

static uint16_t ata_io_base(int drive)   { return (uint16_t)((drive & 2) ? ATA_SECONDARY_IO   : ATA_PRIMARY_IO); }
static uint16_t ata_ctrl_base(int drive) { return (uint16_t)((drive & 2) ? ATA_SECONDARY_CTRL : ATA_PRIMARY_CTRL); }

/* 选盘字节：0xE0 = LBA 模式 | 从盘位 | LBA 高 4 位 */
static uint8_t ata_sel_byte(int drive, uint32_t lba) {
    return (uint8_t)(0xE0u | ((uint32_t)(drive & 1) << 4) | ((lba >> 24) & 0x0Fu));
}

/* 约 400ns 延时：连读 4 次交替状态寄存器（只读，无副作用、不清中断） */
static void ata_delay(uint16_t ctrl) {
    for (int i = 0; i < 4; i++) (void)inb(ctrl);
}

/* 等 BSY 清零：0=就绪, -1=总线浮空(0x00/0xFF，无设备), -2=超时 */
static int ata_wait_ready(uint16_t io) {
    for (uint32_t i = 0; i < ATA_POLL_LIMIT; i++) {
        uint8_t st = inb((uint16_t)(io + ATA_REG_STATUS));
        if (st == 0x00 || st == 0xFF) return -1;
        if (!(st & ATA_SR_BSY)) return 0;
    }
    return -2;
}

/* 等 DRQ：0=数据就绪, -1=设备错误(ERR/DF)或消失, -2=超时 */
static int ata_wait_drq(uint16_t io) {
    for (uint32_t i = 0; i < ATA_POLL_LIMIT; i++) {
        uint8_t st = inb((uint16_t)(io + ATA_REG_STATUS));
        if (st & (ATA_SR_ERR | ATA_SR_DF)) return -1;
        if (st & ATA_SR_DRQ) return 0;
        if (st == 0x00 || st == 0xFF) return -1;
    }
    return -2;
}

/* 等命令完成 (BSY=0 且 DRQ=0)：0=完成, -1=错误/设备消失, -2=超时 */
static int ata_wait_done(uint16_t io) {
    for (uint32_t i = 0; i < ATA_POLL_LIMIT; i++) {
        uint8_t st = inb((uint16_t)(io + ATA_REG_STATUS));
        if (st == 0x00 || st == 0xFF) return -1;
        if (st & (ATA_SR_ERR | ATA_SR_DF)) return -1;
        if (!(st & ATA_SR_BSY) && !(st & ATA_SR_DRQ)) return 0;
    }
    return -2;
}

/* 单条 PIO 命令（LBA28，count = 1..256 扇区）。
 * 每扇区先等 DRQ，再经数据口搬 256 个 16 位字（stitch/io.h 只有 inw/outw，无 insw）。
 * 返回 0=成功, -1=设备错误, -2=超时（调用方打带 lba 的日志）。 */
static int ata_pio_xfer(struct blk_device* dev, uint32_t lba, uint32_t count,
                        void* buf, int is_write)
{
    const uint16_t io   = ata_io_base(dev->drive);
    const uint16_t ctrl = ata_ctrl_base(dev->drive);

    /* 必须先选盘再等状态：状态寄存器反映的是当前选中的驱动器 */
    outb((uint16_t)(io + ATA_REG_HDDEVSEL), ata_sel_byte(dev->drive, lba));
    ata_delay(ctrl);
    int r = ata_wait_ready(io);
    if (r != 0) return r;

    outb((uint16_t)(io + ATA_REG_ERROR), 0);                       /* 特性寄存器清零 */
    outb((uint16_t)(io + ATA_REG_SECCOUNT), (uint8_t)(count & 0xFF)); /* 0 即 256 */
    outb((uint16_t)(io + ATA_REG_LBA0), (uint8_t)(lba & 0xFF));
    outb((uint16_t)(io + ATA_REG_LBA1), (uint8_t)((lba >> 8) & 0xFF));
    outb((uint16_t)(io + ATA_REG_LBA2), (uint8_t)((lba >> 16) & 0xFF));
    outb((uint16_t)(io + ATA_REG_COMMAND), is_write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO);

    uint8_t* p = (uint8_t*)buf;
    for (uint32_t s = 0; s < count; s++) {
        r = ata_wait_drq(io);
        if (r != 0) return r;
        uint16_t* w = (uint16_t*)(p + (uintptr_t)s * 512u);
        if (is_write) {
            for (int i = 0; i < 256; i++) outw(io, w[i]);
        } else {
            for (int i = 0; i < 256; i++) w[i] = inw(io);
        }
    }
    return ata_wait_done(io);
}

/* 刷新设备写缓存：CACHE FLUSH (0xE7) + 等命令完成 */
static int ata_flush_dev(struct blk_device* dev) {
    const uint16_t io   = ata_io_base(dev->drive);
    const uint16_t ctrl = ata_ctrl_base(dev->drive);
    outb((uint16_t)(io + ATA_REG_HDDEVSEL), ata_sel_byte(dev->drive, 0));
    ata_delay(ctrl);
    if (ata_wait_ready(io) != 0) {
        console_printf("[blk] ata flush not ready drive=%d\n", dev->drive);
        return -1;
    }
    outb((uint16_t)(io + ATA_REG_COMMAND), ATA_CMD_CACHE_FLUSH);
    int r = ata_wait_done(io);
    if (r != 0) {
        console_printf("[blk] ata flush %s drive=%d\n", (r == -2) ? "timeout" : "error", dev->drive);
        return -1;
    }
    return 0;
}

/* 读块：LBA28，>256 扇区按 256/条分块 */
static int ata_read(struct blk_device* dev, uint64_t lba, uint32_t count, void* buf) {
    if (!dev || !buf) return -1;
    if (count == 0) return 0;
    if (lba >= ATA_LBA28_LIMIT || (uint64_t)count > ATA_LBA28_LIMIT - lba) {
        console_printf("[blk] ata read out of LBA28 range lba=%llu count=%u\n", lba, count);
        return -1;
    }
    if (dev->sector_count != 0 && (lba + count) > dev->sector_count) {
        console_printf("[blk] ata read beyond capacity lba=%llu count=%u sectors=%llu\n",
                       lba, count, dev->sector_count);
        return -1;
    }
    uint8_t* p = (uint8_t*)buf;
    uint64_t done = 0;
    while (done < count) {
        uint32_t n = (uint32_t)((count - done) > ATA_MAX_SECTORS_PER_CMD
                                ? ATA_MAX_SECTORS_PER_CMD : (count - done));
        int r = ata_pio_xfer(dev, (uint32_t)(lba + done), n, p + done * 512u, 0);
        if (r != 0) {
            if (r == -2) {
                console_printf("[blk] ata read timeout lba=%llu count=%u drive=%d\n",
                               lba + done, n, dev->drive);
            } else {
                console_printf("[blk] ata read error lba=%llu count=%u drive=%d status=0x%x\n",
                               lba + done, n, dev->drive,
                               inb((uint16_t)(ata_io_base(dev->drive) + ATA_REG_STATUS)));
            }
            return -1;
        }
        done += n;
    }
    return 0;
}

/* 写块：LBA28，>256 扇区按 256/条分块；整块写完后发 CACHE FLUSH (0xE7) */
static int ata_write(struct blk_device* dev, uint64_t lba, uint32_t count, const void* buf) {
    if (!dev || !buf) return -1;
    if (count == 0) return 0;
    if (lba >= ATA_LBA28_LIMIT || (uint64_t)count > ATA_LBA28_LIMIT - lba) {
        console_printf("[blk] ata write out of LBA28 range lba=%llu count=%u\n", lba, count);
        return -1;
    }
    if (dev->sector_count != 0 && (lba + count) > dev->sector_count) {
        console_printf("[blk] ata write beyond capacity lba=%llu count=%u sectors=%llu\n",
                       lba, count, dev->sector_count);
        return -1;
    }
    const uint8_t* p = (const uint8_t*)buf;
    uint64_t done = 0;
    while (done < count) {
        uint32_t n = (uint32_t)((count - done) > ATA_MAX_SECTORS_PER_CMD
                                ? ATA_MAX_SECTORS_PER_CMD : (count - done));
        int r = ata_pio_xfer(dev, (uint32_t)(lba + done), n, (void*)(p + done * 512u), 1);
        if (r != 0) {
            if (r == -2) {
                console_printf("[blk] ata write timeout lba=%llu count=%u drive=%d\n",
                               lba + done, n, dev->drive);
            } else {
                console_printf("[blk] ata write error lba=%llu count=%u drive=%d status=0x%x\n",
                               lba + done, n, dev->drive,
                               inb((uint16_t)(ata_io_base(dev->drive) + ATA_REG_STATUS)));
            }
            return -1;
        }
        done += n;
    }
    /* 数据已全部交给设备缓存，刷盘；失败只告警（传输本身已成功） */
    if (ata_flush_dev(dev) != 0) {
        console_printf("[blk] ata flush after write failed drive=%d (data may still be cached)\n",
                       dev->drive);
    }
    return 0;
}

static const struct blk_ops g_ata_ops = {
    .read  = ata_read,
    .write = ata_write,
    .flush = ata_flush_dev,
    .ioctl = NULL,
};

static struct blk_device g_ata_devs[2];   /* 驱动器号 0=主, 1=从 */

/* IDENTIFY DEVICE (0xEC)：0=ATA 硬盘(sectors_out 已填), 1=该位无设备,
 *  2=非 ATA 设备(ATAPI/命令被拒), -1=超时或错误 */
static int ata_identify(int drive, uint64_t* sectors_out) {
    const uint16_t io   = ata_io_base(drive);
    const uint16_t ctrl = ata_ctrl_base(drive);

    /* 1. 选盘 + 400ns 延时；状态 0x00/0xFF = 该位无驱动器
     *    （QEMU 只用 -cdrom 启动时常出现空主盘，跳过即可） */
    outb((uint16_t)(io + ATA_REG_HDDEVSEL), ata_sel_byte(drive, 0));
    ata_delay(ctrl);
    uint8_t st = inb((uint16_t)(io + ATA_REG_STATUS));
    if (st == 0x00 || st == 0xFF) return 1;
    int r = ata_wait_ready(io);
    if (r == -1) return 1;
    if (r != 0) return -1;

    /* 2. 发 IDENTIFY DEVICE */
    outb((uint16_t)(io + ATA_REG_SECCOUNT), 0);
    outb((uint16_t)(io + ATA_REG_LBA0), 0);
    outb((uint16_t)(io + ATA_REG_LBA1), 0);
    outb((uint16_t)(io + ATA_REG_LBA2), 0);
    outb((uint16_t)(io + ATA_REG_COMMAND), ATA_CMD_IDENTIFY);
    st = inb((uint16_t)(io + ATA_REG_STATUS));
    if (st == 0x00 || st == 0xFF) return 1;
    if (ata_wait_ready(io) != 0) return -1;

    /* 3. ATAPI 签名 (LBA1/LBA2 = 0x14 0xEB) => 不是本驱动支持的 ATA 硬盘 */
    if (inb((uint16_t)(io + ATA_REG_LBA1)) == 0x14 &&
        inb((uint16_t)(io + ATA_REG_LBA2)) == 0xEB) return 2;

    /* 命令被设备拒绝 (ERR/DF) => 也不是 ATA 硬盘 */
    st = inb((uint16_t)(io + ATA_REG_STATUS));
    if (st & (ATA_SR_ERR | ATA_SR_DF)) return 2;

    /* 4. 等 DRQ，读 256 字 IDENTIFY 数据（必须读完，否则设备传输未结束） */
    if (ata_wait_drq(io) != 0) return -1;
    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(io);

    /* 5. 容量：word 60-61 (LBA28)；为 0 再看 word 100-101 (LBA48 低 32 位)。
     *    扇区大小固定 512 字节（本驱动 PIO LBA28）。 */
    uint64_t sec = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    if (sec == 0) sec = (uint64_t)id[100] | ((uint64_t)id[101] << 16);
    if (sectors_out) *sectors_out = sec;
    return 0;
}

int blk_ata_init(void) {
    /* 控制器级设置：本驱动纯轮询，把两个通道的 nIEN 置 1，屏蔽设备中断输出，
     * 避免无人处理的 IRQ14/IRQ15 在电平触发下反复进入（SutureOS 尚无 IDE 中断处理程序）。*/
    outb(ATA_PRIMARY_CTRL, 0x02);
    outb(ATA_SECONDARY_CTRL, 0x02);
    console_puts("[blk] ATA driver init: PIO polling, primary 0x1F0/0x3F6 (nIEN=1)\n");
    g_ata_init_done = 1;
    return 0;
}

int blk_ata_probe(void) {
    if (!g_ata_init_done) blk_ata_init();   /* 容错：调用方忘了 init 时先补上控制器设置 */
    console_puts("[blk] ATA probe: primary master/slave (drive 0/1)\n");
    for (int d = 0; d <= 1; d++) {
        if (blk_find_by_drive(d)) {
            console_printf("[blk] ata%d: drive number already registered, skip\n", d);
            continue;
        }
        uint64_t sectors = 0;
        int r = ata_identify(d, &sectors);
        if (r == 1) { console_printf("[blk] ata%d: no drive\n", d); continue; }
        if (r == 2) { console_printf("[blk] ata%d: not an ATA device (ATAPI/unknown), skip\n", d); continue; }
        if (r != 0) { console_printf("[blk] ata%d: identify failed, skip\n", d); continue; }

        struct blk_device* dev = &g_ata_devs[d];
        dev->name         = (d == 0) ? "ata0" : "ata1";
        dev->type         = BLK_TYPE_ATA;
        dev->drive        = d;
        dev->flags        = 0;
        dev->sector_count = sectors;
        dev->sector_size  = 512;
        dev->block_size   = 512;
        dev->ops          = &g_ata_ops;
        dev->driver_data  = NULL;
        if (sectors == 0) console_printf("[blk] ata%d: capacity unknown (0 sectors)\n", d);
        blk_register(dev);
        console_printf("[blk] ata%d: PIO LBA28, %llu sectors x 512B\n", d, sectors);
    }
    return 0;
}