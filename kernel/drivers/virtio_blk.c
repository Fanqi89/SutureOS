/* SutureOS module: VirtIO-blk driver (legacy / virtio 0.9.5 transitional PCI)
 *
 * Ported from (READ-ONLY reference sources, none of them modified):
 *   - C:\Users\fanqi\Desktop\自研操作系统\Cinux\kernel\drivers\virtio\virtio.cpp
 *       (GPL-3.0): status handshake, feature negotiation, 64-bit device-config
 *       reads, queue register programming shape.
 *   - C:\Users\fanqi\Desktop\自研操作系统\Cinux\kernel\drivers\virtio\virtio_blk.cpp
 *       (GPL-3.0): 3-descriptor request chain (header / data / status),
 *       VIRTIO_BLK_T_{IN,OUT,FLUSH}, completion status-byte check.
 *   - C:\Users\fanqi\Desktop\自研操作系统\Cinux\kernel\mini\driver\pci.hpp
 *       (MIT): PCI config-space mechanism #1 dword access.
 *   - C:\Users\fanqi\Desktop\VimtuOS\Vimtu64\kernel\virtio_gpu64.cpp
 *       (GPL-3.0): split-virtqueue desc/avail/used layout, bounded used-ring
 *       polling, and "only accept a feature you truly implement".
 * SutureOS itself is GPL-3.0; the GPL-3.0/MIT terms of the sources carry over.
 *
 * What changed relative to the sources:
 *   - the legacy (0.9.5) I/O-port transport is implemented, not the modern
 *     capability-list/MMIO one: BAR0 is an I/O BAR and there is no MMIO window
 *     to map.  <stitch/io.h> has no outl/inl, so the dword helpers live here.
 *   - no kmalloc: the virtqueue and the request buffers are static .bss objects,
 *     so the driver is independent of the kernel heap (which the boot path
 *     re-initialises) and of the PMM.  The kernel is loaded at 1 MiB and
 *     identity-mapped, so virtual == physical for these buffers and the device
 *     can DMA them directly (the 32-bit PFN register needs < 4 GiB anyway).
 *   - transfers run through one page-sized static bounce buffer and are chunked
 *     to <= 8 sectors per request, so a data descriptor never spans a page and
 *     the caller's buffer is never exposed to the device.
 *   - the driver is pure polling (INTx is disabled in the PCI command register)
 *     because SutureOS has no handler for the shared PCI IRQ line.
 */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/io.h>

#include "virtio_blk.h"
#include "block.h"
/* heap.h is listed with the module includes for interface completeness, but this
 * driver deliberately never allocates from the kernel heap (static .bss only). */
#include "heap.h"
/* mmu.h would be needed if a BAR1 MMIO window were used
 * (mmu_map_mmio_identity()); the legacy path below touches only BAR0 I/O ports,
 * so no MMIO mapping is required and no mapping call is made. */
#include "mmu.h"

/* ===================== PCI config space =====================
 * Mechanism #1 needs DWORD accesses: writing 0xCF8 byte-by-byte does not stick
 * on QEMU (every slot then reads back the same constant).  Same idiom as
 * kernel/drivers/rtl8139.c. */
#define VIO_PCI_CFG_ADDR   0xCF8u
#define VIO_PCI_CFG_DATA   0xCFCu
#define VIO_PCI_CFG_VENDOR 0x00u
#define VIO_PCI_CFG_CMD    0x04u   /* command/status (RW low half)            */
#define VIO_PCI_CFG_HDRTYPE 0x0Cu  /* bit 23 = multi-function device          */
#define VIO_PCI_CFG_BAR0   0x10u

#define VIO_PCI_CMD_IO_SPACE      (1u << 0)     /* decode the I/O BAR0 window  */
#define VIO_PCI_CMD_BUS_MASTER    (1u << 2)     /* the device DMAs the vring   */
#define VIO_PCI_CMD_INTX_DISABLE  (1u << 10)    /* pure polling, never IRQ     */

