/* SutureOS module: ATAPI (ATA Packet Interface) CD-ROM driver
 *
 * 来源 / Source: Uinxed-Kernel, drivers/block/ata/pata/atapi.c
 *               (C:\Users\fanqi\Desktop\自研操作系统\Uinxed-Kernel)
 *               许可证 / License: Apache-2.0
 *               Copyright (C) 2020 ViudiraTech, 2026 MicroFish
 * 改动 / Changes: 只参考其 ATAPI 协议流程（IDENTIFY PACKET 0xA1 -> PACKET 0xA0
 *               + CDB -> 数据阶段按设备 Byte Count 搬运 -> REQUEST SENSE 兜底），
 *               代码按 SutureOS 既有风格重写：
 *                 * 纯轮询，不依赖 IRQ14/IRQ15（本内核没有 IDE 中断处理程序，
 *                   blk_ata_init() 已把两个通道的 nIEN 置 1）；
 *                 * 所有等待都有界（ATAPI_POLL_LIMIT 次 inb），绝不死循环；
 *                 * 不使用 nsleep/中断等待，改为端口轮询，开机不引入延时；
 *                 * 扇区单位固定 2048 字节（CD-ROM 用户数据区），接入
 *                   kernel/drivers/block.h 的 blk_device 接口，只读；
 *                 * 新增位置->驱动号映射（驱动号 12..15，见 block.h）。
 *
 * 设计要点（为什么这样写）：
 *   1. PACKET 命令的字节数上限写在 LBA1/LBA2（不是 SECCOUNT），数据阶段
 *      每块的字节数由设备用 LBA1/LBA2 返回、方向/阶段由 SECCOUNT 的
 *      ireason 位给出（bit0 CoD、bit1 IO）。必须严格按设备给的字节数搬完，
 *      否则协议失去同步，后续命令全部错乱。
 *   2. 每条 READ(10) 最多搬 ATAPI_CHUNK_BLOCKS 个 2048 字节块（16 KiB），
 *      远小于 0xFFFE 的字节数上限，也避免了设备自己分块时的大小不确定。
 *   3. 出错时用 REQUEST SENSE 取回 sense key/ASC/ASCQ 并清掉设备的 sense，
 *      否则下一次命令会立刻再报错。
 */
#include "atapi.h"
#include "block.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/io.h>

/* ---- IDE 通道端口 ---- */
#define IDE_PRIMARY_IO      0x1F0
#define IDE_PRIMARY_CTRL    0x3F6
#define IDE_SECONDARY_IO    0x170
#define IDE_SECONDARY_CTRL  0x376

/* ---- 寄存器偏移（相对数据口） ---- */
#define IDE_REG_DATA        0x0
#define IDE_REG_FEATURES    0x1   /* 写：特性；读：错误 */
#define IDE_REG_SECCOUNT    0x2   /* PACKET 期间读 = ireason（CoD/IO 位） */
#define IDE_REG_LBA0        0x3
#define IDE_REG_LBA1        0x4   /* PACKET 期间读 = 字节数低 8 位 */
#define IDE_REG_LBA2        0x5   /* PACKET 期间读 = 字节数高 8 位 */
#define IDE_REG_HDDEVSEL    0x6
#define IDE_REG_COMMAND     0x7
#define IDE_REG_STATUS      0x7

/* ---- 命令 ---- */
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_PACKET          0xA0

/* ---- 状态位 ---- */
#define ATA_SR_ERR   0x01
#define ATA_SR_DRQ   0x08
#define ATA_SR_DF    0x20
#define ATA_SR_BSY   0x80

/* ---- ireason（PACKET 数据阶段，读 SECCOUNT 得到） ---- */
#define ATAPI_IREASON_COD 0x01   /* 1 = 命令/状态阶段，0 = 数据阶段 */
#define ATAPI_IREASON_IO  0x02   /* 1 = 设备->主机（读），0 = 主机->设备（写） */

