/* SutureOS module: NVMe block driver (NVMe 1.x, polled, single queue pair)
 * License: GPL-3.0 (same as SutureOS).
 *
 * Adapted for SutureOS from three upstream NVMe drivers; the controller
 * register map, queue layout, CQE/SQE layout and the admin Create CQ/SQ
 * command fields were cross-checked between them. This file is a fresh
 * freestanding C11 implementation, not a copy of any of them:
 *   1. CoolPotOS  module/nvme/nvme.c            (GPL-3.0)
 *        - admin/IO queue bring-up, Identify, CQE status (SC/SCT) decoding,
 *          Create CQ/SQ CDW10/CDW11 field layout.
 *   2. VimtuOS    kernel/nvme64.cpp             (GPL-3.0)
 *        - PCI class 01/08/prog-if 02 scan, CAP DSTRD doorbell stride,
 *          phase-bit completion polling, AQA(0-based)/ASQ/ACQ + CC layout,
 *          PRP1-only transfers, bounded timeouts.
 *   3. Cinux      kernel/drivers/nvme/nvme.cpp  (MIT)
 *        - BAR0 handling (64-bit BAR, and the "firmware left BAR0 unassigned"
 *          case), CC IOSQES/IOCQES shifts (16/20), CC.EN wait loops.
 * Upstream paths are relative to C:\Users\fanqi\Desktop\自研操作系统\.
 *
 * Design (matching this tree's conventions, cf. kernel/drivers/rtl8139.c):
 *   - Every DMA-visible structure is a static .bss object aligned to 4 KiB, so
 *     virtual == physical (the kernel links at 1 MiB and identity-maps the low
 *     1 GiB) and no allocator is involved. A later heap re-init cannot move a
 *     queue out from under the controller.
 *   - The kernel does not program MTRRs, so clflush is used on every structure
 *     the device reads (SQ entry before the doorbell) and on everything the
 *     device wrote (CQE, bounce buffer) before the CPU consumes it. clflush is
 *     a no-op for correctness under QEMU's cache-coherent device model and
 *     required on real silicon.
 *   - Pure polling: no MSI-X, no interrupts, every wait is bounded.
 *   - One admin queue pair (qid 0) and one I/O queue pair (qid 1). All I/O
 *     goes through a one-page bounce buffer, so PRP1 alone is enough and every
 *     command transfers at most one page (larger requests are split).
 *
 * Known limitation: mmu_map_mmio_identity() maps the BAR with 2 MiB pages that
 * are cacheable (it has no PCD/PWT option in this tree). QEMU's device model is
 * coherent so this is correct there; on real hardware MTRRs normally type the
 * PCI hole uncached, but a machine that left it WB would need PTE_PCD on the
 * BAR mapping. mmu.c is outside this driver's write scope.
 */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/io.h>

#include "block.h"
#include "mmu.h"
#include "nvme.h"

/* ==================== controller registers (NVMe 1.4, section 3.1) ==================== */
#define NVME_REG_CAP    0x0000u   /* Controller Capabilities (64-bit)          */
#define NVME_REG_VS     0x0008u   /* Version                                   */
#define NVME_REG_INTMS  0x000Cu   /* Interrupt Mask Set (write 1 = mask)       */
#define NVME_REG_INTMC  0x0010u   /* Interrupt Mask Clear                      */
#define NVME_REG_CC     0x0014u   /* Controller Configuration                  */
#define NVME_REG_CSTS   0x001Cu   /* Controller Status                         */
#define NVME_REG_AQA    0x0024u   /* Admin Queue Attributes                    */
#define NVME_REG_ASQ    0x0028u   /* Admin Submission Queue base (64-bit)      */
#define NVME_REG_ACQ    0x0030u   /* Admin Completion Queue base (64-bit)      */
#define NVME_REG_DBS    0x1000u   /* Doorbell base; per-queue stride = 4<<DSTRD */

/* CC fields (spec 3.1.4): EN[0], CSS[6:4], MPS[10:7], SHN[15:14],
 * IOSQES[19:16], IOCQES[23:20]. */
#define NVME_CC_EN              (1u << 0)
#define NVME_CC_CSS_NVM         (0u << 4)   /* NVM command set */
#define NVME_CC_MPS_SHIFT       7u
#define NVME_CC_IOSQES_SHIFT    16u         /* 6 -> 64-byte SQ entries   */
#define NVME_CC_IOCQES_SHIFT    20u         /* 4 -> 16-byte CQ entries   */
#define NVME_CC_IOSQES_64B      (6u << NVME_CC_IOSQES_SHIFT)
#define NVME_CC_IOCQES_16B      (4u << NVME_CC_IOCQES_SHIFT)

/* CSTS fields: RDY[0], CFS[1]. */
#define NVME_CSTS_RDY           (1u << 0)
#define NVME_CSTS_CFS           (1u << 1)

/* Command opcodes (only the ones this driver issues). */
#define NVME_ADMIN_CREATE_SQ    0x01u   /* Admin: Create I/O Submission Queue */
#define NVME_OP_WRITE           0x01u   /* NVM:   Write                       */
#define NVME_OP_READ            0x02u   /* NVM:   Read                        */
#define NVME_ADMIN_CREATE_CQ    0x05u   /* Admin: Create I/O Completion Queue */
#define NVME_ADMIN_IDENTIFY     0x06u   /* Admin: Identify                    */
#define NVME_OP_FLUSH           0x00u   /* NVM:   Flush                       */