/* ===================== legacy virtio register offsets (BAR0) ===================== */
#define VIO_REG_HOST_FEATURES  0x00u   /* r32                                */
#define VIO_REG_GUEST_FEATURES 0x04u   /* w32                                */
#define VIO_REG_QUEUE_ADDR     0x08u   /* w32: PFN = vq_phys >> 12           */
#define VIO_REG_QUEUE_SIZE     0x0Cu   /* r16, read-only in legacy           */
#define VIO_REG_QUEUE_SELECT   0x0Eu   /* w16                                */
#define VIO_REG_QUEUE_NOTIFY   0x10u   /* w16: queue index                   */
#define VIO_REG_STATUS         0x12u   /* r/w8                               */
#define VIO_REG_ISR            0x13u   /* r8, read-to-clear                  */
#define VIO_REG_CONFIG         0x14u   /* device config: le64 capacity @ +0  */

/* Status bits (virtio 0.9.5 / 1.0 share these) */
#define VIO_STATUS_ACK          0x01u
#define VIO_STATUS_DRIVER       0x02u
#define VIO_STATUS_DRIVER_OK    0x04u
#define VIO_STATUS_FEATURES_OK  0x08u
#define VIO_STATUS_FAILED       0x80u

/* The one optional device feature we implement: the FLUSH request type.  QEMU
 * answers VIRTIO_BLK_T_FLUSH with status UNSUPP unless this bit was negotiated
 * (hw/block/virtio-blk.c), so accepting it is what makes ->flush real rather
 * than a lie.  Every other blk feature (RO, BLK_SIZE, TOPOLOGY, SEG_MAX, ...)
 * is left unnegotiated because this driver does not use it. */
#define VIO_BLK_F_FLUSH        0x200u

/* Request types */
#define VIO_T_IN    0u   /* read:  device -> guest                       */
#define VIO_T_OUT   1u   /* write: guest  -> device                      */
#define VIO_T_FLUSH 4u   /* flush the device write cache (sector ignored) */

/* Completion status byte */
#define VIO_S_OK     0u
#define VIO_S_IOERR  1u
#define VIO_S_UNSUPP 2u

/* Descriptor flags (split virtqueue) */
#define VIO_DESC_F_NEXT   0x1u
#define VIO_DESC_F_WRITE  0x2u   /* device writes this buffer (guest-read) */

/* Bounded polls.  QEMU completes a request inside the notify write itself, so
 * the loop body is normally never entered; the bound only exists so a dead
 * device stalls for about a second instead of wedging the kernel forever. */
#define VIO_POLL_SPINS   2000000u

/* Bounce buffer = 8 sectors = one page: a data descriptor never exceeds a page. */
#define VIO_BOUNCE_SECTORS 8u
#define VIO_BOUNCE_BYTES   (VIO_BOUNCE_SECTORS * VIRTIO_BLK_SECTOR_SIZE)

/* Virtqueue arena: desc(16*256) + avail(4+2*256) + page-aligned used(4+8*256)
 * = 10244 bytes for the largest supported queue; 16 KiB leaves headroom. */
#define VIO_VQ_BYTES      16384u

/* ===================== structures (byte-exact, packed) ===================== */
struct vio_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vio_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

/* virtio_blk_req header: type(4) + reserved(4) + sector(8) = 16 bytes. */
struct vio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

_Static_assert(sizeof(struct vio_desc) == 16, "virtqueue descriptor must be 16 bytes");
_Static_assert(sizeof(struct vio_used_elem) == 8, "used ring element must be 8 bytes");
_Static_assert(sizeof(struct vio_blk_req) == 16, "virtio_blk_req header must be 16 bytes");

/* ===================== DMA-visible buffers (.bss) =====================
 * The kernel is loaded at 1 MiB and identity-mapped, so a .bss address is a
 * valid bus address (virtual == physical) and no allocator is involved: a
 * later PMM/heap re-init cannot move the virtqueue out from under the device. */
static uint8_t g_vq[VIO_VQ_BYTES] __attribute__((aligned(4096)));
static uint8_t g_bounce[VIO_BOUNCE_BYTES] __attribute__((aligned(64)));
static struct vio_blk_req g_req __attribute__((aligned(64)));
static uint8_t g_status __attribute__((aligned(64)));

