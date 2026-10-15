/* SutureOS module: AHCI (SATA) block driver - 真实 AHCI 1.3 主机控制器驱动
 *
 * Ported from: VimtuOS kernel/ahci64.cpp (C:\Users\fanqi\Desktop\自研操作系统\
 *              VimtuOS\kernel\ahci64.cpp, 782 行), GPL-3.0 - 与本项目同许可证。
 * Register/FIS/命令列表布局交叉对照：
 *   - PlantOS src/kernel/drivers/storage/ahci.c (435 行) 与
 *     PlantOS src/loader/driver/ahci.c;
 *   - XJ380 driver/ahci/ahci.cpp (511 行);
 *   - AHCI 1.3.1 规范 (Intel) 的 HBA/端口寄存器、H2D Register FIS、命令头、
 *     PRDT 布局。
 *
 * 相对上游的改动（本文件是 SutureOS 自己的 freestanding C 适配）：
 *   - 不再自带 vdisk 层：注册进 SutureOS 的 struct blk_device（block.h），
 *     驱动器号 4,5,6,...（ata0/ata1 = 0/1、ram0 = 9），名字 sata0,sata1,...；
 *   - 无 kmalloc/页池：命令列表(1 KiB@1 KiB)/FIS 接收区(256 B@256 B)/
 *     命令表(512 B@128 B)/DMA 暂存(256 KiB@4 KiB) 全是 .bss 静态缓冲。
 *     本内核把低 1 GiB 恒等映射，所以这些缓冲的**虚拟地址 == 物理地址**，
 *     直接写进 PxCLB/PxCLBU/PxFB/PxFBU 与 PRDT.DBA 是合法的（见下面 DMA 一节）；
 *   - 纯轮询：PxIE = 0，所有等待都有自旋上限（绝不死循环）；自旋到一定次数后
 *     若 IF=1 就 hlt 让出（QEMU 的部分设备状态机挂在宿主定时器/bottom half 上，
 *     死转不肯让出会永远看不到完成 —— VimtuOS 踩坑 E）；
 *   - 单命令单 PRDT 项：一次命令最多 512 扇区（256 KiB，正好等于 DMA 暂存缓冲），
 *     大请求在驱动内按 256 KiB 分块循环；
 *   - PRDT/命令头的字段偏移按 VimtuOS 的实测结论写（见下面 CMDHDR/PRDT 注释）：
 *     命令头 DW0 的 bits[31:16] 才是 PRDTL，DW2/DW3 才是 CTBA/CTBAU；
 *     PRDT 的 DBC 在 +0x0C（+0x08 是保留 DWORD）。写错任何一处 HBA 会**静默
 *     丢弃整条命令**（PxCI 挂着不动）；
 *   - 不清 PxIS/PxSERR 的保留位（不写 0xFFFFFFFF），只清规范定义的 RW1C 位；
 *     不写 PxCMD.ICC，端口启动只用读-改-写 OR 进 FRE/ST（SeaBIOS/libata 写法）。
 *
 * DMA 与缓存一致性假设（本内核不管理 MTRR、页表是 WB 缓存）：
 *   QEMU 的 AHCI 模型与 CPU 缓存是一致的，但真机不是。所以：
 *     * 发命令前 clflush 命令列表/命令表/PRDT/（写请求的）数据缓冲；
 *     * DMA 读完成后 clflush 数据缓冲再把数据交给调用方；
 *   clflush 在 QEMU 上是无害的，在真机上是必需的。绝不假设 in-flight DMA
 *   与 CPU 缓存自动一致。
 */
#include "ahci.h"
#include "block.h"

#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/io.h>
#include "mmu.h"

/* ===================== PCI 配置空间 (mechanism #1) =====================
 * 与 rtl8139.c 同一套做法：stitch/io.h 没有 outl/inl，而且 0xCF8 必须用
 * DWORD 访问 —— 逐字节写 0xCF8 在 QEMU 上不生效（所有槽位读回同一个常量）。 */
#define PCI_CONFIG_ADDR 0xCF8u
#define PCI_CONFIG_DATA 0xCFCu

#define PCI_CFG_VENDOR_ID 0x00u
#define PCI_CFG_COMMAND   0x04u
#define PCI_CFG_REV_CLASS 0x08u
#define PCI_CFG_HEADER    0x0Cu
#define PCI_CFG_BAR5      0x24u

#define PCI_CMD_IO_SPACE   (1u << 0)
#define PCI_CMD_MEM_SPACE  (1u << 1)
#define PCI_CMD_BUS_MASTER (1u << 2)

static inline void ahci_pci_outl(uint16_t port, uint32_t v)
{
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t ahci_pci_inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint32_t ahci_pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    const uint32_t addr = 0x80000000u
                        | ((uint32_t)bus << 16)
                        | ((uint32_t)dev << 11)
                        | ((uint32_t)fn << 8)
                        | (uint32_t)(off & 0xFCu);
    ahci_pci_outl(PCI_CONFIG_ADDR, addr);
    return ahci_pci_inl(PCI_CONFIG_DATA);
}

static void ahci_pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    const uint32_t addr = 0x80000000u
                        | ((uint32_t)bus << 16)
                        | ((uint32_t)dev << 11)
                        | ((uint32_t)fn << 8)
                        | (uint32_t)(off & 0xFCu);
    ahci_pci_outl(PCI_CONFIG_ADDR, addr);
    ahci_pci_outl(PCI_CONFIG_DATA, v);
}

struct ahci_pci_loc {
    uint8_t bus;
    uint8_t dev;
    uint8_t fn;
};

/* 扫 class 0x01 / subclass 0x06 / prog-if 0x01（SATA AHCI 1.0）。0 = 找到。 */
static int ahci_pci_find(struct ahci_pci_loc *out)
{
    uint32_t bus, dev, fn;

    for (bus = 0; bus < 256u; bus++) {
        for (dev = 0; dev < 32u; dev++) {
            uint32_t id = ahci_pci_cfg_read32((uint8_t)bus, (uint8_t)dev, 0, PCI_CFG_VENDOR_ID);
            uint32_t hdr, fn_max;

            if ((id & 0xFFFFu) == 0xFFFFu || (id & 0xFFFFu) == 0x0000u)
                continue;                                   /* 该槽无设备 */

            /* 多功能设备的兄弟功能藏在 header type bit7 后面 */
            hdr = ahci_pci_cfg_read32((uint8_t)bus, (uint8_t)dev, 0, PCI_CFG_HEADER);
            fn_max = (hdr & 0x00800000u) ? 8u : 1u;

            for (fn = 0; fn < fn_max; fn++) {
                uint32_t cc;
                if (fn != 0u) {
                    id = ahci_pci_cfg_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, PCI_CFG_VENDOR_ID);
                    if ((id & 0xFFFFu) == 0xFFFFu)
                        continue;
                }
                cc = ahci_pci_cfg_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, PCI_CFG_REV_CLASS);
                if (((cc >> 24) & 0xFFu) == 0x01u &&      /* base class  = mass storage   */
                    ((cc >> 16) & 0xFFu) == 0x06u &&      /* sub class   = SATA           */
                    ((cc >> 8)  & 0xFFu) == 0x01u) {      /* prog-if     = AHCI 1.0       */
                    out->bus = (uint8_t)bus;
                    out->dev = (uint8_t)dev;
                    out->fn  = (uint8_t)fn;
                    return 0;
                }
            }
        }
    }
    return -1;
}

/* ===================== HBA 寄存器 (ABAR) ===================== */
#define AHCI_CAP   0x00u   /* Host Capabilities  (NP = bits[4:0])              */
#define AHCI_GHC   0x04u   /* Global Host Control (HR=bit0, IE=bit1, AE=bit31) */
#define AHCI_IS    0x08u   /* Interrupt Status  (RW1C)                         */
#define AHCI_PI    0x0Cu   /* Ports Implemented (位图)                         */
#define AHCI_VS    0x10u   /* Version                                          */
#define AHCI_CAP2  0x24u   /* Host Capabilities Extended (BOH=bit0)            */
#define AHCI_BOHC  0x28u   /* BIOS/OS Handoff Control and Status               */

