/* SutureOS module: Block device layer - 统一块设备接口
 * Source: 基于 SpiritFoxOS AHCI、Cinux NVMe、VimtuOS VirtIO-blk 移植
 * Original licenses: GPL-3.0 (SpiritFoxOS), MIT (Cinux), GPL-3.0 (VimtuOS)
 * Changes: Adapted to SutureOS; prefix blk_; unified IBlockDevice interface
 */
#include "block.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"

/* ==================== 全局状态 ==================== */
struct blk_device* g_blk_devices = NULL;

/* ==================== 块设备层实现 ==================== */

int blk_init(void) {
    console_puts("[blk] Block device layer init\n");
    g_blk_devices = NULL;
    return 0;
}

int blk_register(struct blk_device* dev) {
    if (!dev) return -1;
    
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

/* ==================== 驱动存根实现 ==================== */

/* NVMe 驱动 */
int blk_nvme_init(void) {
    console_puts("[blk] NVMe driver init (stub)\n");
    return 0;
}

int blk_nvme_probe(void) {
    console_puts("[blk] NVMe probe (stub)\n");
    return 0;
}

/* AHCI 驱动 */
int blk_ahci_init(void) {
    console_puts("[blk] AHCI driver init (stub)\n");
    return 0;
}

int blk_ahci_probe(void) {
    console_puts("[blk] AHCI probe (stub)\n");
    return 0;
}

/* VirtIO-blk 驱动 */
int blk_virtio_init(void) {
    console_puts("[blk] VirtIO-blk driver init (stub)\n");
    return 0;
}

int blk_virtio_probe(void) {
    console_puts("[blk] VirtIO-blk probe (stub)\n");
    return 0;
}

/* ATA 驱动 (legacy) */
int blk_ata_init(void) {
    console_puts("[blk] ATA driver init (stub)\n");
    return 0;
}

int blk_ata_probe(void) {
    console_puts("[blk] ATA probe (stub)\n");
    return 0;
}