/* ===================== driver state ===================== */
static int      g_state;                 /* 0 = down, 1 = up                  */
static uint16_t g_io_base;               /* decoded BAR0 I/O base             */
static uint16_t g_qsize;                 /* queue size read from 0x0C         */
static uint32_t g_features;              /* host features (0x00)              */
static uint32_t g_guest_features;        /* guest features we accepted (0x04) */
static uint64_t g_capacity;              /* sectors, from device config       */
static int      g_flush_ok;              /* VIRTIO_BLK_F_FLUSH negotiated     */

/* Virtqueue pointers (inside g_vq) and ring cursors.  Requests are serialised
 * by g_lock, so exactly one chain is ever in flight and descriptors 0..2 are
 * always free when a new request starts. */
static struct vio_desc *g_desc;
static volatile uint16_t *g_avail;                    /* [0]=flags [1]=idx */
static volatile uint16_t *g_used;                     /* [0]=flags [1]=idx */
static volatile uint16_t *g_used_idx;                 /* &g_used[1]        */
static volatile struct vio_used_elem *g_used_ring;    /* &g_used[2]        */
static uint16_t g_avail_idx;             /* next avail slot to publish        */
static uint16_t g_used_seen;             /* last used.idx we consumed         */

static volatile uint8_t g_lock;          /* SMP: one request at a time        */
static uint64_t g_requests;              /* completed with status OK          */
static uint64_t g_errors;                /* failed / unsupported requests     */

/* ===================== helpers ===================== */
static inline void vio_lock(void)
{
    while (__atomic_test_and_set(&g_lock, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}

static inline void vio_unlock(void)
{
    __atomic_clear(&g_lock, __ATOMIC_RELEASE);
}

/* Dword port I/O for the legacy window.  These take an ABSOLUTE port number
 * (call sites pass `io + VIO_REG_*`), exactly like io.h's byte/word helpers.
 *
 * SutureOS fix: the first version of this file added g_io_base here as well,
 * so every dword access landed on (g_io_base + io + off) = 0x8200+off instead
 * of 0xc100+off.  The device then reported host features 0xffffffff, capacity
 * 0xffffffffffffffff and never saw the queue PFN, which showed up as a
 * "request timed out" on the first FAT32 read.  Byte and word registers were
 * unaffected because those call sites always used outb/inb/outw/inw directly,
 * which is why the status handshake looked perfectly healthy (status 0x0f)
 * while the queue never worked. */
static inline void vio_outl(uint16_t port, uint32_t v)
{
    outl(port, v);
}

static inline uint32_t vio_inl(uint16_t port)
{
    return inl(port);
}

/* The device DMAs into/out of ordinary cached .bss memory.  QEMU's device model
 * is cache-coherent with the CPU, but on real silicon the ring and the buffers
 * must be written back before the device reads them and invalidated after it
 * writes them.  Same helper as rtl8139.c (a no-op for correctness under QEMU,
 * mandatory on hardware). */
static void vio_clflush(const void *addr, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)((uintptr_t)addr & ~(uintptr_t)63);
    const uint8_t *end = (const uint8_t *)addr + len;

    while (p < end) {
        __asm__ volatile("clflush (%0)" :: "r"(p) : "memory");
        p += 64;
    }
}

/* ===================== PCI config space (mechanism #1) ===================== */
static uint32_t vio_pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8)
                  | (uint32_t)(off & 0xFCu);

    __asm__ volatile("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)VIO_PCI_CFG_ADDR));
    {
        uint32_t v;
        __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)VIO_PCI_CFG_DATA));
        return v;
    }
}

static void vio_pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8)
                  | (uint32_t)(off & 0xFCu);

    __asm__ volatile("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)VIO_PCI_CFG_ADDR));
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"((uint16_t)VIO_PCI_CFG_DATA));
}

/* Scan bus 0..255 / dev 0..31 / fn 0..7 for 1AF4:1001 (legacy virtio-blk).
 * A 1AF4:1042 modern-transitional device is accepted only as a fallback: this
 * driver then still drives it through the legacy BAR0 interface, which the
 * default (transitional, non-disable-legacy) QEMU device keeps available.
 * Returns 0 and fills the out parameters on success. */