/* Identify CNS values. Note: 0x02 is the Active Namespace ID list, NOT
 * Identify Namespace; Identify Namespace data (NSZE/LBAF) is CNS = 0x00. */
#define NVME_CNS_NAMESPACE      0x00u
#define NVME_CNS_CONTROLLER     0x01u

/* ==================== PCI configuration space ==================== */
#define NVME_PCI_CFG_ADDR       0xCF8u
#define NVME_PCI_CFG_DATA       0xCFCu
#define NVME_PCI_VENDOR_ID      0x00u
#define NVME_PCI_COMMAND        0x04u
#define NVME_PCI_REV_CLASS      0x08u
#define NVME_PCI_HEADER_TYPE    0x0Cu
#define NVME_PCI_BAR0           0x10u
#define NVME_PCI_BAR1           0x14u

#define NVME_PCI_CMD_MEM_SPACE  (1u << 1)
#define NVME_PCI_CMD_BUS_MASTER (1u << 2)

#define NVME_PCI_CLASS_STORAGE  0x01u
#define NVME_PCI_SUBCLASS_NVM   0x08u
#define NVME_PCI_PROGIF_NVME    0x02u

/* QEMU's NVMe controller (hw/nvme). Accepted in addition to the class check so
 * a device that reports an unexpected class code is still driven. */
#define NVME_QEMU_VENDOR_ID     0x1B36u
#define NVME_QEMU_DEVICE_ID     0x0010u

/* BAR0 fallback slot if firmware left BAR0 unassigned (see nvme_pci_bar0()). */
#define NVME_BAR0_FALLBACK      0xFEB40000ull

/* ==================== geometry and limits ==================== */
#define NVME_PAGE_SIZE          4096u
#define NVME_ADMIN_QD           16u     /* admin queue entries                */
#define NVME_IO_QD              64u     /* I/O queue entries (1 page of 64 B) */
#define NVME_BOUNCE_SIZE        NVME_PAGE_SIZE

/* Bounded spins. Every wait in this driver is bounded; QEMU completes each
 * handshake in a few hundred iterations. The reset budget is far larger
 * because a real controller may take hundreds of milliseconds. */
#define NVME_RESET_SPINS        50000000u
#define NVME_CMD_SPINS          10000000u

/* Drive number reservation (see block.h "驱动器号分配"): ata0/ata1 = 0/1,
 * AHCI/SATA = 4..7, ram0 = 9, VirtIO-blk = 10. NVMe owns 8 here. */
#define NVME_DRIVE_NVME0        8

/* Identify payload byte offsets (NVMe spec, Identify data structures). */
#define NVME_ID_CTRL_MN         24u     /* Model Number, 40 bytes             */
#define NVME_ID_CTRL_NN         516u    /* Number of Namespaces (uint32)      */
#define NVME_ID_NS_NSZE         0u      /* Namespace Size, in LBAs (uint64)   */
#define NVME_ID_NS_FLBAS        26u     /* Formatted LBA Size (byte)          */
#define NVME_ID_NS_LBAF         128u    /* LBA Format 0 base; LBADS = [23:16] */

