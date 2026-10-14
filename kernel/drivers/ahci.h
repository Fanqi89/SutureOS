/* SutureOS module: AHCI (SATA) block driver - 对外接口
 * Ported from: VimtuOS kernel/ahci64.cpp (GPL-3.0, 与本项目同许可证, 782 行)
 * Register/FIS/command-list layout cross-checked against:
 *   - PlantOS src/kernel/drivers/storage/ahci.c (435 行) 与
 *     PlantOS src/loader/driver/ahci.c
 *   - XJ380 driver/ahci/ahci.cpp (511 行)
 *   - AHCI 1.3.1 specification (Intel)
 * Changes relative to the sources: freestanding C11 适配；不再自带 vdisk 层，
 * 改为注册进 SutureOS 的 struct blk_device（block.h）；DMA 内存全部是 .bss
 * 静态缓冲（不碰内核堆，见 ahci.c 顶部说明）；纯轮询（PxIE = 0）。
 */
#ifndef STITCH_AHCI_H
#define STITCH_AHCI_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 同时管理的 SATA 盘上限（静态 DMA 缓冲按这个上限分配，见 ahci.c）
 * 必须是 4：驱动器号 = AHCI_DRIVE_BASE + 序号，而 4..7 是本项目留给 AHCI 的
 * 唯一无冲突区间（NVMe=8、ram0=9、VirtIO-blk=10，见 block.h 的驱动器号表）。 */
#define AHCI_MAX_DEVS   4
/* 驱动器号基址：ata0/ata1 = 0/1、ram0 = 9；AHCI 盘用 4,5,6,7 */
#define AHCI_DRIVE_BASE 4

/* blk_ahci_init(): PCI 找到 AHCI 控制器 -> ABAR 恒等映射 -> HBA 复位/AHCI 模式
 * -> 每个在位端口（SSTS.DET == 3）建命令列表/FIS/命令表、启端口、IDENTIFY DEVICE
 * (0xEC) -> 按 sata0/sata1... 注册 struct blk_device。
 * 返回 0 = 成功（没有盘也算成功），-1 = 没有 AHCI 控制器或 BAR5 不可用。幂等。 */
int blk_ahci_init(void);

/* blk_ahci_probe(): 走同一条探测路径但**不注册**，返回发现的盘数；
 * 返回 -1 = 没有控制器（与 init 相同的 [ahci] no AHCI controller on PCI 日志）。
 * 幂等：init 之后再调用直接返回 init 的结果。 */
int blk_ahci_probe(void);

/* 最近一次探测/初始化发现的盘数（诊断用，0 = 无盘/未探测） */
int blk_ahci_device_count(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_AHCI_H */