static int vio_pci_find(uint8_t *bus_out, uint8_t *dev_out, uint8_t *fn_out, uint16_t *did_out)
{
    uint8_t  cand_bus = 0, cand_dev = 0, cand_fn = 0;
    int      have_candidate = 0;
    uint32_t bus, dev, fn;

    for (bus = 0; bus < 256u; bus++) {
        for (dev = 0; dev < 32u; dev++) {
            uint32_t id = vio_pci_read32((uint8_t)bus, (uint8_t)dev, 0u, VIO_PCI_CFG_VENDOR);
            uint32_t hdr, fn_max;

            if ((id & 0xFFFFu) == 0xFFFFu)          /* empty slot               */
                continue;

            if ((id & 0xFFFFu) == VIRTIO_BLK_PCI_VENDOR) {
                uint32_t did = (id >> 16) & 0xFFFFu;
                if (did == VIRTIO_BLK_PCI_DEV_LEGACY) {
                    *bus_out = (uint8_t)bus; *dev_out = (uint8_t)dev; *fn_out = 0u;
                    *did_out = (uint16_t)did;
                    return 0;
                }
                if (did == VIRTIO_BLK_PCI_DEV_MODERN && !have_candidate) {
                    cand_bus = (uint8_t)bus; cand_dev = (uint8_t)dev; cand_fn = 0u;
                    have_candidate = 1;
                }
            }

            /* Multi-function devices hide function 0's siblings behind bit 23
             * of the header type; only walk the extra functions when set. */
            hdr = vio_pci_read32((uint8_t)bus, (uint8_t)dev, 0u, VIO_PCI_CFG_HDRTYPE);
            fn_max = (hdr & 0x00800000u) ? 8u : 1u;
            for (fn = 1u; fn < fn_max; fn++) {
                uint32_t did;
                id = vio_pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, VIO_PCI_CFG_VENDOR);
                if ((id & 0xFFFFu) != VIRTIO_BLK_PCI_VENDOR)
                    continue;
                did = (id >> 16) & 0xFFFFu;
                if (did == VIRTIO_BLK_PCI_DEV_LEGACY) {
                    *bus_out = (uint8_t)bus; *dev_out = (uint8_t)dev;
                    *fn_out = (uint8_t)fn; *did_out = (uint16_t)did;
                    return 0;
                }
                if (did == VIRTIO_BLK_PCI_DEV_MODERN && !have_candidate) {
                    cand_bus = (uint8_t)bus; cand_dev = (uint8_t)dev;
                    cand_fn = (uint8_t)fn; have_candidate = 1;
                }
            }
        }
    }

    if (have_candidate) {
        *bus_out = cand_bus; *dev_out = cand_dev; *fn_out = cand_fn;
        *did_out = VIRTIO_BLK_PCI_DEV_MODERN;
        return 0;
    }
    return -1;
}

/* ===================== virtqueue layout ===================== */
/* Lay out desc / avail / used inside g_vq for the current g_qsize.  The used
 * ring goes to the next 4096-byte boundary after the avail ring, which is the
 * legacy rule QEMU applies when it computes vring.used from the PFN. */
static void vio_queue_layout(void)
{
    uint32_t avail_off = 16u * (uint32_t)g_qsize;
    uint32_t used_off  = (avail_off + 4u + 2u * (uint32_t)g_qsize + 4095u) & ~4095u;

    g_desc      = (struct vio_desc *)(void *)g_vq;
    g_avail     = (volatile uint16_t *)(void *)(g_vq + avail_off);
    g_used      = (volatile uint16_t *)(void *)(g_vq + used_off);
    g_used_idx  = g_used + 1;
    g_used_ring = (volatile struct vio_used_elem *)(void *)(g_vq + used_off + 4u);

    g_avail_idx = 0;
    g_used_seen = 0;
}

/* ===================== request engine =====================
 * Build the 3-descriptor chain, publish it on the avail ring, notify the device
 * and poll the used ring with a bounded spin.  Requests are serialised by the
 * caller's vio_lock() and the caller pre-fills/consumes g_bounce.
 * device_writes_data = 1 for reads (VIRTIO_BLK_T_IN).
 * Returns 0 on status OK, -1 on transport failure (timeout / driver down),
 * -2 when the device reported a non-zero status byte. */