/* ==================== command / completion structures ==================== */
/* Submission Queue Entry: exactly 64 bytes (CC.IOSQES = 6). */
typedef struct __attribute__((packed)) {
    uint32_t cdw0;      /* opcode[7:0] | fuse[9:8] | cid[31:16] */
    uint32_t nsid;
    uint64_t rsvd1;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_sqe_t;

/* Completion Queue Entry: exactly 16 bytes (CC.IOCQES = 4).
 * DW3 = cid[15:0] | status_field[31:16]; status_field bit0 is the phase tag,
 * bits 8:1 = SC (status code), bits 11:9 = SCT (status code type). */
typedef struct __attribute__((packed)) {
    uint32_t result;    /* command specific */
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} nvme_cqe_t;

/* One queue: SQ and CQ live in separate 4 KiB-aligned static pages. */
typedef struct {
    uint8_t* sq;
    uint8_t* cq;
    uint16_t qid;
    uint16_t qd;        /* entries; also the ring modulo                 */
    uint16_t sq_tail;
    uint16_t cq_head;
    uint32_t phase;     /* expected phase tag of the next completion     */
    uint16_t cid;       /* command id counter (one command in flight)    */
} nvme_queue_t;

/* ==================== DMA-visible static memory (.bss) ====================
 * 4 KiB aligned; virtual == physical because the kernel identity-maps the low
 * 1 GiB and links at 1 MiB. No heap, no page allocator: a later PMM/heap
 * re-init cannot move a queue or the bounce buffer. */
static uint8_t g_admin_sq[NVME_PAGE_SIZE]  __attribute__((aligned(4096)));
static uint8_t g_admin_cq[NVME_PAGE_SIZE]  __attribute__((aligned(4096)));
static uint8_t g_io_sq[NVME_PAGE_SIZE]     __attribute__((aligned(4096)));
static uint8_t g_io_cq[NVME_PAGE_SIZE]     __attribute__((aligned(4096)));
static uint8_t g_identify[NVME_PAGE_SIZE]  __attribute__((aligned(4096)));
static uint8_t g_bounce[NVME_BOUNCE_SIZE]  __attribute__((aligned(4096)));

/* ==================== driver state ==================== */
static volatile uint8_t* g_mmio;             /* BAR0 (phys == virt)          */
static uint64_t g_mmio_phys;
static uint8_t  g_bus, g_dev, g_fn;
static uint64_t g_cap;
static uint32_t g_vs;
static uint32_t g_mqes;                      /* CAP.MQES (0-based max)       */
static uint32_t g_doorbell_stride;           /* 4 << CAP.DSTRD               */

static nvme_queue_t g_admin;
static nvme_queue_t g_io;

/* 0 = never touched, 1 = controller + namespace ready, 2 = controller up but no
 * usable namespace, -1 = no controller / bring-up failed. Caching the result
 * makes both blk_nvme_init() and blk_nvme_probe() idempotent. */
static int g_init_state;
static int g_registered;
static int g_ns_count;
static uint32_t g_ns_lba_bytes;              /* namespace LBA size           */
static uint32_t g_ns_spb;                    /* 512-byte sectors per LBA     */
static uint64_t g_ns_sectors;                /* namespace size in 512B units */
static char g_model[41];
static struct blk_device g_nvme_dev;

/* ==================== small helpers ==================== */
static inline void nvme_pause(void)
{
    __asm__ volatile("pause" ::: "memory");
}

/* Invalidate/write back the cache lines covering [addr, addr+len). Called on
 * structures the device reads (before the doorbell) and on structures the
 * device wrote (before the CPU reads them). */
static void nvme_clflush(const void* addr, uint32_t len)
{
    uintptr_t p = (uintptr_t)addr & ~(uintptr_t)63u;
    uintptr_t end = (uintptr_t)addr + (uintptr_t)len;

    while (p < end) {
        __asm__ volatile("clflush (%0)" :: "r"(p) : "memory");
        p += 64u;
    }
}

static uint32_t nvme_le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t nvme_le64(const uint8_t* p)
{
    return (uint64_t)nvme_le32(p) | ((uint64_t)nvme_le32(p + 4u) << 32);
}

/* Copy a fixed-width ASCII field, stopping at NUL and trimming trailing blanks. */
static void nvme_copy_ascii(char* dst, size_t dstsz, const uint8_t* src, uint32_t n)
{
    uint32_t i = 0;

    while (i < n && (size_t)i + 1u < dstsz && src[i] != 0u) {
        dst[i] = (char)src[i];
        i++;
    }
    dst[i] = '\0';
    while (i > 0u && dst[i - 1u] == ' ') {
        dst[--i] = '\0';
    }
}

/* ==================== MMIO accessors ==================== */
/* The trailing empty asm with a memory clobber keeps the compiler from
 * reordering, merging or eliding device register accesses. */
static inline uint32_t nvme_rd32(uint32_t off)
{
    return *(volatile uint32_t*)(uintptr_t)(g_mmio + off);
}

static inline uint64_t nvme_rd64(uint32_t off)
{
    return *(volatile uint64_t*)(uintptr_t)(g_mmio + off);
}

static inline void nvme_wr32(uint32_t off, uint32_t v)
{
    *(volatile uint32_t*)(uintptr_t)(g_mmio + off) = v;
    __asm__ volatile("" ::: "memory");
}

static inline void nvme_wr64(uint32_t off, uint64_t v)
{
    *(volatile uint64_t*)(uintptr_t)(g_mmio + off) = v;
    __asm__ volatile("" ::: "memory");
}

/* Doorbell offset: SQ tail = base + 2*qid*stride, CQ head = base + (2*qid+1)*stride. */
static inline uint32_t nvme_doorbell_off(uint16_t qid, int is_cq)
{
    return NVME_REG_DBS + (uint32_t)(2u * (uint32_t)qid + (is_cq ? 1u : 0u)) * g_doorbell_stride;
}

/* ==================== PCI config access ====================
 * Mechanism #1 needs DWORD accesses: writing the address register 0xCF8
 * byte-by-byte does not stick on QEMU (every slot then reads back the same
 * constant). stitch/io.h only has byte/word helpers, hence the local dword
 * helpers - same approach as kernel/drivers/rtl8139.c. */
static inline void nvme_pci_outl(uint16_t port, uint32_t v)
{
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t nvme_pci_inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint32_t nvme_pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8)
                  | (uint32_t)(off & 0xFCu);

    nvme_pci_outl(NVME_PCI_CFG_ADDR, addr);
    return nvme_pci_inl(NVME_PCI_CFG_DATA);
}

static void nvme_pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8)
                  | (uint32_t)(off & 0xFCu);

    nvme_pci_outl(NVME_PCI_CFG_ADDR, addr);
    nvme_pci_outl(NVME_PCI_CFG_DATA, v);
}

/* Scan every bus/device/function for an NVMe controller. Primary match is the
 * class code (01h/08h/prog-if 02h); the QEMU vendor/device pair is accepted as
 * a fallback. Four consecutive empty buses end the scan early. Returns 0 and
 * fills bus/dev/fn when found, -1 otherwise. */