#define AHCI_GHC_HR (1u << 0)
#define AHCI_GHC_IE (1u << 1)
#define AHCI_GHC_AE (1u << 31)

#define AHCI_CAP2_BOH (1u << 0)
#define AHCI_BOHC_BOS (1u << 0)
#define AHCI_BOHC_OOS (1u << 1)
#define AHCI_BOHC_OOC (1u << 2)

/* 端口寄存器：基址 0x100 + 端口号 * 0x80 */
#define AHCI_PORT_BASE(p) (0x100u + (p) * 0x80u)

#define P_CLB   0x00u   /* 命令列表基址（1 KiB 对齐）                       */
#define P_CLBU  0x04u
#define P_FB    0x08u   /* FIS 接收区基址（256 B 对齐）                     */
#define P_FBU   0x0Cu
#define P_IS    0x10u   /* 端口中断状态（RW1C）                             */
#define P_IE    0x14u   /* 端口中断使能（本驱动恒 0 = 轮询）                */
#define P_CMD   0x18u   /* 端口命令/状态                                    */
#define P_TFD   0x20u   /* Task File Data (ERR/DF/DRQ/BSY)                  */
#define P_SIG   0x24u   /* 设备签名                                         */
#define P_SSTS  0x28u   /* SATA 状态 (DET bits[3:0], IPM bits[11:8])        */
#define P_SCTL  0x2Cu   /* SATA 控制 (DET 写 1 = COMRESET)                  */
#define P_SERR  0x30u   /* SATA 错误（RW1C）                                */
#define P_SACT  0x34u   /* 命令激活（NCQ；本驱动不用）                      */
#define P_CI    0x38u   /* 命令已下发（bit n = slot n）                     */
#define P_SNTF  0x3Cu

#define P_CMD_ST   (1u << 0)    /* Start（命令列表引擎）                    */
#define P_CMD_SUD  (1u << 1)    /* Spin Up Device                           */
#define P_CMD_POD  (1u << 2)    /* Power On Device                          */
#define P_CMD_CLO  (1u << 3)    /* Command List Overrun                     */
#define P_CMD_FRE  (1u << 4)    /* FIS Receive Enable                       */
#define P_CMD_FR   (1u << 14)   /* FIS Receive Running（只读镜像位）        */
#define P_CMD_CR   (1u << 15)   /* Command List Running（只读镜像位）       */

/* PxIS：只列本驱动会用到的位。**不要**整体写 0xFFFFFFFF（VimtuOS 踩坑 C：
 * 把保留位写成 1 之后 HBA 会"收下 PxCI 但不执行"）。 */
#define AHCI_IS_DHRS (1u << 0)    /* 收到 D2H Register FIS（完成位）      */
#define AHCI_IS_PSS  (1u << 1)    /* PIO setup FIS                        */
#define AHCI_IS_DSS  (1u << 2)    /* DMA setup FIS                        */
#define AHCI_IS_SDBS (1u << 3)    /* Set Device Bits FIS                  */
#define AHCI_IS_UFS  (1u << 4)    /* Unknown FIS                          */
#define AHCI_IS_DPS  (1u << 5)    /* Descriptor processed                 */
#define AHCI_IS_PCS  (1u << 6)    /* Port connect change                  */
#define AHCI_IS_DMPS (1u << 7)    /* Device mechanical presence           */
#define AHCI_IS_PRCS (1u << 22)   /* PhyRdy change                        */
#define AHCI_IS_IPMS (1u << 23)   /* Incorrect port multiplier            */
#define AHCI_IS_OFS  (1u << 24)   /* Overflow                             */
#define AHCI_IS_INFS (1u << 26)   /* Interface non-fatal error            */
#define AHCI_IS_IFS  (1u << 27)   /* Interface fatal error                */
#define AHCI_IS_HBFS (1u << 28)   /* Host bus fatal error                 */
#define AHCI_IS_HBDS (1u << 29)   /* Host bus data error                  */
#define AHCI_IS_TFES (1u << 30)   /* Task file error                      */

/* 规范定义的 PxIS 可清位（RW1C），不含保留位 */
#define AHCI_IS_CLEAR (AHCI_IS_DHRS | AHCI_IS_PSS  | AHCI_IS_DSS  | AHCI_IS_SDBS | \
                       AHCI_IS_UFS  | AHCI_IS_DPS  | AHCI_IS_PCS  | AHCI_IS_DMPS | \
                       AHCI_IS_PRCS | AHCI_IS_IPMS | AHCI_IS_OFS  | AHCI_IS_INFS | \
                       AHCI_IS_IFS  | AHCI_IS_HBFS | AHCI_IS_HBDS | AHCI_IS_TFES)
/* 判定"这条命令失败"的错误位 */
#define AHCI_IS_FATAL (AHCI_IS_TFES | AHCI_IS_HBDS | AHCI_IS_HBFS | AHCI_IS_IFS | \
                       AHCI_IS_INFS | AHCI_IS_OFS  | AHCI_IS_IPMS | AHCI_IS_PRCS)

#define P_TFD_ERR (1u << 0)
#define P_TFD_DRQ (1u << 3)
#define P_TFD_DF  (1u << 5)
#define P_TFD_BSY (1u << 7)

/* PxSERR：同样只清规范定义的 RW1C 位（bit0/1 恢复类错误、bit8..11 协议/内部、
 * bit16 PhyRdy 变化、bit26 设备热插拔；位名与 Linux libata 的 SERR_* 一致）。 */
#define AHCI_SERR_RECOV     0x00000003u
#define AHCI_SERR_DATA      (1u << 8)
#define AHCI_SERR_PERSIST   (1u << 9)
#define AHCI_SERR_PROTOCOL  (1u << 10)
#define AHCI_SERR_INTERNAL  (1u << 11)
#define AHCI_SERR_PHYRDY    (1u << 16)
#define AHCI_SERR_DEV_XCHG  (1u << 26)
#define AHCI_SERR_CLEAR (AHCI_SERR_RECOV | AHCI_SERR_DATA | AHCI_SERR_PERSIST | \
                         AHCI_SERR_PROTOCOL | AHCI_SERR_INTERNAL | \
                         AHCI_SERR_PHYRDY | AHCI_SERR_DEV_XCHG)

/* SSTS.DET == 3：设备在位且 PHY 通信建立 */
#define AHCI_SSTS_DET_MASK 0x0Fu
#define AHCI_SSTS_DET_NONE 0u     /* 端口上没有设备 */
#define AHCI_SSTS_DET_READY 3u

/* 签名 0xEB140101 = ATAPI（本驱动只支持 ATA 盘） */
#define AHCI_SIG_ATAPI 0xEB140101u

/* ===================== ATA 命令与 FIS ===================== */
#define ATA_CMD_READ_DMA      0x20u   /* LBA28 DMA 读 */
#define ATA_CMD_READ_DMA_EXT  0x24u   /* LBA48 DMA 读 */
#define ATA_CMD_WRITE_DMA     0x30u   /* LBA28 DMA 写 */
#define ATA_CMD_WRITE_DMA_EXT 0x34u   /* LBA48 DMA 写 */
#define ATA_CMD_IDENTIFY      0xECu   /* IDENTIFY DEVICE */
#define ATA_CMD_CACHE_FLUSH   0xE7u   /* FLUSH CACHE */

#define AHCI_FIS_H2D     0x27u   /* H2D Register FIS 类型 */
#define AHCI_FIS_CFL     5u      /* 20 字节 FIS = 5 个 dword */
#define AHCI_FIS_C       0x80u   /* FIS byte1 bit7 = C：更新命令寄存器 */

#define AHCI_CMDH_W       (1u << 6)   /* 命令头 DW0 的 W（DMA 方向） */