static int vio_do_request(uint32_t type, uint64_t sector, uint32_t bytes, int device_writes_data)
{
    uint32_t spins;
    uint16_t now;
    uint8_t  st;

    if (!g_state || g_qsize == 0u)
        return -1;
    if (bytes > VIO_BOUNCE_BYTES)
        return -1;                          /* cannot happen: callers chunk */

    /* Header + poison the status byte so a no-op completion is detectable. */
    g_req.type     = type;
    g_req.reserved = 0;
    g_req.sector   = sector;
    g_status       = 0xFFu;

    /* desc0: header (device reads), desc1: data (optional), desc2: status. */
    g_desc[0].addr  = (uint64_t)(uintptr_t)&g_req;
    g_desc[0].len   = (uint32_t)sizeof(g_req);
    g_desc[0].flags = VIO_DESC_F_NEXT;
    g_desc[0].next  = 1u;

    if (bytes != 0u) {
        g_desc[1].addr  = (uint64_t)(uintptr_t)g_bounce;
        g_desc[1].len   = bytes;
        g_desc[1].flags = (uint16_t)((device_writes_data ? VIO_DESC_F_WRITE : 0u) | VIO_DESC_F_NEXT);
        g_desc[1].next  = 2u;
        g_desc[2].addr  = (uint64_t)(uintptr_t)&g_status;
        g_desc[2].len   = 1u;
        g_desc[2].flags = VIO_DESC_F_WRITE;
        g_desc[2].next  = 0u;
    } else {
        /* FLUSH: header -> status only. */
        g_desc[1].addr  = (uint64_t)(uintptr_t)&g_status;
        g_desc[1].len   = 1u;
        g_desc[1].flags = VIO_DESC_F_WRITE;
        g_desc[1].next  = 0u;
    }

    /* Cache maintenance before the DMA:
     *  - the header always has to reach memory for the device to read it;
     *  - a write transfer has just filled g_bounce and its data must be written
     *    back, while a read transfer only needs stale (possibly dirty) lines
     *    dropped so a late writeback cannot clobber the device's data. */
    vio_clflush(&g_req, (uint32_t)sizeof(g_req));
    if (bytes != 0u)
        vio_clflush(g_bounce, bytes);
    vio_clflush(&g_status, 1u);
    __asm__ volatile("mfence" ::: "memory");

    /* Publish: ring entry first, then the index, then kick the device. */
    g_avail[2u + (uint32_t)(g_avail_idx % g_qsize)] = 0u;   /* head = desc 0 */
    __asm__ volatile("mfence" ::: "memory");
    g_avail_idx = (uint16_t)(g_avail_idx + 1u);
    g_avail[1] = g_avail_idx;
    __asm__ volatile("mfence" ::: "memory");
    outw((uint16_t)(g_io_base + VIO_REG_QUEUE_NOTIFY), 0u);

    /* Poll the used ring.  Invalidate the used.idx line before each sample:
     * the index is device-written and a cached sample would otherwise never
     * change on real silicon (harmless under QEMU's coherent model). */
    spins = VIO_POLL_SPINS;
    for (;;) {
        vio_clflush((const void *)g_used_idx, 2u);
        now = *g_used_idx;
        if (now != g_used_seen)
            break;
        if (--spins == 0u) {
            console_printf("[virtio-blk] request timed out: type=%u sector=%llu "
                           "(device stopped answering, driver disabled)\n",
                           (uint64_t)type, sector);
            (void)inb((uint16_t)(g_io_base + VIO_REG_ISR));
            (void)outb((uint16_t)(g_io_base + VIO_REG_STATUS),
                       (uint8_t)(VIO_STATUS_ACK | VIO_STATUS_DRIVER | VIO_STATUS_FAILED));
            g_errors++;
            g_state = 0;                    /* never pretend the device works */
            return -1;
        }
        __asm__ volatile("pause");
    }

    /* The used element was written before the index; read it, then ack. */
    vio_clflush((const void *)&g_used_ring[g_used_seen % g_qsize],
                (uint32_t)sizeof(struct vio_used_elem));
    g_used_seen = now;
    (void)inb((uint16_t)(g_io_base + VIO_REG_ISR));     /* read-to-clear       */

    /* Invalidate the buffers the device just wrote. */
    vio_clflush(&g_status, 1u);
    if (bytes != 0u && device_writes_data)
        vio_clflush(g_bounce, bytes);

    st = g_status;
    if (st != VIO_S_OK) {
        console_printf("[virtio-blk] request failed: status=%u type=%u sector=%u\n",
                       (uint64_t)st, (uint64_t)type, (uint64_t)sector);
        g_errors++;
        return -2;
    }
    g_requests++;
    return 0;
}