static int nvme_pci_find(uint8_t* bus_out, uint8_t* dev_out, uint8_t* fn_out)
{
    uint32_t empty_run = 0;

    for (uint32_t bus = 0; bus < 256u; bus++) {
        int bus_has_device = 0;

        for (uint32_t d = 0; d < 32u; d++) {
            uint32_t id = nvme_pci_read32((uint8_t)bus, (uint8_t)d, 0u, NVME_PCI_VENDOR_ID);
            uint16_t vendor = (uint16_t)(id & 0xFFFFu);

            if (vendor == 0xFFFFu || vendor == 0u)
                continue;
            bus_has_device = 1;

            /* Multi-function devices hide function 0's siblings behind bit 7 of
             * the header type; only scan the extra functions when it is set. */
            uint32_t hdr = nvme_pci_read32((uint8_t)bus, (uint8_t)d, 0u, NVME_PCI_HEADER_TYPE);
            uint32_t nfn = (hdr & 0x00800000u) ? 8u : 1u;

            for (uint32_t fn = 0; fn < nfn; fn++) {
                if (fn != 0u) {
                    id = nvme_pci_read32((uint8_t)bus, (uint8_t)d, (uint8_t)fn, NVME_PCI_VENDOR_ID);
                    vendor = (uint16_t)(id & 0xFFFFu);
                    if (vendor == 0xFFFFu || vendor == 0u)
                        continue;
                }

                uint16_t device = (uint16_t)((id >> 16) & 0xFFFFu);
                uint32_t cc = nvme_pci_read32((uint8_t)bus, (uint8_t)d, (uint8_t)fn,
                                              NVME_PCI_REV_CLASS);
                uint32_t cls = (cc >> 24) & 0xFFu;
                uint32_t sub = (cc >> 16) & 0xFFu;
                uint32_t pif = (cc >> 8) & 0xFFu;

                if ((cls == NVME_PCI_CLASS_STORAGE && sub == NVME_PCI_SUBCLASS_NVM &&
                     pif == NVME_PCI_PROGIF_NVME) ||
                    (vendor == NVME_QEMU_VENDOR_ID && device == NVME_QEMU_DEVICE_ID)) {
                    *bus_out = (uint8_t)bus;
                    *dev_out = (uint8_t)d;
                    *fn_out  = (uint8_t)fn;
                    return 0;
                }
            }
        }

        if (bus_has_device) {
            empty_run = 0;
        } else if (++empty_run >= 4u) {
            break;
        }
    }
    return -1;
}

/* Read BAR0 (64-bit capable: a 64-bit memory BAR sets bit 2 and puts the high
 * dword in BAR1). If firmware left BAR0 unassigned (address 0) the controller
 * is unreachable, so probe the BAR size and self-assign the slot the Cinux
 * reference driver uses. This only runs when BAR0 is genuinely 0, so a working
 * firmware assignment is never disturbed. */
static uint64_t nvme_pci_bar0(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint32_t lo = nvme_pci_read32(bus, dev, fn, NVME_PCI_BAR0);
    uint32_t hi = 0;
    int is64 = (lo & 0x4u) != 0u;

    if (is64)
        hi = nvme_pci_read32(bus, dev, fn, NVME_PCI_BAR1);

    uint64_t bar = ((uint64_t)hi << 32) | (uint64_t)(lo & 0xFFFFFFF0u);
    if (bar != 0u)
        return bar;

    const uint32_t flags = lo & 0xFu;

    nvme_pci_write32(bus, dev, fn, NVME_PCI_BAR0, 0xFFFFFFFFu);
    if (is64)
        nvme_pci_write32(bus, dev, fn, NVME_PCI_BAR1, 0xFFFFFFFFu);

    uint32_t probe = nvme_pci_read32(bus, dev, fn, NVME_PCI_BAR0);
    uint32_t mask = probe & 0xFFFFFFF0u;
    uint64_t size = mask ? (~(uint64_t)mask + 1ull) : 0x4000ull;
    if (size < 0x4000ull)
        size = 0x4000ull;

    nvme_pci_write32(bus, dev, fn, NVME_PCI_BAR0, (uint32_t)(NVME_BAR0_FALLBACK | flags));
    if (is64)
        nvme_pci_write32(bus, dev, fn, NVME_PCI_BAR1, 0u);

    console_printf("[nvme] BAR0 was unassigned: self-assigned 0x%x (size=0x%llx)\n",
                   (uint64_t)NVME_BAR0_FALLBACK, (uint64_t)size);
    return NVME_BAR0_FALLBACK;
}

/* ==================== queues and command submission ==================== */
static void nvme_queue_init(nvme_queue_t* q, uint16_t qid, uint16_t qd,
                            uint8_t* sq, uint8_t* cq)
{
    q->sq      = sq;
    q->cq      = cq;
    q->qid     = qid;
    q->qd      = qd;
    q->sq_tail = 0;
    q->cq_head = 0;
    q->phase   = 1u;
    q->cid     = 0;
}

/* Copy one 64-byte command into the SQ, flush it, ring the SQ tail doorbell and
 * poll the CQ for its completion. Returns 0 on success, -1 on device error or
 * timeout. One command is in flight at a time, so the next CQE is ours. */