/* ===================== 缓冲区/传输尺寸 ===================== */
#define AHCI_SECTOR_SIZE      512u
#define AHCI_MAX_CHUNK_SECTORS 512u                    /* 512 * 512 = 256 KiB */
#define AHCI_DMA_BUF_SIZE     (AHCI_MAX_CHUNK_SECTORS * AHCI_SECTOR_SIZE)
#define AHCI_CMD_LIST_BYTES   1024u   /* 32 slot * 32 B；本驱动只用 slot 0   */
#define AHCI_FIS_BYTES        256u
#define AHCI_CMD_TBL_BYTES    512u    /* 0x00 CFIS 64 B + 0x40 ACMD + 0x80 PRDT */
#define AHCI_PRDT_OFF         0x80u

/* LBA28 可寻址上限（0x0FFFFFFF + 1 个扇区） */
#define AHCI_LBA28_LIMIT 0x10000000ULL
/* 容量上限：52 位 LBA48 全 1 之上再截没有意义；这里按 2^48 扇区（128 PiB）
 * 兜底，避免把坏掉的 IDENTIFY 数据当成真实容量去算偏移。 */
#define AHCI_MAX_SECTOR_COUNT (1ULL << 48)

/* ===================== 有界等待 ===================== */
#define AHCI_SPIN_SHORT     2000000u     /* 端口启停/寄存器握手           */
#define AHCI_SPIN_MEDIUM   20000000u     /* COMRESET / BIOS handoff       */
#define AHCI_SPIN_CMD     100000000u     /* 单条命令完成（无中断时用）    */
#define AHCI_SPINS_BEFORE_HLT 4096u      /* 先自旋再 hlt，见文件头说明    */
/* SutureOS fix: hlt 要等下一次 LAPIC tick 才醒（100 Hz → 10 ms），所以"自旋次数"
 * 预算一旦交给 hlt 循环就完全不是时间预算了——空 SATA 端口上实测
 * `for (i = 0; i < 20000000; i++) hlt;` 相当于 ~55 小时，第二次开机直接卡死在
 * "[ahci] port 1: det=0, issuing COMRESET"（pi=0x3f，QEMU 有 6 个端口，只有
 * port 0 插了盘）。所有"先自旋后 hlt"的等待都改成两阶段：自旋 AHCI_SPINS_BEFORE_HLT
 * 次，然后最多 hlt AHCI_HLT_WAITS 次（≈1 s）。 */
#define AHCI_HLT_WAITS        100u
/* 完成之后等 Task File 真正空闲的自旋预算（见 ahci_issue_once 的 SutureOS fix） */
#define AHCI_TF_SETTLE_SPINS 200000u

/* SutureOS fix（VMware 实测）：有的 AHCI 模型/固件在命令完成的那一瞬间 PxTFD
 * 仍显示 DRQ=1（QEMU 不会）。原实现把"完成但不空闲"直接判失败，于是 VMware 上
 * 第一次读盘就报
 *   [ahci] port 0 error: tfd=0x8 serr=0x0 cmd=0x24 (tf not idle after completion)
 * → 端口重启重试 → 又撞上 pre-check 的 "device busy" → sata0 read failed
 * → FAT32/ext2/SquashFS 全部挂不上（用户截图为证）。
 * 正确判据是 ERR/DF 与 PxIS 的致命位：DRQ/BSY 只决定"下一条命令能不能发"。
 * 第一次遇到时短等一下并记一次日志；若该设备始终如此就置 sticky 标志，
 * 后续命令不再为它付等待时间（PxCI 已清零且无错误位 ⇒ HBA 已经把数据 DMA 进
 * 内存，传输本身是成功的）。 */
static int      g_tf_idle_quirk;
static uint32_t g_tf_quirk_logged;

/* ===================== DMA 可见缓冲 (.bss) =====================
 * 本内核恒等映射低 1 GiB，且 PMM 从 _bss_end 之后才开始分配，所以这些静态
 * 缓冲的虚拟地址就是物理地址，且永远不会被堆/页分配器复用。每个端口一份
 * 命令列表/FIS/命令表（不共用：两端口并发会互相踩，VimtuOS 踩坑 I）。
 * jmplist/FIS/命令表 的对齐按规范：1 KiB / 256 B / 128 B。 */
static uint8_t g_cmd_list[AHCI_MAX_DEVS][AHCI_CMD_LIST_BYTES] __attribute__((aligned(1024)));
static uint8_t g_fis     [AHCI_MAX_DEVS][AHCI_FIS_BYTES]      __attribute__((aligned(256)));
static uint8_t g_cmd_tbl [AHCI_MAX_DEVS][AHCI_CMD_TBL_BYTES]  __attribute__((aligned(128)));

/* 唯一的数据 DMA 暂存缓冲：所有读写都经过它（单线程、同步轮询）。
 * 用暂存而不是直接 DMA 调用方缓冲，是因为调用方指针不一定在恒等映射的低
 * 1 GiB 内，而 DMA 只认物理地址；暂存同时保证 PRDT 的 DBA 一定是
 * 4 KiB 对齐的物理地址。代价是每个 chunk 一次 memcpy。 */
static uint8_t g_dma_buf[AHCI_DMA_BUF_SIZE] __attribute__((aligned(4096)));

/* ===================== 驱动状态 ===================== */
struct ahci_dev {
    uint32_t port;             /* 硬件端口号                              */
    uint32_t slot;             /* 静态缓冲下标（== g_devs[] 下标）        */
    uint64_t sectors;          /* IDENTIFY 得到的容量（扇区）             */
    int      lba48;            /* 设备支持 LBA48                          */
    int      present;
    char     model[41];        /* words 27..46                            */
    char     serial[21];       /* words 10..19                            */
    char     name[8];          /* "sata0" ...                             */
    struct blk_device blk;     /* 注册进块层的设备对象                    */
};

static struct ahci_dev g_devs[AHCI_MAX_DEVS];
static int      g_dev_count;         /* 已识别（含未注册）的盘数        */
static int      g_controller_found;  /* 找到并 bring-up 过控制器        */
static int      g_init_done;         /* blk_ahci_init() 幂等标志        */
static int      g_last_count;        /* 最近一次探测结果（诊断）        */
static uint64_t g_abar;              /* ABAR（已恒等映射）              */
static uint32_t g_ctrl_bus, g_ctrl_dev, g_ctrl_fn;

/* ===================== MMIO 访问 ===================== */
static inline uint32_t ahci_mmio_rd(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(g_abar + (uint64_t)off);
}

static inline void ahci_mmio_wr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)(g_abar + (uint64_t)off) = v;
    __asm__ volatile("" ::: "memory");   /* 防编译器挪动/合并 MMIO 写 */
}

static inline uint32_t ahci_port_rd(uint32_t p, uint32_t reg)
{
    return ahci_mmio_rd(AHCI_PORT_BASE(p) + reg);
}

static inline void ahci_port_wr(uint32_t p, uint32_t reg, uint32_t v)
{
    ahci_mmio_wr(AHCI_PORT_BASE(p) + reg, v);
}

/* ===================== 缓存维护 ===================== */
/* 见文件头"缓存一致性假设"：DMA 前刷出、DMA 读回后刷掉（invalidate）。 */
static void ahci_clflush(const void *addr, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)((uintptr_t)addr & ~(uintptr_t)63);
    const uint8_t *end = (const uint8_t *)addr + len;

    while (p < end) {
        __asm__ volatile("clflush (%0)" :: "r"(p) : "memory");
        p += 64;
    }
}

/* ===================== 有界等待 ===================== */
static int ahci_irqs_enabled(void)
{
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0" : "=r"(fl));
    return (fl & 0x200u) ? 1 : 0;
}

/* 等 (端口寄存器 reg & mask) == want。0 = 满足，-1 = 超时。
 * 两阶段有界：自旋 min(spins, AHCI_SPINS_BEFORE_HLT) 次，然后最多 hlt
 * AHCI_HLT_WAITS 次（≈1 s）。IF=0 时没有 tick，只自旋 spins 次。 */