/* ---- SCSI/MMC 命令 ---- */
#define GPCMD_TEST_UNIT_READY 0x00
#define GPCMD_REQUEST_SENSE   0x03
#define GPCMD_INQUIRY         0x12
#define GPCMD_READ_CAPACITY   0x25
#define GPCMD_READ_10         0x28

#define ATAPI_SECTOR_SIZE   2048u          /* CD-ROM 用户数据块 */
#define ATAPI_CHUNK_BLOCKS  8u             /* 单条 READ(10)：8 * 2048 = 16 KiB */
#define ATAPI_POLL_LIMIT    400000u        /* 单次等待的最大轮询次数（有界） */

/* 每个 IDE 位置一个实例（位置号 = 通道*2 + 主从：
 *   0 = 主通道主盘, 1 = 主通道从盘, 2 = 次通道主盘, 3 = 次通道从盘） */
struct atapi_dev {
    int      present;
    int      pos;
    int      drive;            /* blk_device 驱动号 = BLK_DRIVE_ATAPI_BASE + pos */
    uint32_t last_lba;         /* READ CAPACITY 返回的最后一个可寻址 LBA */
    uint8_t  peripheral;       /* INQUIRY 的外设类型（0x05 = CD-ROM） */
    char     model[41];
    struct blk_device dev;
};

static struct atapi_dev g_atapi[4];
static const char *const g_atapi_names[4] = { "atapi0", "atapi1", "atapi2", "atapi3" };
static int g_atapi_count;
static int g_atapi_probed;

static uint16_t atapi_io_base(int pos) {
    return (uint16_t)((pos & 2) ? IDE_SECONDARY_IO : IDE_PRIMARY_IO);
}

static uint16_t atapi_ctrl_base(int pos) {
    return (uint16_t)((pos & 2) ? IDE_SECONDARY_CTRL : IDE_PRIMARY_CTRL);
}

/* 选盘后的 ~400ns 延时：连读 4 次交替状态寄存器（只读、无副作用） */
static void atapi_delay(uint16_t ctrl) {
    for (int i = 0; i < 4; i++) (void)inb(ctrl);
}

/* 等 BSY 清零。返回 ATAPI_OK / ATAPI_ENODEV（总线浮空）/ ATAPI_ETIMEOUT */
static int atapi_wait_idle(uint16_t io) {
    for (uint32_t i = 0; i < ATAPI_POLL_LIMIT; i++) {
        uint8_t st = inb((uint16_t)(io + IDE_REG_STATUS));
        if (st == 0x00 || st == 0xFF) return ATAPI_ENODEV;
        if (!(st & ATA_SR_BSY)) return ATAPI_OK;
    }
    return ATAPI_ETIMEOUT;
}

/* 等 DRQ（BSY=0 且 DRQ=1）；ERR/DF 立即返回 ATAPI_EIO */
static int atapi_wait_drq(uint16_t io) {
    for (uint32_t i = 0; i < ATAPI_POLL_LIMIT; i++) {
        uint8_t st = inb((uint16_t)(io + IDE_REG_STATUS));
        if (st == 0x00 || st == 0xFF) return ATAPI_ENODEV;
        if (st & (ATA_SR_ERR | ATA_SR_DF)) return ATAPI_EIO;
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ)) return ATAPI_OK;
    }
    return ATAPI_ETIMEOUT;
}

/* 发命令前把设备拉回空闲：等 BSY 清零，并把上一条命令残留的“数据阶段”
 * 读干净（若残留的是命令/状态阶段（CoD=1）则不动，交给下一条命令覆盖）。
 * 不做这一步时，一条失败的 READ 会让 DRQ 一直挂着，后续所有命令错位。 */