static int nvme_submit(nvme_queue_t* q, const nvme_sqe_t* cmd, uint8_t opcode)
{
    uint8_t* slot = q->sq + (uintptr_t)q->sq_tail * 64u;

    memcpy(slot, cmd, 64u);
    nvme_clflush(slot, 64u);

    q->sq_tail = (uint16_t)((q->sq_tail + 1u) % q->qd);
    nvme_wr32(nvme_doorbell_off(q->qid, 0), (uint32_t)q->sq_tail);

    for (uint32_t spins = 0; spins < NVME_CMD_SPINS; spins++) {
        volatile nvme_cqe_t* cqe =
            (volatile nvme_cqe_t*)(q->cq + (uintptr_t)q->cq_head * 16u);

        /* Invalidate the line the device may have written before reading it. */
        nvme_clflush((const void*)(uintptr_t)cqe, 16u);

        uint16_t st = cqe->status;
        if ((uint32_t)(st & 0x1u) == q->phase) {
            uint16_t status_field = (uint16_t)(st & 0xFFFEu);   /* phase removed */
            uint16_t sc  = (uint16_t)((st >> 1) & 0xFFu);
            uint16_t sct = (uint16_t)((st >> 9) & 0x7u);

            q->cq_head++;
            if (q->cq_head >= q->qd) {
                q->cq_head = 0;
                q->phase ^= 1u;
            }
            nvme_wr32(nvme_doorbell_off(q->qid, 1), (uint32_t)q->cq_head);

            if (sc != 0u || sct != 0u) {
                console_printf("[nvme] command 0x%x failed: status=0x%x\n",
                               (uint64_t)opcode, (uint64_t)status_field);
                return -1;
            }
            return 0;
        }
        nvme_pause();
    }

    console_printf("[nvme] command 0x%x timed out (csts=0x%x cq_head=%u phase=%u)\n",
                   (uint64_t)opcode, (uint64_t)nvme_rd32(NVME_REG_CSTS),
                   (uint64_t)q->cq_head, (uint64_t)q->phase);
    return -1;
}

/* Admin Identify (opcode 0x06): CNS selects Controller (NSID 0) or Namespace. */
static int nvme_admin_identify(uint32_t cns, uint32_t nsid, const uint8_t* buf)
{
    nvme_sqe_t cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = (uint32_t)NVME_ADMIN_IDENTIFY | ((uint32_t)(g_admin.cid++) << 16);
    cmd.nsid  = nsid;
    cmd.prp1  = (uint64_t)(uintptr_t)buf;               /* one page: PRP1 only */
    cmd.cdw10 = cns;
    return nvme_submit(&g_admin, &cmd, NVME_ADMIN_IDENTIFY);
}

/* Admin Create I/O Completion Queue (opcode 0x05).
 * CDW10 = QID | (QSIZE-1)<<16, CDW11 = IEN<<1 | PC, with PC = 1 (physically
 * contiguous) and IEN = 0 (no interrupts: this driver polls). */
static int nvme_create_io_cq(nvme_queue_t* q)
{
    nvme_sqe_t cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = (uint32_t)NVME_ADMIN_CREATE_CQ | ((uint32_t)(g_admin.cid++) << 16);
    cmd.prp1  = (uint64_t)(uintptr_t)q->cq;
    cmd.cdw10 = ((uint32_t)(q->qd - 1u) << 16) | (uint32_t)q->qid;
    cmd.cdw11 = 1u;                                     /* PC=1, IEN=0, IV=0 */
    return nvme_submit(&g_admin, &cmd, NVME_ADMIN_CREATE_CQ);
}

/* Admin Create I/O Submission Queue (opcode 0x01).
 * CDW10 = QID | (QSIZE-1)<<16, CDW11 = CQID<<16 | QPRIO<<1 | PC. */
static int nvme_create_io_sq(nvme_queue_t* q)
{
    nvme_sqe_t cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = (uint32_t)NVME_ADMIN_CREATE_SQ | ((uint32_t)(g_admin.cid++) << 16);
    cmd.prp1  = (uint64_t)(uintptr_t)q->sq;
    cmd.cdw10 = ((uint32_t)(q->qd - 1u) << 16) | (uint32_t)q->qid;
    cmd.cdw11 = ((uint32_t)q->qid << 16) | 1u;          /* CQID=q->qid, PC=1 */
    return nvme_submit(&g_admin, &cmd, NVME_ADMIN_CREATE_SQ);
}

/* ==================== namespace I/O ====================
 * All transfers go through g_bounce (one 4 KiB page), so PRP1 alone describes
 * them and a command never crosses a page. Requests larger than one page are
 * split into page-sized commands. `lba`/`count` are in 512-byte sectors, the
 * unit the SutureOS block layer uses; they are converted to the namespace's
 * own LBA size (g_ns_lba_bytes) here. */