static int ahci_poll_port(uint32_t p, uint32_t reg, uint32_t mask, uint32_t want,
                          uint32_t spins)
{
    const int can_hlt = ahci_irqs_enabled();
    uint32_t i;
    uint32_t spin_budget = spins;

    if (can_hlt && spin_budget > AHCI_SPINS_BEFORE_HLT)
        spin_budget = AHCI_SPINS_BEFORE_HLT;

    for (i = 0; i < spin_budget; i++) {
        if ((ahci_port_rd(p, reg) & mask) == want)
            return 0;
        __asm__ volatile("pause");
    }
    if (!can_hlt)
        return -1;
    for (i = 0; i < AHCI_HLT_WAITS; i++) {
        if ((ahci_port_rd(p, reg) & mask) == want)
            return 0;
        __asm__ volatile("hlt");
    }
    return -1;
}

/* GHC.HR 自清等待（GHC 是全局寄存器，不复用 ahci_poll_port） */
static int ahci_wait_ghc_hr(void)
{
    const int can_hlt = ahci_irqs_enabled();
    uint32_t i;
    uint32_t spin_budget = can_hlt ? AHCI_SPINS_BEFORE_HLT : AHCI_SPIN_MEDIUM;

    for (i = 0; i < spin_budget; i++) {
        if ((ahci_mmio_rd(AHCI_GHC) & AHCI_GHC_HR) == 0u)
            return 0;
        __asm__ volatile("pause");
    }
    if (!can_hlt)
        return -1;
    for (i = 0; i < AHCI_HLT_WAITS; i++) {
        if ((ahci_mmio_rd(AHCI_GHC) & AHCI_GHC_HR) == 0u)
            return 0;
        __asm__ volatile("hlt");
    }
    return -1;
}

/* ===================== 端口命令引擎 ===================== */
/* 停端口：ST=0 等 CR/CLO 落，FRE=0 等 FR 落。只读-改-写 ST/FRE 两位，
 * 绝不碰 ICC（bits[31:28]）：VimtuOS 踩坑 D 证明写 ICC 会让命令不再执行。 */
static int ahci_port_stop(uint32_t p)
{
    uint32_t cmd = ahci_port_rd(p, P_CMD);

    if (cmd & P_CMD_ST) {
        ahci_port_wr(p, P_CMD, cmd & ~P_CMD_ST);
        if (ahci_poll_port(p, P_CMD, P_CMD_CR | P_CMD_CLO, 0u, AHCI_SPIN_SHORT) != 0) {
            console_printf("[ahci] port %u: CR/CLO did not clear after ST=0 (cmd=0x%x)\n",
                           (uint64_t)p, (uint64_t)ahci_port_rd(p, P_CMD));
            return -1;
        }
    }
    cmd = ahci_port_rd(p, P_CMD);
    if (cmd & P_CMD_FRE) {
        ahci_port_wr(p, P_CMD, cmd & ~P_CMD_FRE);
        if (ahci_poll_port(p, P_CMD, P_CMD_FR, 0u, AHCI_SPIN_SHORT) != 0) {
            console_printf("[ahci] port %u: FR did not clear after FRE=0 (cmd=0x%x)\n",
                           (uint64_t)p, (uint64_t)ahci_port_rd(p, P_CMD));
            return -1;
        }
    }
    return 0;
}

/* 启动端口：命令列表/FIS 基址必须在 ST=0 时写；然后 FRE=1 -> ST=1。 */
static int ahci_port_start(uint32_t p, uint32_t slot)
{
    const uint64_t cl_pa = (uint64_t)(uintptr_t)g_cmd_list[slot];
    const uint64_t fb_pa = (uint64_t)(uintptr_t)g_fis[slot];
    uint32_t cmd;

    /* 恒等映射下 PA == VA；缓冲都在低 1 GiB，所以 32 位基址够用 */
    memset(g_cmd_list[slot], 0, AHCI_CMD_LIST_BYTES);
    memset(g_fis[slot], 0, AHCI_FIS_BYTES);

    ahci_port_wr(p, P_CLB,  (uint32_t)(cl_pa & 0xFFFFFFFFu));
    ahci_port_wr(p, P_CLBU, (uint32_t)(cl_pa >> 32));
    ahci_port_wr(p, P_FB,   (uint32_t)(fb_pa & 0xFFFFFFFFu));
    ahci_port_wr(p, P_FBU,  (uint32_t)(fb_pa >> 32));
    ahci_port_wr(p, P_IE, 0u);                  /* 轮询：端口中断全关 */
    ahci_port_wr(p, P_IS, AHCI_IS_CLEAR);

    cmd = ahci_port_rd(p, P_CMD);
    ahci_port_wr(p, P_CMD, cmd | P_CMD_FRE);
    if (ahci_poll_port(p, P_CMD, P_CMD_FRE, P_CMD_FRE, AHCI_SPIN_SHORT) != 0) {
        console_printf("[ahci] port %u: FRE did not set (cmd=0x%x)\n",
                       (uint64_t)p, (uint64_t)ahci_port_rd(p, P_CMD));
        return -1;
    }
    cmd = ahci_port_rd(p, P_CMD);
    ahci_port_wr(p, P_CMD, cmd | P_CMD_ST);
    if (ahci_poll_port(p, P_CMD, P_CMD_ST, P_CMD_ST, AHCI_SPIN_SHORT) != 0) {
        console_printf("[ahci] port %u: ST did not set (cmd=0x%x)\n",
                       (uint64_t)p, (uint64_t)ahci_port_rd(p, P_CMD));
        return -1;
    }
    return 0;
}

/* 重启用最小动作：停 ST（等 CLO/CR 落）再置 ST。CLB/FB 保持不动、FRE 保持 1。
 * 用于命令失败后的一次重试（真机上冷启动后某些 HBA/设备需要一次 link 周期）。 */
static void ahci_port_restart(uint32_t p)
{
    uint32_t cmd = ahci_port_rd(p, P_CMD);

    if (cmd & P_CMD_ST) {
        ahci_port_wr(p, P_CMD, cmd & ~P_CMD_ST);
        (void)ahci_poll_port(p, P_CMD, P_CMD_CR | P_CMD_CLO, 0u, AHCI_SPIN_SHORT);
    }
    cmd = ahci_port_rd(p, P_CMD);
    ahci_port_wr(p, P_CMD, cmd | P_CMD_ST);
    (void)ahci_poll_port(p, P_CMD, P_CMD_ST, P_CMD_ST, AHCI_SPIN_SHORT);
    ahci_port_wr(p, P_IS, AHCI_IS_CLEAR);
}

/* ===================== 命令下发 ===================== */
/* H2D Register FIS：20 字节有效（CFL = 5 个 dword），其余填 0。 */
static void ahci_fill_cfis(uint8_t *cfis, uint8_t cmd, uint64_t lba, uint32_t count,
                           int lba48)
{
    memset(cfis, 0, 64);

    cfis[0] = AHCI_FIS_H2D;
    cfis[1] = AHCI_FIS_C;                    /* C=1：更新命令寄存器 */
    cfis[2] = cmd;
    cfis[3] = 0;                             /* features low        */
    cfis[4] = (uint8_t)(lba);                /* LBA0 / lba[7:0]     */
    cfis[5] = (uint8_t)(lba >> 8);           /* LBA1                */
    cfis[6] = (uint8_t)(lba >> 16);          /* LBA2                */

    if (lba48) {
        cfis[7]  = 0x40;                     /* device：LBA 模式，LBA28 段留 0 */
        cfis[8]  = (uint8_t)(lba >> 24);     /* LBA3 */
        cfis[9]  = (uint8_t)(lba >> 32);     /* LBA4 */
        cfis[10] = (uint8_t)(lba >> 40);     /* LBA5 */
        cfis[11] = 0;                        /* features high       */
        cfis[12] = (uint8_t)(count);         /* count low  (16 位)  */
        cfis[13] = (uint8_t)(count >> 8);    /* count high          */
    } else {
        /* LBA28：device 寄存器低 4 位装 LBA 的高 4 位；count = 0 表示 256 */
        cfis[7]  = (uint8_t)(0x40u | ((lba >> 24) & 0x0Fu));
        cfis[8]  = 0;
        cfis[9]  = 0;
        cfis[10] = 0;
        cfis[12] = (uint8_t)(count & 0xFFu);
        cfis[13] = 0;
    }
    cfis[14] = 0;                            /* ICC     */
    cfis[15] = 0;                            /* control */
}