/* ===================== block device callbacks ===================== */
static int vblk_bounds(struct blk_device *dev, uint64_t lba, uint32_t count, const char *what)
{
    if (dev->sector_count == 0u)
        return 0;                            /* capacity unknown: device decides */
    if (lba >= dev->sector_count || (uint64_t)count > dev->sector_count - lba) {
        console_printf("[virtio-blk] %s out of range lba=%llu count=%u sectors=%llu\n",
                       what, lba, (uint64_t)count, dev->sector_count);
        return -1;
    }
    return 0;
}

static int vblk_read(struct blk_device *dev, uint64_t lba, uint32_t count, void *buf)
{
    uint8_t *dst = (uint8_t *)buf;
    uint64_t done = 0;

    if (!dev || !buf)
        return -1;
    if (count == 0u)
        return 0;
    if (!g_state)
        return -1;
    if (vblk_bounds(dev, lba, count, "read") != 0)
        return -1;

    vio_lock();
    while (done < count) {
        uint64_t left = (uint64_t)count - done;
        uint32_t n = (left > (uint64_t)VIO_BOUNCE_SECTORS) ? VIO_BOUNCE_SECTORS : (uint32_t)left;
        uint32_t bytes = n * VIRTIO_BLK_SECTOR_SIZE;

        if (vio_do_request(VIO_T_IN, lba + done, bytes, 1) != 0) {
            vio_unlock();
            return -1;
        }
        memcpy(dst + (uintptr_t)done * VIRTIO_BLK_SECTOR_SIZE, g_bounce, bytes);
        done += n;
    }
    vio_unlock();
    return 0;
}

static int vblk_write(struct blk_device *dev, uint64_t lba, uint32_t count, const void *buf)
{
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t done = 0;

    if (!dev || !buf)
        return -1;
    if (count == 0u)
        return 0;
    if (!g_state)
        return -1;
    if (vblk_bounds(dev, lba, count, "write") != 0)
        return -1;

    vio_lock();
    while (done < count) {
        uint64_t left = (uint64_t)count - done;
        uint32_t n = (left > (uint64_t)VIO_BOUNCE_SECTORS) ? VIO_BOUNCE_SECTORS : (uint32_t)left;
        uint32_t bytes = n * VIRTIO_BLK_SECTOR_SIZE;

        memcpy(g_bounce, src + (uintptr_t)done * VIRTIO_BLK_SECTOR_SIZE, bytes);
        if (vio_do_request(VIO_T_OUT, lba + done, bytes, 0) != 0) {
            vio_unlock();
            return -1;
        }
        done += n;
    }
    vio_unlock();
    return 0;
}

/* Real FLUSH when the device offered VIRTIO_BLK_F_FLUSH (we negotiated it);
 * an honest error when it did not, because a flush that never happened must
 * not be reported as success. */
static int vblk_flush(struct blk_device *dev)
{
    int r;

    (void)dev;
    if (!g_state)
        return -1;
    if (!g_flush_ok) {
        console_puts("[virtio-blk] flush unsupported (device did not offer "
                     "VIRTIO_BLK_F_FLUSH)\n");
        g_errors++;
        return -1;
    }
    vio_lock();
    r = vio_do_request(VIO_T_FLUSH, 0u, 0u, 0);
    vio_unlock();
    return (r == 0) ? 0 : -1;
}

static const struct blk_ops g_vblk_ops = {
    .read  = vblk_read,
    .write = vblk_write,
    .flush = vblk_flush,
    .ioctl = NULL,
};

static struct blk_device g_vblk_dev = {
    .name         = "vda",
    .type         = BLK_TYPE_VIRTIO,
    .drive        = VIRTIO_BLK_DRIVE,
    .flags        = 0,
    .sector_count = 0,
    .sector_size  = VIRTIO_BLK_SECTOR_SIZE,
    .block_size   = VIRTIO_BLK_SECTOR_SIZE,
    .ops          = &g_vblk_ops,
    .driver_data  = NULL,
    .next         = NULL,
};

