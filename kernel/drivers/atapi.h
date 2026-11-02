/* SutureOS module: ATAPI (ATA Packet Interface) CD-ROM driver - 光驱块设备
 *
 * 来源 / Source: Uinxed-Kernel, drivers/block/ata/pata/atapi.c
 *               (C:\Users\fanqi\Desktop\自研操作系统\Uinxed-Kernel)
 *               许可证 / License: Apache-2.0
 *               Copyright (C) 2020 ViudiraTech, 2026 MicroFish
 * 改动 / Changes: 只在“协议流程”层面参考（IDENTIFY PACKET -> PACKET ->
 *               READ(10) -> READ CAPACITY -> REQUEST SENSE 的时序与错误处理），
 *               代码按 SutureOS 现有风格重写：纯轮询（无 IRQ、无 nsleep）、
 *               所有等待有界、接入 kernel/drivers/block.h 的 blk_device 接口，
 *               扇区单位固定 2048 字节（CD-ROM 用户数据区），只读。
 *
 * 用法：开机在 blk_ata_probe() 之后调用 blk_atapi_probe()；它扫描两个 IDE
 * 通道的 4 个位置，把真实存在的 ATAPI 光盘注册成块设备（驱动号 12..15），
 * 之后文件系统层只用 blk_read(drive, 2048 字节块号, ...) 读取，不依赖本头文件。
 * 没有光驱时不注册任何设备，也不打印任何日志（开机日志保持原样）。
 */
#ifndef STITCH_ATAPI_H
#define STITCH_ATAPI_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ATAPI 返回码（与 ext2_* 一样用负值表示错误） */
#define ATAPI_OK         0
#define ATAPI_EIO       (-1)   /* 设备报了 ERR/DF，或数据方向/阶段不合法 */
#define ATAPI_ETIMEOUT  (-2)   /* 轮询超时（设备挂着不回状态） */
#define ATAPI_ENODEV    (-3)   /* 该位置没有设备（总线浮空 0x00/0xFF） */
#define ATAPI_EINVAL    (-4)   /* 参数越界 / 未支持的块大小 */

/* 扫描主/次 IDE 通道的 4 个位置（主主、主从、次主、次从），把 ATAPI 光盘
 * 注册进块设备层（驱动号 BLK_DRIVE_ATAPI_BASE + 位置号）。
 * 返回注册成功的设备数（>= 0）。幂等：重复调用不会重复注册。 */
int blk_atapi_probe(void);

/* 已注册的 ATAPI 光盘数量 / 第 index 个的块设备驱动号（越界返回 -1）。 */
int blk_atapi_device_count(void);
int blk_atapi_drive_at(int index);

/* 位置号（0..3）与块设备驱动号（12..15）互换，非法返回 -1。 */
int atapi_drive_of_position(int pos);
int atapi_position_of_drive(int drive);

/* 底层 ATAPI 命令（位置号 0..3）。一般只需要 blk_read()。 */
int atapi_test_unit_ready(int pos);
int atapi_read_capacity(int pos, uint32_t *last_lba, uint32_t *block_size);
int atapi_request_sense(int pos, uint8_t *sense_key, uint8_t *asc, uint8_t *ascq);
/* 按 2048 字节块读，count 块整块读入 buf。成功返回 ATAPI_OK。 */
int atapi_read_blocks(int pos, uint32_t lba, uint32_t count, void *buf);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_ATAPI_H */