/* 下发一条命令并轮询完成（单 slot 0）。
 * bytes = 0 表示无数据命令（如 CACHE FLUSH，PRDTL = 0）。
 * 返回 0 = 成功；-1 = 失败（已打 [ahci] 日志）。 */
static int ahci_issue_once(struct ahci_dev *d, uint8_t cmd, uint64_t lba,
                           uint32_t count, uint32_t bytes, int is_write, int lba48)
{
    const uint32_t p = d->port;
    const uint32_t s = d->slot;
    const uint32_t nprd = (bytes == 0u) ? 0u : 1u;
    uint8_t *cl = g_cmd_list[s];
    uint8_t *ct = g_cmd_tbl[s];
    const uint64_t ct_pa = (uint64_t)(uintptr_t)ct;
    const uint64_t buf_pa = (uint64_t)(uintptr_t)g_dma_buf;
    volatile uint32_t *hdr = (volatile uint32_t *)(void *)cl;
    uint32_t is, tfd, serr, ci;
    int completed = 0;

    if (bytes > AHCI_DMA_BUF_SIZE)
        return -1;

    /* 1) 设备必须空闲（BSY/DRQ 落）。对已知"完成但 DRQ 残留"的设备（见文件上方
     * 的 SutureOS fix）在 quirk 标志置位后只要求 BSY 落，否则 VMware 上重试时
     * 会卡在这里。 */
    {
        uint32_t want_mask = P_TFD_BSY | P_TFD_DRQ;
        if (g_tf_idle_quirk)
            want_mask = P_TFD_BSY;
        if (ahci_poll_port(p, P_TFD, want_mask, 0u, AHCI_SPIN_SHORT) != 0) {
            console_printf("[ahci] port %u: device busy before cmd=0x%x tfd=0x%x\n",
                           (uint64_t)p, (uint64_t)cmd, (uint64_t)ahci_port_rd(p, P_TFD));
            return -1;
        }
    }
    /* 2) 上一条命令必须已经退场（slot 0 的 PxCI 位清零） */
    if (ahci_poll_port(p, P_CI, 1u, 0u, AHCI_SPIN_SHORT) != 0) {
        console_printf("[ahci] port %u: previous command still pending ci=0x%x\n",
                       (uint64_t)p, (uint64_t)ahci_port_rd(p, P_CI));
        return -1;
    }

    /* 3) 命令表：CFIS(0x00..0x3F) + ACMD(0x40..0x4F) + PRDT(从 0x80 起) */
    memset(ct, 0, AHCI_CMD_TBL_BYTES);
    ahci_fill_cfis(ct, cmd, lba, count, lba48);

    if (nprd != 0u) {
        uint8_t *pe = ct + AHCI_PRDT_OFF;
        /* PRDT 项 16 B：+0x00 DBA(8 B) +0x04 DBAU +0x08 **保留** +0x0C DBC。
         * ★ DBC 在 +0x0C（bits[21:0] = 字节数-1，bit31 = I），不是 +0x08：
         *   写到 +0x08 等于 DMA 长度 0（VimtuOS 踩坑 K）。 */
        *(volatile uint64_t *)(void *)pe        = buf_pa;
        *(volatile uint32_t *)(void *)(pe + 8)  = 0u;
        *(volatile uint32_t *)(void *)(pe + 12) =
            ((bytes - 1u) & 0x3FFFFFu) | ((nprd == 1u) ? 0x80000000u : 0u);
    }

    /* 4) 命令头（命令列表 slot 0，32 字节）
     * ★ 正确布局（VimtuOS 踩坑 J，写错 HBA 会静默丢弃整条命令）：
     *     DW0 bits[4:0] = CFL(5)，bit6 = W，bits[31:16] = PRDTL；
     *     DW1 = PRDBC（HBA 回填，软件写 0）；
     *     DW2/DW3 = CTBA/CTBAU；DW4..DW7 保留。 */
    hdr[0] = (AHCI_FIS_CFL & 0x1Fu)
           | (is_write ? AHCI_CMDH_W : 0u)
           | (nprd << 16);
    hdr[1] = 0;
    hdr[2] = (uint32_t)(ct_pa & 0xFFFFFFFFu);
    hdr[3] = (uint32_t)(ct_pa >> 32);
    hdr[4] = 0;
    hdr[5] = 0;
    hdr[6] = 0;
    hdr[7] = 0;
    __asm__ volatile("" ::: "memory");

    /* 5) DMA 前的缓存刷出：命令列表、命令表(含 CFIS/PRDT)、写请求的数据 */
    ahci_clflush(cl, 64);
    ahci_clflush(ct, AHCI_PRDT_OFF + (nprd * 16u));
    if (is_write && bytes != 0u)
        ahci_clflush(g_dma_buf, bytes);

    /* 6) 清端口状态位，然后写 PxCI 门铃 */
    ahci_port_wr(p, P_IS, AHCI_IS_CLEAR);
    __asm__ volatile("" ::: "memory");
    ahci_port_wr(p, P_CI, 1u);

    /* 7) 轮询完成：PxCI bit0 清零（两阶段有界：先自旋后 hlt，见 AHCI_HLT_WAITS） */
    {
        const int can_hlt = ahci_irqs_enabled();
        uint32_t i;
        uint32_t spin_budget = can_hlt ? AHCI_SPINS_BEFORE_HLT : AHCI_SPIN_CMD;
        uint32_t hlt_budget  = can_hlt ? AHCI_HLT_WAITS : 0u;

        for (i = 0; i < spin_budget + hlt_budget; i++) {
            if ((ahci_port_rd(p, P_CI) & 1u) == 0u) {
                completed = 1;
                break;
            }
            if (ahci_port_rd(p, P_IS) & AHCI_IS_FATAL)
                break;                        /* 错误：下面统一判定/打日志 */
            if (ahci_port_rd(p, P_TFD) & (P_TFD_ERR | P_TFD_DF))
                break;
            if (i < spin_budget)
                __asm__ volatile("pause");
            else
                __asm__ volatile("hlt");
        }
    }

    is   = ahci_port_rd(p, P_IS);
    tfd  = ahci_port_rd(p, P_TFD);
    serr = ahci_port_rd(p, P_SERR);
    ci   = ahci_port_rd(p, P_CI);

    /* 收尾：把状态位清干净，别留给下一条命令 */
    ahci_port_wr(p, P_IS, AHCI_IS_CLEAR);

    if ((is & AHCI_IS_FATAL) != 0u || (tfd & (P_TFD_ERR | P_TFD_DF)) != 0u) {
        /* 失败日志前缀固定，便于自动 grep：`[ahci] port N error: tfd=0x.. serr=0x..` */
        console_printf("[ahci] port %u error: tfd=0x%x serr=0x%x cmd=0x%x lba=%llu count=%u is=0x%x\n",
                       (uint64_t)p, (uint64_t)tfd, (uint64_t)serr, (uint64_t)cmd,
                       (unsigned long long)lba, (uint64_t)count, (uint64_t)is);
        return -1;
    }
    if (!completed) {
        console_printf("[ahci] port %u timeout: cmd=0x%x lba=%llu count=%u ci=0x%x tfd=0x%x serr=0x%x\n",
                       (uint64_t)p, (uint64_t)cmd, (unsigned long long)lba,
                       (uint64_t)count, (uint64_t)ci, (uint64_t)tfd, (uint64_t)serr);
        return -1;
    }
    if ((tfd & (P_TFD_BSY | P_TFD_DRQ)) != 0u) {
        /* 见文件上方的 SutureOS fix：完成但 Task File 还没空闲。等一小会儿；
         * 仍然如此就当作设备行为，接受这次已完成（无错误位）的传输并记住它。 */
        uint32_t spins;
        for (spins = 0; spins < AHCI_TF_SETTLE_SPINS; spins++) {
            if ((ahci_port_rd(p, P_TFD) & (P_TFD_BSY | P_TFD_DRQ)) == 0u)
                break;
            __asm__ volatile("pause");
        }
        tfd = ahci_port_rd(p, P_TFD);
        if ((tfd & (P_TFD_BSY | P_TFD_DRQ)) != 0u) {
            g_tf_idle_quirk = 1;
            if (g_tf_quirk_logged++ < 2u) {
                console_printf("[ahci] port %u: TFD=0x%x not idle after completion "
                               "(device quirk); transfer accepted "
                               "(PxCI clear, no ERR/DF)\n",
                               (uint64_t)p, (uint64_t)tfd);
            }
        }
    }
    return 0;
}