static int atapi_drain(uint16_t io) {
    int r = atapi_wait_idle(io);
    if (r != ATAPI_OK) return r;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t st = inb((uint16_t)(io + IDE_REG_STATUS));
        if (st == 0x00 || st == 0xFF) return ATAPI_ENODEV;
        if (!(st & ATA_SR_DRQ)) return ATAPI_OK;
        if (inb((uint16_t)(io + IDE_REG_SECCOUNT)) & ATAPI_IREASON_COD) return ATAPI_OK;
        uint32_t bc = (uint32_t)inb((uint16_t)(io + IDE_REG_LBA1)) |
                      ((uint32_t)inb((uint16_t)(io + IDE_REG_LBA2)) << 8);
        if (bc == 0) bc = 65536u;
        for (uint32_t w = 0; w < bc / 2u; w++) (void)inw((uint16_t)(io + IDE_REG_DATA));
    }
    return ATAPI_OK;
}

/* 发送一条 ATAPI 包命令（CDB 12 字节）。
 * byte_count > 0 表示期望从设备读回这么多字节到 buf；0 表示无数据阶段。
 * 返回 ATAPI_OK / ATAPI_EIO / ATAPI_ETIMEOUT / ATAPI_ENODEV。 */
static int atapi_command(int pos, const uint8_t *cdb, uint32_t byte_count, void *buf) {
    const uint16_t io   = atapi_io_base(pos);
    const uint16_t ctrl = atapi_ctrl_base(pos);
    uint32_t done = 0;      /* 数据阶段已搬走的字节数（短读检查用） */
    int r;

    if (!cdb) return ATAPI_EINVAL;
    if (byte_count > 0 && !buf) return ATAPI_EINVAL;

    /* 1. 选盘：0xA0 = PACKET 设备的选盘基数，bit4 = 从盘 */
    outb((uint16_t)(io + IDE_REG_HDDEVSEL),
         (uint8_t)(0xA0u | ((uint32_t)(pos & 1) << 4)));
    atapi_delay(ctrl);
    r = atapi_drain(io);
    if (r != ATAPI_OK) return r;

    /* 2. 字节数上限写进 LBA1/LBA2，然后发 PACKET 命令 */
    {
        uint16_t limit = (byte_count > 0xFFFEu) ? 0xFFFEu : (uint16_t)byte_count;
        outb((uint16_t)(io + IDE_REG_FEATURES), 0x00);          /* PIO，不用 DMA */
        outb((uint16_t)(io + IDE_REG_SECCOUNT), 0x00);          /* PACKET 命令不用它 */
        outb((uint16_t)(io + IDE_REG_LBA1), (uint8_t)(limit & 0xFFu));
        outb((uint16_t)(io + IDE_REG_LBA2), (uint8_t)(limit >> 8));
        outb((uint16_t)(io + IDE_REG_COMMAND), ATA_CMD_PACKET);
    }

    /* 3. 等设备索取 CDB：BSY=0, DRQ=1, CoD=1 */
    r = atapi_wait_drq(io);
    if (r != ATAPI_OK) return r;
    if (!(inb((uint16_t)(io + IDE_REG_SECCOUNT)) & ATAPI_IREASON_COD)) {
        (void)atapi_drain(io);
        return ATAPI_EIO;
    }

    /* 4. 写 12 字节 CDB（数据口是 16 位，按低字节先出） */
    for (int i = 0; i < 6; i++) {
        uint16_t w = (uint16_t)((uint16_t)cdb[i * 2] |
                                ((uint16_t)cdb[i * 2 + 1] << 8));
        outw((uint16_t)(io + IDE_REG_DATA), w);
    }

    /* 5. 数据阶段：严格按设备给出的 Byte Count 搬完所有字，多余部分丢弃，
     *    绝不假设“一次搬完”——设备可以分成多个 DRQ 块。 */
    {
        uint8_t *out = (uint8_t *)buf;
        while (done < byte_count) {
            uint8_t ireason;
            uint32_t bc, words;

            r = atapi_wait_drq(io);
            if (r != ATAPI_OK) return r;
            ireason = inb((uint16_t)(io + IDE_REG_SECCOUNT));
            if (ireason & ATAPI_IREASON_COD) break;          /* 回到命令阶段：数据已完 */
            if (!(ireason & ATAPI_IREASON_IO)) {             /* 方向必须是 设备->主机 */
                (void)atapi_drain(io);
                return ATAPI_EIO;
            }
            bc = (uint32_t)inb((uint16_t)(io + IDE_REG_LBA1)) |
                 ((uint32_t)inb((uint16_t)(io + IDE_REG_LBA2)) << 8);
            if (bc == 0) bc = 65536u;                        /* 0 表示 65536 字节 */
            words = bc / 2u;
            for (uint32_t w = 0; w < words; w++) {
                uint16_t v = inw((uint16_t)(io + IDE_REG_DATA));
                uint32_t at = done + w * 2u;
                if (at + 1u < byte_count) {
                    out[at] = (uint8_t)(v & 0xFFu);
                    out[at + 1u] = (uint8_t)(v >> 8);
                }
            }
            done = (bc > byte_count - done) ? byte_count : (done + bc);
        }
    }

    /* 6. 等命令完成并检查错误位 */
    r = atapi_wait_idle(io);
    if (r != ATAPI_OK) return r;
    if (inb((uint16_t)(io + IDE_REG_STATUS)) & (ATA_SR_ERR | ATA_SR_DF)) return ATAPI_EIO;
    /* 设备提前结束数据阶段（短读）必须报错：返回“成功”但缓冲区只装了半截数据，
     * 会让上层把残留的旧内容当成文件内容读出去。 */
    if (done < byte_count) return ATAPI_EIO;
    return ATAPI_OK;
}