static int nvme_rw(uint64_t lba, uint32_t count, void* buf, int is_write)
{
    const char* op = is_write ? "write" : "read";

    if (g_init_state != 1 || !buf)
        return -1;
    if (count == 0u)
        return 0;

    if (lba >= g_ns_sectors || (uint64_t)count > (g_ns_sectors - lba)) {
        console_printf("[nvme] %s out of range lba=%llu count=%u sectors=%llu\n",
                       op, (uint64_t)lba, (uint64_t)count, (uint64_t)g_ns_sectors);
        return -1;
    }

    const uint32_t spb = g_ns_spb;                      /* sectors per LBA */
    if ((lba % spb) != 0u || (count % spb) != 0u) {
        console_printf("[nvme] %s unaligned for %u-byte LBAs: lba=%llu count=%u\n",
                       op, (uint64_t)g_ns_lba_bytes, (uint64_t)lba, (uint64_t)count);
        return -1;
    }

    uint64_t dev_lba = lba / spb;
    uint64_t dev_left = (uint64_t)count / spb;
    uint32_t max_blk = NVME_BOUNCE_SIZE / g_ns_lba_bytes;   /* blocks per command */
    if (max_blk == 0u)
        max_blk = 1u;

    uint8_t* p = (uint8_t*)buf;

    while (dev_left > 0u) {
        uint32_t nblk = (dev_left > (uint64_t)max_blk) ? max_blk : (uint32_t)dev_left;
        uint32_t bytes = nblk * g_ns_lba_bytes;

        if (is_write) {
            memcpy(g_bounce, p, bytes);
            nvme_clflush(g_bounce, bytes);              /* device reads it next */
        }

        nvme_sqe_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.cdw0  = (uint32_t)(is_write ? NVME_OP_WRITE : NVME_OP_READ)
                  | ((uint32_t)(g_io.cid++) << 16);
        cmd.nsid  = 1u;
        cmd.prp1  = (uint64_t)(uintptr_t)g_bounce;
        cmd.cdw10 = (uint32_t)(dev_lba & 0xFFFFFFFFu);          /* SLBA low  */
        cmd.cdw11 = (uint32_t)(dev_lba >> 32);                  /* SLBA high */
        cmd.cdw12 = (nblk - 1u) & 0xFFFFu;                      /* NLB is 0-based */

        if (nvme_submit(&g_io, &cmd, is_write ? NVME_OP_WRITE : NVME_OP_READ) != 0) {
            console_printf("[nvme] %s failed at lba=%llu blocks=%u\n",
                           op, (uint64_t)dev_lba, (uint64_t)nblk);
            return -1;
        }

        if (!is_write) {
            nvme_clflush(g_bounce, bytes);              /* drop stale lines */
            memcpy(p, g_bounce, bytes);
        }

        dev_lba += nblk;
        dev_left -= nblk;
        p += (uintptr_t)bytes;
    }
    return 0;
}

static int nvme_read(struct blk_device* dev, uint64_t lba, uint32_t count, void* buf)
{
    (void)dev;
    return nvme_rw(lba, count, buf, 0);
}

static int nvme_write(struct blk_device* dev, uint64_t lba, uint32_t count, const void* buf)
{
    (void)dev;
    return nvme_rw(lba, count, (void*)buf, 1);
}

/* Flush (opcode 0x00): asks the controller to commit the namespace's volatile
 * write cache. No data transfer. */
static int nvme_flush(struct blk_device* dev)
{
    (void)dev;

    if (g_init_state != 1)
        return -1;

    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = (uint32_t)NVME_OP_FLUSH | ((uint32_t)(g_io.cid++) << 16);
    cmd.nsid = 1u;
    return nvme_submit(&g_io, &cmd, NVME_OP_FLUSH);
}

static const struct blk_ops g_nvme_ops = {
    .read  = nvme_read,
    .write = nvme_write,
    .flush = nvme_flush,
    .ioctl = NULL,
};

/* ==================== controller bring-up ==================== */
/* Everything up to (and including) Identify Namespace NSID 1. On success fills
 * g_ns_count/g_ns_lba_bytes/g_ns_spb/g_ns_sectors/g_model and returns 0. */