/* 失败后做一次"端口重启 + 重发"。绝不假装成功：重试仍失败就返回 -1。 */
static int ahci_issue(struct ahci_dev *d, uint8_t cmd, uint64_t lba, uint32_t count,
                      uint32_t bytes, int is_write, int lba48)
{
    if (ahci_issue_once(d, cmd, lba, count, bytes, is_write, lba48) == 0)
        return 0;

    console_printf("[ahci] port %u: retry cmd=0x%x lba=%llu after port restart\n",
                   (uint64_t)d->port, (uint64_t)cmd, (unsigned long long)lba);
    ahci_port_restart(d->port);
    return ahci_issue_once(d, cmd, lba, count, bytes, is_write, lba48);
}

/* ===================== IDENTIFY DEVICE (0xEC) ===================== */
/* words 10..19 = serial，27..46 = model（字内大端）。去尾部空格/NUL。 */
static void ahci_pick_string(char *out, uint32_t outsz, const uint16_t *w,
                             uint32_t first, uint32_t words)
{
    uint32_t i, n = 0;

    if (outsz == 0u)
        return;
    for (i = 0; i < words; i++) {
        const uint16_t v = w[first + i];
        if (n + 1u < outsz) out[n++] = (char)(v >> 8);
        if (n + 1u < outsz) out[n++] = (char)(v & 0xFFu);
    }
    out[n] = 0;
    while (n > 0u && (out[n - 1u] == ' ' || out[n - 1u] == '\0'))
        out[n-- - 1u] = 0;
}

/* 返回 0 = 成功（model/serial/sectors/lba48 已填），-1 = 失败。 */
static int ahci_identify(struct ahci_dev *d)
{
    const uint16_t *w;
    uint64_t lba28, lba48;
    int has_lba48;

    memset(g_dma_buf, 0, AHCI_SECTOR_SIZE);

    if (ahci_issue(d, ATA_CMD_IDENTIFY, 0, 1,
                   AHCI_SECTOR_SIZE, 0, 0) != 0)
        return -1;

    /* 设备刚 DMA 写入这块内存：刷掉缓存行再让 CPU 读 */
    ahci_clflush(g_dma_buf, AHCI_SECTOR_SIZE);

    w = (const uint16_t *)(const void *)g_dma_buf;

    ahci_pick_string(d->serial, sizeof(d->serial), w, 10u, 10u);
    ahci_pick_string(d->model,  sizeof(d->model),  w, 27u, 20u);
    if (d->model[0] == '\0')
        strcpy(d->model, "unknown");

    lba28 = (uint64_t)w[60] | ((uint64_t)w[61] << 16);
    lba48 = (uint64_t)w[100] | ((uint64_t)w[101] << 16)
          | ((uint64_t)w[102] << 32) | ((uint64_t)w[103] << 48);
    has_lba48 = (w[83] & 0x0400u) != 0;             /* word83 bit10 = LBA48 支持 */

    d->lba48  = (has_lba48 && lba48 != 0u) ? 1 : 0;
    d->sectors = d->lba48 ? lba48 : lba28;

    /* 不信任盘上数据：容量做上限兜底并如实记录 */
    if (d->sectors > AHCI_MAX_SECTOR_COUNT) {
        console_printf("[ahci] port %u: IDENTIFY capacity 0x%llx sectors is implausible, capping\n",
                       (uint64_t)d->port, (unsigned long long)d->sectors);
        d->sectors = AHCI_MAX_SECTOR_COUNT;
    }
    if (d->sectors == 0u) {
        console_printf("[ahci] port %u: IDENTIFY reports 0 sectors (capacity unknown)\n",
                       (uint64_t)d->port);
    }
    return 0;
}

/* ===================== 数据搬运 ===================== */
/* 按 <= 256 KiB（512 扇区）分块；LBA28 设备限制到 256 扇区/条（count 寄存器 8 位）。
 * 返回 0 = 成功，-1 = 失败（已打日志）。 */
static int ahci_transfer(struct blk_device *dev, uint64_t lba, uint32_t count,
                         void *buf, int is_write)
{
    struct ahci_dev *d;
    uint8_t *p8 = (uint8_t *)buf;
    uint32_t done = 0;

    if (!dev || !buf)
        return -1;
    d = (struct ahci_dev *)dev->driver_data;
    if (!d)
        return -1;
    if (count == 0u)
        return 0;

    /* 边界检查：只在容量可信（非 0）时拒绝越界访问 */
    if (d->sectors != 0u &&
        (lba >= d->sectors || (uint64_t)count > (d->sectors - lba))) {
        console_printf("[ahci] sata%u %s beyond capacity lba=%llu count=%u sectors=%llu\n",
                       (uint64_t)d->slot, is_write ? "write" : "read",
                       (unsigned long long)lba, (uint64_t)count,
                       (unsigned long long)d->sectors);
        return -1;
    }

    while (done < count) {
        const uint64_t cur_lba = lba + (uint64_t)done;
        uint32_t chunk = count - done;
        int use48;
        uint64_t end;

        /* LBA48：设备支持就用；否则 LBA/末尾超出 28 位范围时也必须用 */
        end = cur_lba + (uint64_t)chunk;
        use48 = (d->lba48 || end > AHCI_LBA28_LIMIT) ? 1 : 0;

        if (use48) {
            if (chunk > AHCI_MAX_CHUNK_SECTORS)
                chunk = AHCI_MAX_CHUNK_SECTORS;
        } else {
            if (chunk > 256u)
                chunk = 256u;                       /* LBA28 count 寄存器 8 位 */
            if (cur_lba + (uint64_t)chunk > AHCI_LBA28_LIMIT) {
                const uint64_t room = AHCI_LBA28_LIMIT - cur_lba;
                if (room == 0u) {
                    console_printf("[ahci] sata%u: lba=%llu beyond LBA28 range\n",
                                   (uint64_t)d->slot, (unsigned long long)cur_lba);
                    return -1;
                }
                if ((uint64_t)chunk > room)
                    chunk = (uint32_t)room;
            }
        }

        {
            const uint32_t bytes = chunk * AHCI_SECTOR_SIZE;
            const uint8_t cmd =
                is_write ? (use48 ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_WRITE_DMA)
                         : (use48 ? ATA_CMD_READ_DMA_EXT  : ATA_CMD_READ_DMA);

            if (is_write) {
                memcpy(g_dma_buf, p8 + (uintptr_t)done * AHCI_SECTOR_SIZE, bytes);
                /* DMA 前的刷出在 ahci_issue_once 里做（只在需要时） */
            }

            if (ahci_issue(d, cmd, cur_lba, chunk, bytes, is_write, use48) != 0) {
                console_printf("[ahci] sata%u %s failed lba=%llu count=%u cmd=0x%x\n",
                               (uint64_t)d->slot, is_write ? "write" : "read",
                               (unsigned long long)cur_lba, (uint64_t)chunk,
                               (uint64_t)cmd);
                return -1;
            }

            if (!is_write) {
                /* 设备刚 DMA 写入：刷掉缓存行再拷贝给调用方 */
                ahci_clflush(g_dma_buf, bytes);
                memcpy(p8 + (uintptr_t)done * AHCI_SECTOR_SIZE, g_dma_buf, bytes);
            }
        }

        done += chunk;
    }
    return 0;
}