/* ---- 具体命令 ---- */

int atapi_test_unit_ready(int pos) {
    uint8_t cdb[12] = { 0 };
    if (pos < 0 || pos >= 4) return ATAPI_EINVAL;
    cdb[0] = GPCMD_TEST_UNIT_READY;
    return atapi_command(pos, cdb, 0, NULL);
}

int atapi_read_capacity(int pos, uint32_t *last_lba, uint32_t *block_size) {
    uint8_t cdb[12] = { 0 };
    uint8_t cap[8] = { 0 };
    int r;

    if (pos < 0 || pos >= 4) return ATAPI_EINVAL;
    cdb[0] = GPCMD_READ_CAPACITY;
    r = atapi_command(pos, cdb, 8, cap);
    if (r != ATAPI_OK) return r;
    /* 8 字节大端：前 4 字节 = 最后一个 LBA，后 4 字节 = 块大小 */
    if (last_lba) {
        *last_lba = ((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16) |
                    ((uint32_t)cap[2] << 8) | (uint32_t)cap[3];
    }
    if (block_size) {
        *block_size = ((uint32_t)cap[4] << 24) | ((uint32_t)cap[5] << 16) |
                      ((uint32_t)cap[6] << 8) | (uint32_t)cap[7];
    }
    return ATAPI_OK;
}

int atapi_request_sense(int pos, uint8_t *sense_key, uint8_t *asc, uint8_t *ascq) {
    uint8_t cdb[12] = { 0 };
    uint8_t sense[18];
    int r;

    if (pos < 0 || pos >= 4) return ATAPI_EINVAL;
    memset(sense, 0, sizeof(sense));
    cdb[0] = GPCMD_REQUEST_SENSE;
    cdb[4] = (uint8_t)sizeof(sense);          /* 分配长度 */
    r = atapi_command(pos, cdb, (uint32_t)sizeof(sense), sense);
    if (r != ATAPI_OK) return r;
    /* 定长 sense：byte2 = sense key，byte12/13 = ASC/ASCQ */
    if (sense_key) *sense_key = (uint8_t)(sense[2] & 0x0Fu);
    if (asc) *asc = sense[12];
    if (ascq) *ascq = sense[13];
    return ATAPI_OK;
}

/* INQUIRY：外设类型 + 厂商/型号字符串（VPD 页 0，36 字节足够） */
static int atapi_inquiry(int pos, uint8_t *buf, uint32_t len) {
    uint8_t cdb[12] = { 0 };
    if (pos < 0 || pos >= 4) return ATAPI_EINVAL;
    if (!buf || len == 0) return ATAPI_EINVAL;
    cdb[0] = GPCMD_INQUIRY;
    cdb[4] = (uint8_t)(len > 0xFFu ? 0xFFu : len);
    return atapi_command(pos, cdb, len, buf);
}

/* IDENTIFY PACKET DEVICE (0xA1)：不是包命令，直接返回 512 字节识别数据。
 * 返回 ATAPI_OK 才说明该位置真的是 ATAPI 设备（普通 ATA 盘会 ABRT）。 */
static int atapi_identify_packet(int pos, uint16_t *id_out) {
    const uint16_t io   = atapi_io_base(pos);
    const uint16_t ctrl = atapi_ctrl_base(pos);
    int r;

    if (!id_out) return ATAPI_EINVAL;
    outb((uint16_t)(io + IDE_REG_HDDEVSEL),
         (uint8_t)(0xA0u | ((uint32_t)(pos & 1) << 4)));
    atapi_delay(ctrl);
    r = atapi_wait_idle(io);
    if (r != ATAPI_OK) return r;

    outb((uint16_t)(io + IDE_REG_SECCOUNT), 0);
    outb((uint16_t)(io + IDE_REG_LBA0), 0);
    outb((uint16_t)(io + IDE_REG_LBA1), 0);
    outb((uint16_t)(io + IDE_REG_LBA2), 0);
    outb((uint16_t)(io + IDE_REG_COMMAND), ATA_CMD_IDENTIFY_PACKET);
    atapi_delay(ctrl);

    {
        uint8_t st = inb((uint16_t)(io + IDE_REG_STATUS));
        if (st == 0x00 || st == 0xFF) return ATAPI_ENODEV;
    }
    r = atapi_wait_drq(io);
    if (r != ATAPI_OK) return r;
    for (int i = 0; i < 256; i++) id_out[i] = inw((uint16_t)(io + IDE_REG_DATA));
    return ATAPI_OK;
}

/* IDENTIFY PACKET 的 word 27..46 是型号字符串，每字内两字节是反序的 */
static void atapi_model_from_identify(const uint16_t *id, char *out, uint32_t cap) {
    uint32_t n = 0;
    if (!out || cap == 0) return;
    for (uint32_t k = 0; k + 1 < cap && k < 40u; k++) {
        uint16_t w = id[27 + k / 2u];
        out[k] = (char)((k & 1u) ? (uint8_t)(w & 0xFFu) : (uint8_t)(w >> 8));
        n = k + 1;
    }
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0')) out[--n] = '\0';
}

/* ==================== 块设备接口（只读） ==================== */

static int atapi_dev_read(struct blk_device *dev, uint64_t lba, uint32_t count, void *buf) {
    int pos;
    if (!dev || !buf || count == 0) return -1;
    pos = (int)(uintptr_t)dev->driver_data;
    if (pos < 0 || pos >= 4 || !g_atapi[pos].present) return -1;
    if (lba > (uint64_t)g_atapi[pos].last_lba ||
        (uint64_t)count - 1u > (uint64_t)g_atapi[pos].last_lba - lba) {
        console_printf("[atapi] drive %d: read beyond capacity lba=%llu count=%u"
                       " blocks=%llu\n", dev->drive, lba, count,
                       (uint64_t)g_atapi[pos].last_lba + 1u);
        return -1;
    }
    if (atapi_read_blocks(pos, (uint32_t)lba, count, buf) != ATAPI_OK) return -1;
    return 0;
}

static int atapi_dev_write(struct blk_device *dev, uint64_t lba, uint32_t count,
                           const void *buf) {
    (void)dev; (void)lba; (void)count; (void)buf;
    return -1;   /* 只读：ISO9660 是只读文件系统 */
}

static int atapi_dev_flush(struct blk_device *dev) {
    (void)dev;
    return 0;    /* 只读设备没有写缓存要刷 */
}

static const struct blk_ops g_atapi_ops = {
    .read  = atapi_dev_read,
    .write = atapi_dev_write,
    .flush = atapi_dev_flush,
    .ioctl = NULL,
};

int atapi_read_blocks(int pos, uint32_t lba, uint32_t count, void *buf) {
    struct atapi_dev *d;
    uint8_t *p = (uint8_t *)buf;

    if (pos < 0 || pos >= 4) return ATAPI_EINVAL;
    d = &g_atapi[pos];
    if (!d->present) return ATAPI_ENODEV;
    if (!p || count == 0) return ATAPI_EINVAL;
    if (lba > d->last_lba || count - 1u > d->last_lba - lba) return ATAPI_EINVAL;

    while (count > 0) {
        uint32_t chunk = (count > ATAPI_CHUNK_BLOCKS) ? ATAPI_CHUNK_BLOCKS : count;
        uint8_t cdb[12] = { 0 };
        int r;

        cdb[0] = GPCMD_READ_10;
        cdb[2] = (uint8_t)((lba >> 24) & 0xFFu);
        cdb[3] = (uint8_t)((lba >> 16) & 0xFFu);
        cdb[4] = (uint8_t)((lba >> 8) & 0xFFu);
        cdb[5] = (uint8_t)(lba & 0xFFu);
        cdb[7] = (uint8_t)((chunk >> 8) & 0xFFu);
        cdb[8] = (uint8_t)(chunk & 0xFFu);
        r = atapi_command(pos, cdb, chunk * ATAPI_SECTOR_SIZE, p);
        if (r != ATAPI_OK) {
            uint8_t key = 0, asc = 0, ascq = 0;
            (void)atapi_request_sense(pos, &key, &asc, &ascq);
            console_printf("[atapi] drive %d: READ(10) failed lba=%u blocks=%u"
                           " sense=%x/%x/%x\n", d->drive, lba, chunk,
                           (uint64_t)key, (uint64_t)asc, (uint64_t)ascq);
            return r;
        }
        lba += chunk;
        count -= chunk;
        p += (uint32_t)chunk * ATAPI_SECTOR_SIZE;
    }
    return ATAPI_OK;
}

/* ==================== 探测 ==================== */

int blk_atapi_device_count(void) {
    return g_atapi_count;
}

int atapi_drive_of_position(int pos) {
    if (pos < 0 || pos >= 4) return -1;
    return BLK_DRIVE_ATAPI_BASE + pos;
}

int atapi_position_of_drive(int drive) {
    int pos = drive - BLK_DRIVE_ATAPI_BASE;
    return (pos >= 0 && pos < 4) ? pos : -1;
}

int blk_atapi_drive_at(int index) {
    int seen = 0;
    if (index < 0) return -1;
    for (int pos = 0; pos < 4; pos++) {
        if (!g_atapi[pos].present) continue;
        if (seen == index) return g_atapi[pos].drive;
        seen++;
    }
    return -1;
}

/* 静默探测一个位置：只有真的存在 ATAPI 光盘才打印一行 [atapi] 日志。
 * 空位（状态 0x00/0xFF）与“不是 ATAPI 的设备”（IDENTIFY PACKET 被 ABRT）
 * 都直接跳过、不打印，因此没有光驱时开机日志与改动前完全一致。 */
static void atapi_probe_position(int pos) {
    const uint16_t io   = atapi_io_base(pos);
    const uint16_t ctrl = atapi_ctrl_base(pos);
    struct atapi_dev *d = &g_atapi[pos];
    uint16_t id[256];
    uint8_t inquiry[36];
    uint32_t last_lba = 0, block_size = 0;
    uint8_t key = 0, asc = 0, ascq = 0;
    int r;

    /* 该位置已被 ATA PIO 注册成硬盘（drive 0/1）就不要去惊动它 */
    if (blk_find_by_drive(pos)) return;

    outb((uint16_t)(io + IDE_REG_HDDEVSEL),
         (uint8_t)(0xA0u | ((uint32_t)(pos & 1) << 4)));
    atapi_delay(ctrl);
    {
        uint8_t st = inb((uint16_t)(io + IDE_REG_STATUS));
        if (st == 0x00 || st == 0xFF) return;     /* 空位：安静跳过 */
    }

    if (atapi_identify_packet(pos, id) != ATAPI_OK) return;   /* 不是 ATAPI：静默跳过 */
    atapi_model_from_identify(id, d->model, sizeof(d->model));

    r = atapi_test_unit_ready(pos);
    if (r != ATAPI_OK) {
        (void)atapi_request_sense(pos, &key, &asc, &ascq);
        console_printf("[atapi] drive %d: \"%s\" no media (sense=%x/%x/%x)\n",
                       atapi_drive_of_position(pos), d->model,
                       (uint64_t)key, (uint64_t)asc, (uint64_t)ascq);
        return;
    }

    r = atapi_read_capacity(pos, &last_lba, &block_size);
    if (r != ATAPI_OK) {
        (void)atapi_request_sense(pos, &key, &asc, &ascq);
        console_printf("[atapi] drive %d: \"%s\" READ CAPACITY failed"
                       " (sense=%x/%x/%x)\n", atapi_drive_of_position(pos), d->model,
                       (uint64_t)key, (uint64_t)asc, (uint64_t)ascq);
        return;
    }
    if (block_size != ATAPI_SECTOR_SIZE) {
        console_printf("[atapi] drive %d: \"%s\" unsupported block size %u"
                       " (only 2048-byte CD-ROM)\n", atapi_drive_of_position(pos),
                       d->model, (uint64_t)block_size);
        return;
    }

    memset(inquiry, 0, sizeof(inquiry));
    d->peripheral = 0xFF;
    if (atapi_inquiry(pos, inquiry, (uint32_t)sizeof(inquiry)) == ATAPI_OK)
        d->peripheral = (uint8_t)(inquiry[0] & 0x1Fu);

    d->present = 1;
    d->pos = pos;
    d->drive = atapi_drive_of_position(pos);
    d->last_lba = last_lba;

    d->dev.name         = g_atapi_names[pos];
    d->dev.type         = BLK_TYPE_ATAPI;
    d->dev.drive        = d->drive;
    d->dev.flags        = BLK_FLAG_READONLY | BLK_FLAG_REMOVABLE;
    d->dev.sector_count = (uint64_t)last_lba + 1u;
    d->dev.sector_size  = ATAPI_SECTOR_SIZE;
    d->dev.block_size   = ATAPI_SECTOR_SIZE;
    d->dev.ops          = &g_atapi_ops;
    d->dev.driver_data  = (void *)(uintptr_t)pos;
    d->dev.next         = NULL;

    if (blk_register(&d->dev) != 0) {
        console_printf("[atapi] drive %d: \"%s\" register failed\n", d->drive, d->model);
        d->present = 0;
        return;
    }
    g_atapi_count++;
    console_printf("[atapi] drive %d: %s \"%s\" %llu blocks x 2048B\n",
                   d->drive,
                   (d->peripheral == 0x05) ? "CD-ROM" : "ATAPI",
                   d->model, (uint64_t)last_lba + 1u);
}

int blk_atapi_probe(void) {
    if (g_atapi_probed) return g_atapi_count;
    g_atapi_probed = 1;

    /* 纯轮询：把两个通道的 nIEN 置 1（与 blk_ata_init 一致，独立调用也安全） */
    outb(IDE_PRIMARY_CTRL, 0x02);
    outb(IDE_SECONDARY_CTRL, 0x02);

    for (int pos = 0; pos < 4; pos++) atapi_probe_position(pos);
    return g_atapi_count;
}