static int nvme_hw_init(void)
{
    uint8_t bus = 0, dev = 0, fn = 0;
    uint64_t bar0, asq_phys, acq_phys;
    uint32_t cc, csts, spins, mpsmin;
    uint16_t aqd, iqd;
    int ok;

    if (nvme_pci_find(&bus, &dev, &fn) != 0) {
        console_puts("[nvme] no NVMe controller on PCI\n");
        return -1;
    }
    g_bus = bus;
    g_dev = dev;
    g_fn  = fn;

    /* Enable memory space decoding + bus mastering: without the latter the
     * controller can never DMA a queue or a data buffer. */
    uint32_t pci_cmd = nvme_pci_read32(bus, dev, fn, NVME_PCI_COMMAND);
    nvme_pci_write32(bus, dev, fn, NVME_PCI_COMMAND,
                     pci_cmd | NVME_PCI_CMD_MEM_SPACE | NVME_PCI_CMD_BUS_MASTER);

    bar0 = nvme_pci_bar0(bus, dev, fn);
    if (bar0 == 0u || bar0 == 0xFFFFFFFFull) {
        console_printf("[nvme] BAR0 unusable (0x%llx)\n", (uint64_t)bar0);
        return -1;
    }

    /* The kernel identity-maps only the low 1 GiB, so the MMIO window must be
     * mapped explicitly before the first register access or it page-faults.
     * 16 KiB covers the register file plus the admin doorbells. */
    if (mmu_map_mmio_identity(bar0, 0x4000ull) != 0) {
        console_printf("[nvme] mmu_map_mmio_identity(0x%llx) failed\n", (uint64_t)bar0);
        return -1;
    }
    g_mmio_phys = bar0;
    g_mmio = (volatile uint8_t*)(uintptr_t)bar0;

    /* CAP: MQES[15:0], TO[39:24], DSTRD[31:28] ... MPSMIN[51:48].
     * Note the field offsets: DSTRD is bits 31:28 of the low dword. */
    g_cap = nvme_rd64(NVME_REG_CAP);
    g_vs  = nvme_rd32(NVME_REG_VS);
    g_mqes = (uint32_t)(g_cap & 0xFFFFu);
    g_doorbell_stride = 4u << (uint32_t)((g_cap >> 28) & 0xFu);
    mpsmin = (uint32_t)((g_cap >> 48) & 0xFu);

    console_printf("[nvme] controller at %02x:%02x.%x mmio=0x%llx version=0x%x mqes=%u\n",
                   (uint64_t)bus, (uint64_t)dev, (uint64_t)fn,
                   (uint64_t)bar0, (uint64_t)g_vs, (uint64_t)g_mqes);

    /* The queues below are 4 KiB aligned; a controller whose minimum page size
     * is larger than 4 KiB could not use them, so refuse rather than guess. */
    if (mpsmin != 0u) {
        console_printf("[nvme] unsupported CAP.MPSMIN=%u (only 4 KiB pages)\n",
                       (uint64_t)mpsmin);
        return -1;
    }

    /* Reset: CC.EN = 0, then wait for CSTS.RDY = 0 (spec 3.1.4/3.1.5). */
    cc = nvme_rd32(NVME_REG_CC);
    if ((cc & NVME_CC_EN) != 0u) {
        nvme_wr32(NVME_REG_CC, cc & ~NVME_CC_EN);
        ok = 0;
        for (spins = 0; spins < NVME_RESET_SPINS; spins++) {
            csts = nvme_rd32(NVME_REG_CSTS);
            if ((csts & NVME_CSTS_CFS) != 0u)
                break;
            if ((csts & NVME_CSTS_RDY) == 0u) {
                ok = 1;
                break;
            }
            nvme_pause();
        }
        if (!ok) {
            console_printf("[nvme] controller did not disable: cc=0x%x csts=0x%x\n",
                           (uint64_t)nvme_rd32(NVME_REG_CC),
                           (uint64_t)nvme_rd32(NVME_REG_CSTS));
            return -1;
        }
    }

    /* Polled driver: mask every interrupt source (INTMS bit set = masked). */
    nvme_wr32(NVME_REG_INTMS, 0xFFFFFFFFu);

    /* ONE admin queue pair and ONE I/O queue pair, in static 4 KiB-aligned
     * memory. Sizes are capped by CAP.MQES (0-based). */
    aqd = NVME_ADMIN_QD;
    if ((uint32_t)(g_mqes + 1u) < (uint32_t)aqd)
        aqd = (uint16_t)(g_mqes + 1u);
    if (aqd < 2u)
        aqd = 2u;

    iqd = NVME_IO_QD;
    if ((uint32_t)(g_mqes + 1u) < (uint32_t)iqd)
        iqd = (uint16_t)(g_mqes + 1u);
    if (iqd < 2u)
        iqd = 2u;

    nvme_queue_init(&g_admin, 0u, aqd, g_admin_sq, g_admin_cq);
    nvme_queue_init(&g_io, 1u, iqd, g_io_sq, g_io_cq);

    memset(g_admin_sq, 0, (size_t)aqd * 64u);
    memset(g_admin_cq, 0, (size_t)aqd * 16u);
    memset(g_io_sq, 0, (size_t)iqd * 64u);
    memset(g_io_cq, 0, (size_t)iqd * 16u);
    memset(g_identify, 0, NVME_PAGE_SIZE);
    memset(g_bounce, 0, NVME_BOUNCE_SIZE);

    asq_phys = (uint64_t)(uintptr_t)g_admin_sq;
    acq_phys = (uint64_t)(uintptr_t)g_admin_cq;

    /* The controller reads these as bus addresses; virtual == physical only
     * below the identity map and the 4 KiB alignment is a spec requirement. */
    if ((asq_phys & 0xFFFu) != 0u || (acq_phys & 0xFFFu) != 0u ||
        (uint64_t)(uintptr_t)g_io_sq & 0xFFFu) {
        console_printf("[nvme] queue memory not usable as DMA (asq=0x%llx acq=0x%llx)\n",
                       (uint64_t)asq_phys, (uint64_t)acq_phys);
        return -1;
    }

    /* AQA sizes are 0-based; ASQ/ACQ must be programmed while CC.EN = 0. */
    nvme_wr32(NVME_REG_AQA, ((uint32_t)(aqd - 1u) << 16) | (uint32_t)(aqd - 1u));
    nvme_wr64(NVME_REG_ASQ, asq_phys);
    nvme_wr64(NVME_REG_ACQ, acq_phys);

    /* Enable: EN=1, CSS=0 (NVM command set), MPS=0 (4 KiB), IOSQES=6 (64-byte
     * SQ entries), IOCQES=4 (16-byte CQ entries), AMS=0, SHN=0. */
    cc = NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_IOSQES_64B | NVME_CC_IOCQES_16B;
    nvme_wr32(NVME_REG_CC, cc);
    if (nvme_rd32(NVME_REG_CC) != cc) {
        console_printf("[nvme] CC write did not stick (cc=0x%x)\n",
                       (uint64_t)nvme_rd32(NVME_REG_CC));
        return -1;
    }

    /* Wait for CSTS.RDY = 1 (controller processed CC.EN = 1). */
    ok = 0;
    for (spins = 0; spins < NVME_RESET_SPINS; spins++) {
        csts = nvme_rd32(NVME_REG_CSTS);
        if ((csts & NVME_CSTS_CFS) != 0u)
            break;
        if ((csts & NVME_CSTS_RDY) != 0u) {
            ok = 1;
            break;
        }
        nvme_pause();
    }
    if (!ok) {
        console_printf("[nvme] controller did not become ready: csts=0x%x\n",
                       (uint64_t)nvme_rd32(NVME_REG_CSTS));
        return -1;
    }

    /* Identify Controller (CNS = 1): model number for the log line. */
    if (nvme_admin_identify(NVME_CNS_CONTROLLER, 0u, g_identify) != 0) {
        console_puts("[nvme] identify controller failed\n");
        return -1;
    }
    nvme_copy_ascii(g_model, sizeof(g_model), g_identify + NVME_ID_CTRL_MN, 40u);

    /* Create the single I/O queue pair (CQ before SQ, as the spec recommends). */
    if (nvme_create_io_cq(&g_io) != 0) {
        console_puts("[nvme] create I/O CQ failed\n");
        return -1;
    }
    if (nvme_create_io_sq(&g_io) != 0) {
        console_puts("[nvme] create I/O SQ failed\n");
        return -1;
    }

    console_printf("[nvme] admin queues up (asq=0x%llx acq=0x%llx), io queues up\n",
                   (uint64_t)asq_phys, (uint64_t)acq_phys);

    /* Identify Namespace NSID 1 (CNS = 0): NSZE + LBA format. CNS = 2 would be
     * the Active Namespace ID list, not the namespace data. */
    memset(g_identify, 0, NVME_PAGE_SIZE);
    if (nvme_admin_identify(NVME_CNS_NAMESPACE, 1u, g_identify) != 0) {
        console_puts("[nvme] identify namespace 1 failed\n");
        return -1;
    }

    uint64_t nsze = nvme_le64(g_identify + NVME_ID_NS_NSZE);
    uint32_t flbas = (uint32_t)(g_identify[NVME_ID_NS_FLBAS] & 0x0Fu);
    uint32_t lbaf = nvme_le32(g_identify + NVME_ID_NS_LBAF + flbas * 4u);
    uint32_t lbads = (lbaf >> 16) & 0xFFu;               /* log2(LBA bytes) */
    uint32_t lba_bytes = (lbads > 0u && lbads < 32u) ? (1u << lbads) : 0u;

    if (nsze == 0u || lba_bytes == 0u) {
        console_printf("[nvme] namespace 1 not usable (nsze=%llu lba_bytes=%u)\n",
                       (uint64_t)nsze, (uint64_t)lba_bytes);
        return -1;
    }
    if ((lba_bytes % 512u) != 0u || lba_bytes > NVME_BOUNCE_SIZE) {
        console_printf("[nvme] namespace 1 LBA size %u unsupported "
                       "(need a multiple of 512, <= %u)\n",
                       (uint64_t)lba_bytes, (uint64_t)NVME_BOUNCE_SIZE);
        return -1;
    }

    g_ns_count     = 1;
    g_ns_lba_bytes = lba_bytes;
    g_ns_spb       = lba_bytes / 512u;
    g_ns_sectors   = nsze * (uint64_t)g_ns_spb;          /* namespace size in 512B units */

    console_printf("[nvme] namespace 1: %s sectors=%llu (lba=%u)\n",
                   g_model[0] ? g_model : "(none)",
                   (uint64_t)g_ns_sectors, (uint64_t)g_ns_lba_bytes);
    return 0;
}