static int ahci_blk_read(struct blk_device *dev, uint64_t lba, uint32_t count, void *buf)
{
    return ahci_transfer(dev, lba, count, buf, 0);
}

static int ahci_blk_write(struct blk_device *dev, uint64_t lba, uint32_t count,
                          const void *buf)
{
    return ahci_transfer(dev, lba, count, (void *)buf, 1);
}

/* FLUSH CACHE (0xE7)：无数据命令（PRDTL = 0） */
static int ahci_blk_flush(struct blk_device *dev)
{
    struct ahci_dev *d = dev ? (struct ahci_dev *)dev->driver_data : NULL;

    if (!d)
        return -1;
    return ahci_issue(d, ATA_CMD_CACHE_FLUSH, 0, 0, 0u, 0, d->lba48);
}

static const struct blk_ops g_ahci_ops = {
    .read  = ahci_blk_read,
    .write = ahci_blk_write,
    .flush = ahci_blk_flush,
    .ioctl = NULL,
};

/* ===================== 端口探测 + 初始化 ===================== */
/* 一个端口的完整 bring-up：建缓冲、启引擎、IDENTIFY、（可选）注册块设备。
 * 返回 0 = 该端口登记为一块盘，-1 = 跳过（已打日志）。 */
static int ahci_bringup_port(uint32_t p, int do_register)
{
    struct ahci_dev *d;
    uint32_t ssts, det, sig, tfd, serr;

    /* 停引擎：只有 ST=0 时才能改 CLB/FB，也只有停了才能干净地清状态位 */
    if (ahci_port_stop(p) != 0)
        return -1;

    /* 只清规范定义的 RW1C 位（不写 0xFFFFFFFF，VimtuOS 踩坑 C） */
    ahci_port_wr(p, P_IS, AHCI_IS_CLEAR);
    ahci_port_wr(p, P_SERR, AHCI_SERR_CLEAR);

    ssts = ahci_port_rd(p, P_SSTS);
    det  = ssts & AHCI_SSTS_DET_MASK;

    if (det == AHCI_SSTS_DET_NONE) {
        /* DET=0 = 该端口没有设备。COMRESET 唤不醒空端口，而等待（即使有界）
         * 也没有意义：QEMU 的 pi=0x3f 就代表 6 个端口里通常只有 0 号插盘，
         * 光是为了"确认另外 5 个是空的"不该花掉几秒。直接跳过。 */
        return -1;
    }

    if (det != AHCI_SSTS_DET_READY) {
        /* 只有当前不就绪才做一次 COMRESET（VimtuOS 踩坑 F：已就绪的端口不要惊醒） */
        console_printf("[ahci] port %u: det=%u, issuing COMRESET\n",
                       (uint64_t)p, (uint64_t)det);
        ahci_port_wr(p, P_SCTL, (ahci_port_rd(p, P_SCTL) & ~AHCI_SSTS_DET_MASK) | 1u);
        {
            const int can_hlt = ahci_irqs_enabled();
            uint32_t i;
            uint32_t spin_budget = can_hlt ? AHCI_SPINS_BEFORE_HLT : AHCI_SPIN_MEDIUM;
            uint32_t hlt_budget  = can_hlt ? AHCI_HLT_WAITS : 0u;
            for (i = 0; i < spin_budget + hlt_budget; i++) {
                if ((ahci_port_rd(p, P_SSTS) & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_READY)
                    break;
                if (i < spin_budget) __asm__ volatile("pause");
                else                 __asm__ volatile("hlt");
            }
        }
        ahci_port_wr(p, P_SCTL, ahci_port_rd(p, P_SCTL) & ~AHCI_SSTS_DET_MASK);
        if (ahci_poll_port(p, P_SSTS, AHCI_SSTS_DET_MASK, AHCI_SSTS_DET_READY,
                           AHCI_SPIN_MEDIUM) != 0) {
            console_printf("[ahci] port %u: no device (ssts=0x%x det=%u ipm=%u)\n",
                           (uint64_t)p, (uint64_t)ahci_port_rd(p, P_SSTS),
                           (uint64_t)(ahci_port_rd(p, P_SSTS) & AHCI_SSTS_DET_MASK),
                           (uint64_t)((ahci_port_rd(p, P_SSTS) >> 8) & 0x0Fu));
            return -1;
        }
    }

    if (g_dev_count >= AHCI_MAX_DEVS) {
        console_printf("[ahci] port %u: device limit (%u) reached, skipped\n",
                       (uint64_t)p, (uint64_t)AHCI_MAX_DEVS);
        return -1;
    }

    d = &g_devs[g_dev_count];
    memset(d, 0, sizeof(*d));
    d->port = p;
    d->slot = (uint32_t)g_dev_count;

    if (ahci_port_start(p, d->slot) != 0) {
        console_printf("[ahci] port %u: failed to start command engine\n", (uint64_t)p);
        return -1;
    }

    /* 启动后再读一次 DET/SIG：QEMU/真机上签名只在端口跑起来后才是真值 */
    ssts = ahci_port_rd(p, P_SSTS);
    det  = ssts & AHCI_SSTS_DET_MASK;
    sig  = ahci_port_rd(p, P_SIG);

    if (det != AHCI_SSTS_DET_READY) {
        console_printf("[ahci] port %u: link down after start (ssts=0x%x det=%u)\n",
                       (uint64_t)p, (uint64_t)ssts, (uint64_t)det);
        return -1;
    }
    if (sig == AHCI_SIG_ATAPI) {
        console_printf("[ahci] port %u: ATAPI device (sig=0x%x), not supported by this driver\n",
                       (uint64_t)p, (uint64_t)sig);
        return -1;
    }

    if (ahci_identify(d) != 0) {
        tfd  = ahci_port_rd(p, P_TFD);
        serr = ahci_port_rd(p, P_SERR);
        console_printf("[ahci] port %u: IDENTIFY DEVICE failed tfd=0x%x serr=0x%x\n",
                       (uint64_t)p, (uint64_t)tfd, (uint64_t)serr);
        return -1;
    }

    /* 固定格式（自动验收按这行 grep）：
     * `[ahci] port 0: <model> sectors=<n> lba48=<0|1>` */
    console_printf("[ahci] port %u: %s sectors=%llu lba48=%u serial=%s\n",
                   (uint64_t)p, d->model, (unsigned long long)d->sectors,
                   (uint64_t)(uint32_t)d->lba48, d->serial);

    d->present = 1;

    if (do_register) {
        const int drive = AHCI_DRIVE_BASE + (int)g_dev_count;
        const uint32_t idx = (uint32_t)g_dev_count;

        d->name[0] = 's'; d->name[1] = 'a'; d->name[2] = 't'; d->name[3] = 'a';
        d->name[4] = (char)('0' + (idx % 10u));
        d->name[5] = (idx >= 10u) ? (char)('0' + ((idx / 10u) % 10u)) : '\0';
        if (idx >= 10u) d->name[6] = '\0';

        if (blk_find_by_drive(drive) != NULL) {
            console_printf("[ahci] drive %d already registered, sata%u not registered\n",
                           (int64_t)drive, (uint64_t)idx);
        } else {
            d->blk.name         = d->name;
            d->blk.type         = BLK_TYPE_AHCI;
            d->blk.drive        = drive;
            d->blk.flags        = 0;
            d->blk.sector_count = d->sectors;
            d->blk.sector_size  = AHCI_SECTOR_SIZE;
            d->blk.block_size   = AHCI_SECTOR_SIZE;
            d->blk.ops          = &g_ahci_ops;
            d->blk.driver_data  = d;
            d->blk.next         = NULL;
            blk_register(&d->blk);
            console_printf("[ahci] registered sata%u (drive=%d) sectors=%llu\n",
                           (uint64_t)idx, (int64_t)drive,
                           (unsigned long long)d->sectors);
        }
    }

    g_dev_count++;
    return 0;
}

/* 探测/初始化的公共路径。返回盘数，-1 = 没有控制器。 */
static int ahci_bringup(int do_register)
{
    struct ahci_pci_loc loc;
    uint32_t pi, p, cap, vs, cmd_before, cmd_after, bar5;
    uint64_t abar;

    g_dev_count = 0;

    if (ahci_pci_find(&loc) != 0) {
        console_puts("[ahci] no AHCI controller on PCI\n");
        return -1;
    }

    /* 命令寄存器：MEM（BAR5 是 MMIO，不开 MEM 读回来全是 0xFF）+ I/O + 总线主控 */
    cmd_before = ahci_pci_cfg_read32(loc.bus, loc.dev, loc.fn, PCI_CFG_COMMAND);
    cmd_after  = (cmd_before & 0xFFFF0000u)
               | PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
    ahci_pci_cfg_write32(loc.bus, loc.dev, loc.fn, PCI_CFG_COMMAND, cmd_after);
    console_printf("[ahci] pci %02x:%02x.%x cmd 0x%x -> 0x%x\n",
                   (uint64_t)loc.bus, (uint64_t)loc.dev, (uint64_t)loc.fn,
                   (uint64_t)cmd_before,
                   (uint64_t)ahci_pci_cfg_read32(loc.bus, loc.dev, loc.fn, PCI_CFG_COMMAND));

    /* BAR5 = ABAR（MMIO）。bit2 表示 64 位 BAR，高 32 位在偏移 0x28。 */
    bar5 = ahci_pci_cfg_read32(loc.bus, loc.dev, loc.fn, PCI_CFG_BAR5);
    if ((bar5 & 0x1u) != 0u) {
        console_printf("[ahci] BAR5 is an I/O BAR (0x%x), not an AHCI MMIO BAR\n",
                       (uint64_t)bar5);
        return -1;
    }
    abar = (uint64_t)(bar5 & 0xFFFFFFF0u);
    if ((bar5 & 0x4u) != 0u) {
        const uint32_t hi = ahci_pci_cfg_read32(loc.bus, loc.dev, loc.fn,
                                                (uint8_t)(PCI_CFG_BAR5 + 4u));
        abar |= (uint64_t)hi << 32;
    }
    if (abar == 0u || (bar5 & 0xFFFFFFF0u) == 0xFFFFFFF0u) {
        console_printf("[ahci] BAR5 unassigned (0x%x), cannot map ABAR\n", (uint64_t)bar5);
        return -1;
    }

    /* ★ ABAR 必须在第一次 MMIO 访问之前做 2 MiB 页恒等映射：QEMU 把 ABAR
     * 放在 0xFEBD0000 一类 >1 GiB 的地址上，不映射第一次读就 #PF。
     * AHCI MMIO 空间是 0x1100 字节，按任务要求映射 0x2000（2 MiB 页粒度）。 */
    if (mmu_map_mmio_identity(abar, 0x2000u) != 0) {
        console_printf("[ahci] mmu_map_mmio_identity(abar=0x%x) failed\n",
                       (unsigned long long)abar);
        return -1;
    }
    g_abar = abar;
    g_ctrl_bus = loc.bus;
    g_ctrl_dev = loc.dev;
    g_ctrl_fn  = loc.fn;

    /* ---- BIOS/OS Handoff（真机固件可能还持有控制器） ---- */
    if ((ahci_mmio_rd(AHCI_CAP2) & AHCI_CAP2_BOH) != 0u) {
        uint32_t bohc = ahci_mmio_rd(AHCI_BOHC);
        if ((bohc & AHCI_BOHC_BOS) != 0u) {
            const int can_hlt = ahci_irqs_enabled();
            uint32_t i;
            uint32_t spin_budget = can_hlt ? AHCI_SPINS_BEFORE_HLT : AHCI_SPIN_MEDIUM;
            uint32_t hlt_budget  = can_hlt ? AHCI_HLT_WAITS : 0u;
            ahci_mmio_wr(AHCI_BOHC, bohc | AHCI_BOHC_OOS | AHCI_BOHC_OOC);
            for (i = 0; i < spin_budget + hlt_budget; i++) {
                if ((ahci_mmio_rd(AHCI_BOHC) & AHCI_BOHC_BOS) == 0u)
                    break;
                if (i < spin_budget) __asm__ volatile("pause");
                else                 __asm__ volatile("hlt");
            }
            console_printf("[ahci] BIOS/OS handoff bohc=0x%x\n",
                           (uint64_t)ahci_mmio_rd(AHCI_BOHC));
        }
    }

    /* ---- HBA 复位：AE=1 -> HR=1（有界等待自清）-> AE=1 ---- */
    ahci_mmio_wr(AHCI_GHC, ahci_mmio_rd(AHCI_GHC) | AHCI_GHC_AE);
    ahci_mmio_wr(AHCI_GHC, ahci_mmio_rd(AHCI_GHC) | AHCI_GHC_HR);
    if (ahci_wait_ghc_hr() != 0)
        console_puts("[ahci] HBA reset timeout (continuing)\n");
    else
        console_puts("[ahci] HBA reset complete\n");
    ahci_mmio_wr(AHCI_GHC, ahci_mmio_rd(AHCI_GHC) | AHCI_GHC_AE);
    ahci_mmio_wr(AHCI_IS, 0xFFFFFFFFu);              /* 全局 IS 是 RW1C */

    cap = ahci_mmio_rd(AHCI_CAP);
    pi  = ahci_mmio_rd(AHCI_PI);
    vs  = ahci_mmio_rd(AHCI_VS);

    /* 固定格式（自动验收按这行 grep）：
     * `[ahci] controller at 00:1f.2 abar=0x........ (pi=0x.. np=..)` */
    console_printf("[ahci] controller at %02x:%02x.%x abar=0x%x (pi=0x%x np=%u)\n",
                   (uint64_t)loc.bus, (uint64_t)loc.dev, (uint64_t)loc.fn,
                   (unsigned long long)abar, (uint64_t)pi,
                   (uint64_t)((cap & 0x1Fu) + 1u));
    console_printf("[ahci] hba version=0x%x cap=0x%x (np=%u, 64bit=%u, sxs=%u, ccc=%u)\n",
                   (uint64_t)vs, (uint64_t)cap,
                   (uint64_t)((cap & 0x1Fu) + 1u),
                   (uint64_t)((cap >> 31) & 1u),
                   (uint64_t)((cap >> 5) & 1u),
                   (uint64_t)((cap >> 7) & 1u));

    /* ---- 逐端口：PI 位图里的端口一个不漏地检查 ---- */
    for (p = 0; p < 32u; p++) {
        if ((pi & (1u << p)) == 0u)
            continue;
        (void)ahci_bringup_port(p, do_register);
    }

    g_controller_found = 1;
    g_last_count = g_dev_count;
    console_printf("[ahci] %d AHCI drive(s) detected (registered=%d)\n",
                   (int64_t)((g_dev_count > 8) ? 8 : g_dev_count),
                   (int64_t)(do_register ? g_dev_count : 0));
    return g_dev_count;
}

/* ===================== 对外 API ===================== */
int blk_ahci_init(void)
{
    int n;

    if (g_init_done)
        return g_controller_found ? 0 : -1;      /* 幂等 */

    n = ahci_bringup(1);
    g_init_done = 1;

    if (n < 0)
        return -1;
    return 0;
}

int blk_ahci_probe(void)
{
    if (g_controller_found)
        return g_last_count;                     /* init 已探测过：直接回报 */

    return ahci_bringup(0);                      /* 只探测不注册；-1 = 无控制器 */
}

int blk_ahci_device_count(void)
{
    return g_last_count;
}