/* ===================== bring-up ===================== */
int blk_virtio_init(void)
{
    uint8_t  bus = 0, dev = 0, fn = 0;
    uint16_t did = 0;
    uint16_t qsz;
    uint16_t io;
    uint32_t bar0, cmd;
    uint32_t features;
    uint8_t  status;
    uint64_t cap;

    if (g_state)
        return 0;                            /* idempotent */

    if (vio_pci_find(&bus, &dev, &fn, &did) != 0) {
        console_puts("[virtio-blk] no virtio-blk device on PCI\n");
        return -1;
    }

    /* I/O space decode + bus master (the virtqueue is DMA'd) + INTx disabled:
     * this driver is pure polling and SutureOS has no handler for the shared
     * PCI IRQ line, so the device must never assert it. */
    cmd = vio_pci_read32(bus, dev, fn, VIO_PCI_CFG_CMD);
    vio_pci_write32(bus, dev, fn, VIO_PCI_CFG_CMD,
                    cmd | VIO_PCI_CMD_IO_SPACE | VIO_PCI_CMD_BUS_MASTER |
                    VIO_PCI_CMD_INTX_DISABLE);

    /* BAR0 must be the legacy I/O window.  It is 0x20 bytes wide (0x00..0x1B
     * plus the 64-bit capacity at 0x14..0x1B), hence the 0xFFE0 limit. */
    bar0 = vio_pci_read32(bus, dev, fn, VIO_PCI_CFG_BAR0);
    if ((bar0 & 0x1u) == 0u) {
        console_printf("[virtio-blk] BAR0=0x%x is not an I/O BAR, no legacy window\n",
                       (uint64_t)bar0);
        return -1;
    }
    if ((bar0 & ~0x3u) > 0xFFE0u) {
        console_printf("[virtio-blk] BAR0 I/O base 0x%x outside the 16-bit port space\n",
                       (uint64_t)(bar0 & ~0x3u));
        return -1;
    }
    io = (uint16_t)(bar0 & ~0x3u);
    g_io_base = io;

    if (did == VIRTIO_BLK_PCI_DEV_MODERN)
        console_printf("[virtio-blk] transitional device 0x%x at %02u:%02u.%u, "
                       "driving the legacy BAR0 interface\n",
                       (uint64_t)did, (uint64_t)bus, (uint64_t)dev, (uint64_t)fn);

    /* ---- legacy status handshake ---- */
    outb((uint16_t)(io + VIO_REG_STATUS), 0u);                                  /* reset     */
    outb((uint16_t)(io + VIO_REG_STATUS), VIO_STATUS_ACK);                      /* ACKNOWLEDGE */
    outb((uint16_t)(io + VIO_REG_STATUS),
         (uint8_t)(VIO_STATUS_ACK | VIO_STATUS_DRIVER));                        /* DRIVER    */

    features = vio_inl((uint16_t)(io + VIO_REG_HOST_FEATURES));
    g_features = features;

    /* Accept only the bits we implement: FLUSH (see vblk_flush).  Everything
     * else stays unnegotiated. */
    g_guest_features = features & VIO_BLK_F_FLUSH;
    g_flush_ok = (g_guest_features & VIO_BLK_F_FLUSH) ? 1 : 0;
    vio_outl((uint16_t)(io + VIO_REG_GUEST_FEATURES), g_guest_features);

    outb((uint16_t)(io + VIO_REG_STATUS),
         (uint8_t)(VIO_STATUS_ACK | VIO_STATUS_DRIVER | VIO_STATUS_FEATURES_OK));
    if ((inb((uint16_t)(io + VIO_REG_STATUS)) & VIO_STATUS_FEATURES_OK) == 0u) {
        /* virtio 0.9.5 devices are not required to implement FEATURES_OK; the
         * transitional QEMU device does (and reports 0x0f below). */
        console_puts("[virtio-blk] note: FEATURES_OK not acknowledged "
                     "(legacy device), continuing\n");
    }

    /* ---- queue 0 ---- */
    outw((uint16_t)(io + VIO_REG_QUEUE_SELECT), 0u);
    qsz = inw((uint16_t)(io + VIO_REG_QUEUE_SIZE));

    console_printf("[virtio-blk] controller at %02u:%02u.%u io=0x%04x "
                   "queue_size=%u features=0x%08x\n",
                   (uint64_t)bus, (uint64_t)dev, (uint64_t)fn, (uint64_t)io,
                   (uint64_t)qsz, (uint64_t)features);

    if (qsz < 2u || qsz > VIRTIO_BLK_QUEUE_MAX ||
        (qsz & (uint16_t)(qsz - 1u)) != 0u) {
        console_printf("[virtio-blk] unusable queue size %u (want 2..%u, power of two)\n",
                       (uint64_t)qsz, (uint64_t)VIRTIO_BLK_QUEUE_MAX);
        return -1;
    }

    /* Static, page-aligned, identity-mapped virtqueue: physical == virtual. */
    memset(g_vq, 0, sizeof(g_vq));
    g_qsize = qsz;
    vio_queue_layout();

    if (((uintptr_t)g_vq & 0xFFFu) != 0u || (uintptr_t)g_vq > 0xFFFFFFFFull) {
        console_printf("[virtio-blk] virtqueue at 0x%llx is not a usable DMA page frame\n",
                       (uint64_t)(uintptr_t)g_vq);
        return -1;
    }
    vio_outl((uint16_t)(io + VIO_REG_QUEUE_ADDR), (uint32_t)((uintptr_t)g_vq >> 12));

    /* ---- DRIVER_OK: the device may now consume the virtqueue ---- */
    outb((uint16_t)(io + VIO_REG_STATUS),
         (uint8_t)(VIO_STATUS_ACK | VIO_STATUS_DRIVER | VIO_STATUS_FEATURES_OK |
                   VIO_STATUS_DRIVER_OK));
    status = inb((uint16_t)(io + VIO_REG_STATUS));

    /* Device config: 64-bit little-endian capacity in 512-byte sectors. */
    cap = (uint64_t)vio_inl((uint16_t)(io + VIO_REG_CONFIG))
        | ((uint64_t)vio_inl((uint16_t)(io + VIO_REG_CONFIG + 4u)) << 32);
    if (cap == 0u)
        console_puts("[virtio-blk] device reports 0 sectors (capacity unknown)\n");
    if (cap > VIRTIO_BLK_MAX_SECTORS) {
        console_printf("[virtio-blk] capacity %llu capped to %llu sectors\n",
                       cap, (uint64_t)VIRTIO_BLK_MAX_SECTORS);
        cap = VIRTIO_BLK_MAX_SECTORS;
    }
    g_capacity = cap;

    console_printf("[virtio-blk] capacity %llu sectors (%llu MiB), device status 0x%02x\n",
                   cap, cap / 2048u, (uint64_t)status);

    /* ---- register with the block layer as "vda" ---- */
    g_vblk_dev.sector_count = cap;
    g_vblk_dev.sector_size  = VIRTIO_BLK_SECTOR_SIZE;
    g_vblk_dev.block_size   = VIRTIO_BLK_SECTOR_SIZE;
    g_vblk_dev.flags        = 0;
    g_vblk_dev.next         = NULL;
    if (blk_register(&g_vblk_dev) != 0) {
        console_puts("[virtio-blk] blk_register failed\n");
        return -1;
    }
    g_state = 1;
    console_printf("[virtio-blk] registered %s (drive=%d)\n",
                   g_vblk_dev.name, (uint64_t)VIRTIO_BLK_DRIVE);
    return 0;
}

/* Detection without registration: 1 = a virtio-blk PCI function is present. */
int blk_virtio_probe(void)
{
    uint8_t  bus = 0, dev = 0, fn = 0;
    uint16_t did = 0;

    return (vio_pci_find(&bus, &dev, &fn, &did) == 0) ? 1 : 0;
}

/* ===================== diagnostics ===================== */
int blk_virtio_present(void)              { return g_state ? 1 : 0; }
uint16_t blk_virtio_io_base(void)         { return g_io_base; }
uint16_t blk_virtio_queue_size(void)      { return g_qsize; }
uint32_t blk_virtio_features(void)        { return g_features; }
uint32_t blk_virtio_guest_features(void)  { return g_guest_features; }
uint64_t blk_virtio_capacity_sectors(void){ return g_capacity; }
uint64_t blk_virtio_requests(void)        { return g_requests; }
uint64_t blk_virtio_errors(void)          { return g_errors; }