/* Run bring-up once; cache the outcome so init/probe are idempotent. */
static int nvme_bringup(void)
{
    if (g_init_state == 1)
        return 0;
    if (g_init_state != 0)
        return -1;

    if (nvme_hw_init() != 0) {
        g_init_state = -1;
        return -1;
    }
    g_init_state = 1;
    return 0;
}

/* ==================== public API ==================== */
int blk_nvme_init(void)
{
    if (g_registered)
        return 0;                                        /* idempotent */

    if (nvme_bringup() != 0)
        return -1;

    g_nvme_dev.name         = "nvme0";
    g_nvme_dev.type         = BLK_TYPE_NVME;
    g_nvme_dev.drive        = NVME_DRIVE_NVME0;
    g_nvme_dev.flags        = 0;
    g_nvme_dev.sector_count = g_ns_sectors;
    g_nvme_dev.sector_size  = 512u;
    g_nvme_dev.block_size   = 512u;
    g_nvme_dev.ops          = &g_nvme_ops;
    g_nvme_dev.driver_data  = NULL;
    g_nvme_dev.next         = NULL;

    blk_register(&g_nvme_dev);
    g_registered = 1;

    console_printf("[nvme] registered nvme0 (drive=%u) sectors=%llu\n",
                   (uint64_t)NVME_DRIVE_NVME0, (uint64_t)g_ns_sectors);
    return 0;
}

int blk_nvme_probe(void)
{
    (void)nvme_bringup();                                /* detection only: no blk_register */
    return g_ns_count;
}

int blk_nvme_present(void)
{
    return (g_init_state == 1) ? 1 : 0;
}

int blk_nvme_namespace_count(void)
{
    return g_ns_count;
}

uint64_t blk_nvme_mmio_base(void)
{
    return g_mmio_phys;
}

uint32_t blk_nvme_version(void)
{
    return g_vs;
}

uint32_t blk_nvme_max_queue_entries(void)
{
    return g_mqes;
}
