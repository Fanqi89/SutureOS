/* SutureOS module: VirtIO-GPU 2D display backend (modern virtio 1.0 PCI device)
 *
 * Origin and licensing
 * --------------------
 * SutureOS original, written against the public VirtIO 1.2 specification
 * (OASIS: section 2.7 split virtqueues, 4.1 virtio over PCI bus with the
 * modern capability list, 5.7 GPU device commands) and against observed QEMU
 * behaviour (QMP `info pci` -> 1af4:1050 "Display controller", BAR4 64-bit
 * prefetchable 16 KiB window).  No third-party source file was copied.
 * The split-virtqueue mechanics (desc/avail/used layout, bounded used-ring
 * polling, strict completion status checking) follow the in-tree legacy
 * reference kernel/drivers/virtio_blk.c.  SutureOS is GPL-3.0.
 *
 * Why "modern" and not the legacy (0.9.5) I/O-port transport of virtio_blk:
 * `-device virtio-gpu-pci` in QEMU is a **non-transitional virtio 1.0**
 * device: there is no legacy I/O BAR.  It exposes the modern PCI capability
 * list (common config / notify / ISR / device config) and requires
 * VIRTIO_F_VERSION_1, a split virtqueue described by descriptor / available /
 * used ring addresses, and a per-queue notify doorbell.  So this driver
 * drives the modern interface only; a legacy-only device
 * (`disable-modern=on`) is detected and rejected with an honest message
 * instead of being half-supported.
 *
 * Hard constraints from the porting brief
 * --------------------------------------
 *  - Boot presentation / terminal / GUI must not change.  Therefore nothing
 *    in this file runs at boot: kmain does not call it.  The first use is the
 *    shell command `vga2d on`, and on a plain `-vga std` boot not one
 *    `[vga2d]` line is printed.
 *  - The scanout buffer is a static .bss array (so it does not depend on the
 *    kernel heap or the PMM), capped at 1024x768x4 = 3 MiB.
 *  - fbcon / kernel/term / kernel/gui are never touched: this is an extra
 *    display output, not a replacement for the framebuffer console.
 *
 * Pipeline (control queue 0, one command in flight, polled):
 *   GET_DISPLAY_INFO(0x0100) -> RESOURCE_CREATE_2D(0x0101) ->
 *   RESOURCE_ATTACH_BACKING(0x0106) -> SET_SCANOUT(0x0103) ->
 *   TRANSFER_TO_HOST_2D(0x0105) -> RESOURCE_FLUSH(0x0104)
 * Every response type is checked strictly: OK_NODATA (0x1100) for everything
 * except GET_DISPLAY_INFO, which must answer OK_DISPLAY_INFO (0x1101).  An
 * unknown response type is a failure, never "tolerated": a tolerant check is
 * what makes a broken command look successful.
 */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/io.h>

#include "virtio_gpu.h"
#include "font.h"   /* font8x16: SutureOS's own 8x16 VGA font for the title line */
#include "mmu.h"    /* mmu_map_mmio_identity() for the modern BAR window */

/* System tick (100 Hz).  Declared exactly like kernel/shell/shell.c does; the
 * definition lives in kernel/interrupts/init.c. */
extern volatile uint64_t g_tick_count;

/* ===================== PCI config space (mechanism #1) =====================
 * DWORD accesses only: writing 0xCF8 byte-by-byte does not stick on QEMU.
 * Same idiom as virtio_blk.c / rtl8139.c. */
#define VGPU_PCI_CFG_ADDR   0xCF8u
#define VGPU_PCI_CFG_DATA   0xCFCu

#define VGPU_CFG_VENDOR     0x00u
#define VGPU_CFG_COMMAND    0x04u
#define VGPU_CFG_STATUS     0x06u
#define VGPU_CFG_CLASS      0x08u
#define VGPU_CFG_HEADER     0x0Cu
#define VGPU_CFG_BAR0       0x10u
#define VGPU_CFG_CAP_PTR    0x34u

#define VGPU_CMD_MEM_SPACE      (1u << 1)   /* decode the memory BAR window   */
#define VGPU_CMD_BUS_MASTER     (1u << 2)   /* the device DMAs the rings/data */
#define VGPU_CMD_INTX_DISABLE   (1u << 10)  /* pure polling, no IRQ handler   */
#define VGPU_STATUS_CAP_LIST    (1u << 20)  /* status word bit 4 (read as hi) */

/* virtio PCI IDs: 0x1040 + device type.  Device type 16 = GPU, hence 0x1050
 * for a modern (non-transitional) virtio-gpu; 0x1000 + 16 = 0x1010 is the
 * legacy-only id.  Both are recognised so the diagnostic can tell the user
 * which one it found. */
#define VGPU_PCI_VENDOR         0x1AF4u
#define VGPU_PCI_DEV_MODERN     0x1050u
#define VGPU_PCI_DEV_LEGACY     0x1010u
#define VGPU_PCI_CLASS_DISPLAY  0x03u

/* Modern virtio PCI capability (struct virtio_pci_cap), cfg_type values. */
#define VGPU_CAP_VNDR       0x09u
#define VGPU_CAP_COMMON     1u
#define VGPU_CAP_NOTIFY     2u
#define VGPU_CAP_ISR        3u
#define VGPU_CAP_DEVICE     4u

/* ===================== virtio status bits ===================== */
#define VGPU_ST_ACK          0x01u
#define VGPU_ST_DRIVER       0x02u
#define VGPU_ST_DRIVER_OK    0x04u
#define VGPU_ST_FEATURES_OK  0x08u
#define VGPU_ST_FAILED       0x80u

/* Feature bit 32 = VIRTIO_F_VERSION_1 (device_feature_select = 1, bit 0). */
#define VGPU_FEATURE_VERSION1_SEL  1u
#define VGPU_FEATURE_VERSION1_BIT  0u

/* ===================== virtio-gpu command / response types ===================== */
#define VGPU_CMD_GET_DISPLAY_INFO      0x0100u
#define VGPU_CMD_RESOURCE_CREATE_2D    0x0101u
#define VGPU_CMD_SET_SCANOUT           0x0103u
#define VGPU_CMD_RESOURCE_FLUSH        0x0104u
#define VGPU_CMD_TRANSFER_TO_HOST_2D   0x0105u
#define VGPU_CMD_RESOURCE_ATTACH_BACKING 0x0106u

#define VGPU_RESP_OK_NODATA            0x1100u
#define VGPU_RESP_OK_DISPLAY_INFO      0x1101u

/* VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM.  A 32bpp resource: the little-endian u32
 * a guest stores as 0x00RRGGBB lands in memory as B,G,R,A, which is exactly
 * this format.  (QEMU's 2D path accepts this format; it is the one every
 * virtio-gpu 2D guest uses.) */
#define VGPU_FORMAT_B8G8R8A8_UNORM     1u

#define VGPU_MAX_SCANOUTS              16u
#define VGPU_SCANOUT_ID                0u
#define VGPU_RESOURCE_ID               1u

/* ===================== virtqueue (split ring) ===================== */
#define VGPU_DESC_F_NEXT   0x1u
#define VGPU_DESC_F_WRITE  0x2u

#define VGPU_QUEUE_MAX     8u          /* control queue depth we ask for      */
#define VGPU_VQ_BYTES      12288u      /* desc + avail + page-aligned used    */
#define VGPU_CMD_BYTES     128u        /* largest command we build            */
#define VGPU_RESP_BYTES    512u        /* GET_DISPLAY_INFO answers 408 bytes  */

/* Bounded polls: QEMU completes a virtio-gpu command inside the notify write,
 * so the loop body is normally never entered; the bound only stops a dead
 * device from wedging the shell forever. */
#define VGPU_POLL_SPINS    2000000u

/* Frame pacing: one TRANSFER+FLUSH every 5 system ticks (100 Hz) = 50 ms =
 * 20 fps.  Each frame only transfers the strip the moving square occupies, so
 * the DMA cost is at most 2 x 64 x 64 x 4 bytes, not the whole 3 MiB frame. */
#define VGPU_FRAME_TICKS   5u

/* Animation geometry: a 64x64 white square sliding horizontally over eight
 * solid colour bands.  Both are exact-value friendly so the host-side
 * acceptance can assert pixels instead of eyeballing a screenshot. */
#define VGPU_SQ            64u
#define VGPU_SQ_COLOR      0xFFFFFFu

/* ===================== structures (byte-exact, packed) =====================
 * Every size below is pinned by a _Static_assert.  A struct that is 8 bytes
 * too long is invisible at compile time and shows up as the device reading
 * garbage at a shifted offset - exactly the failure mode the porting notes
 * warn about for RESOURCE_ATTACH_BACKING. */
struct vgpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct vgpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct vgpu_display_one {
    struct vgpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

struct vgpu_resp_display_info {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_display_one pmodes[VGPU_MAX_SCANOUTS];
} __attribute__((packed));

struct vgpu_cmd_create_2d {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct vgpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct vgpu_cmd_attach_backing {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    struct vgpu_mem_entry entries[1];
} __attribute__((packed));

struct vgpu_cmd_set_scanout {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct vgpu_cmd_transfer_to_host_2d {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct vgpu_cmd_resource_flush {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

_Static_assert(sizeof(struct vgpu_ctrl_hdr) == 24, "virtio_gpu_ctrl_hdr is 24 bytes");
_Static_assert(sizeof(struct vgpu_rect) == 16, "virtio_gpu_rect is 16 bytes");
_Static_assert(sizeof(struct vgpu_display_one) == 24, "virtio_gpu_display_one is 24 bytes");
_Static_assert(sizeof(struct vgpu_resp_display_info) == 408,
               "resp_display_info is 24 + 16*24 = 408 bytes");
_Static_assert(sizeof(struct vgpu_cmd_create_2d) == 40, "resource_create_2d is 40 bytes");
_Static_assert(sizeof(struct vgpu_mem_entry) == 16, "virtio_gpu_mem_entry is 16 bytes");
_Static_assert(sizeof(struct vgpu_cmd_attach_backing) == 48,
               "resource_attach_backing with 1 entry is 24+4+4+16 = 48 bytes");
_Static_assert(sizeof(struct vgpu_cmd_set_scanout) == 48, "set_scanout is 48 bytes");
_Static_assert(sizeof(struct vgpu_cmd_transfer_to_host_2d) == 56,
               "transfer_to_host_2d is 56 bytes");
_Static_assert(sizeof(struct vgpu_cmd_resource_flush) == 48, "resource_flush is 48 bytes");

/* Split-virtqueue rings (virtio 1.2 section 2.7). */
struct vgpu_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vgpu_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

_Static_assert(sizeof(struct vgpu_desc) == 16, "virtqueue descriptor is 16 bytes");
_Static_assert(sizeof(struct vgpu_used_elem) == 8, "used ring element is 8 bytes");

/* Modern virtio-pci common configuration structure (virtio 1.2 section 4.1.4.3).
 * Only the fields we use are declared; the two 1.2 additions after queue_device
 * (queue_notif_config_data / queue_reset) are intentionally left out. */
struct vgpu_common_cfg {
    volatile uint32_t device_feature_select;   /* 0x00 */
    volatile uint32_t device_feature;          /* 0x04 */
    volatile uint32_t driver_feature_select;   /* 0x08 */
    volatile uint32_t driver_feature;          /* 0x0C */
    volatile uint16_t msix_config;             /* 0x10 */
    volatile uint16_t num_queues;              /* 0x12 */
    volatile uint8_t  device_status;           /* 0x14 */
    volatile uint8_t  config_generation;       /* 0x15 */
    volatile uint16_t queue_select;            /* 0x16 */
    volatile uint16_t queue_size;              /* 0x18 */
    volatile uint16_t queue_msix_vector;       /* 0x1A */
    volatile uint16_t queue_enable;            /* 0x1C */
    volatile uint16_t queue_notify_off;        /* 0x1E */
    volatile uint64_t queue_desc;              /* 0x20 */
    volatile uint64_t queue_driver;            /* 0x28 (available ring)      */
    volatile uint64_t queue_device;            /* 0x30 (used ring)           */
} __attribute__((packed));

_Static_assert(sizeof(struct vgpu_common_cfg) == 56,
               "virtio_pci_common_cfg through queue_device is 56 bytes");

/* Device configuration: le32 events_read, events_clear, num_scanouts,
 * num_capsets (the last one only exists from virtio 1.1, so it is not read). */
struct vgpu_device_cfg {
    volatile uint32_t events_read;
    volatile uint32_t events_clear;
    volatile uint32_t num_scanouts;
} __attribute__((packed));

/* One parsed modern PCI capability. */
struct vgpu_cap {
    uint8_t  cfg_type;
    uint8_t  bar;
    uint32_t offset;
    uint32_t length;
    int      found;
};

union vgpu_cmd_buf {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_cmd_create_2d create_2d;
    struct vgpu_cmd_attach_backing attach;
    struct vgpu_cmd_set_scanout set_scanout;
    struct vgpu_cmd_transfer_to_host_2d transfer;
    struct vgpu_cmd_resource_flush flush;
    uint8_t bytes[VGPU_CMD_BYTES];
};

/* ===================== DMA-visible buffers (.bss) =====================
 * The kernel is loaded at 1 MiB and identity-mapped, so these addresses are
 * valid bus addresses and no allocator is involved: a later PMM/heap
 * re-initialisation cannot move them out from under the device. */
static uint8_t  g_vq[VGPU_VQ_BYTES] __attribute__((aligned(4096)));
static union vgpu_cmd_buf  g_cmd  __attribute__((aligned(64)));
static uint8_t  g_resp[VGPU_RESP_BYTES] __attribute__((aligned(64)));

/* The scanout backing store: 1024x768x4 = 3 MiB, the brief's hard cap. */
static uint32_t g_fb[VIRTIO_GPU_MAX_WIDTH * VIRTIO_GPU_MAX_HEIGHT]
    __attribute__((aligned(4096)));

/* ===================== driver state ===================== */
static struct {
    int      found;          /* a virtio-gpu PCI function was seen         */
    int      up;             /* modern bring-up finished (queue usable)    */
    int      enabled;        /* `vga2d on` in effect -> service runs       */

    uint8_t  bus, dev, fn;
    uint16_t did;
    uint32_t n_caps;

    struct vgpu_cap common, notify, isr, device;
    uint64_t bar_base[6];
    uint32_t notify_mult;

    struct vgpu_common_cfg *cfg;
    uint8_t  *notify_area;
    uint8_t  *isr_area;
    struct vgpu_device_cfg *dev_cfg;

    uint16_t qsize;
    uint16_t q_notify_off;
    struct vgpu_desc *desc;
    volatile uint16_t *avail;                  /* [0]=flags [1]=idx [2..]=ring */
    volatile uint16_t *used;                   /* [0]=flags [1]=idx            */
    volatile uint16_t *used_idx;
    volatile struct vgpu_used_elem *used_ring;
    uint16_t avail_idx;
    uint16_t used_seen;

    uint32_t features_lo, features_hi;
    uint32_t num_scanouts;

    uint32_t disp_w, disp_h;                   /* reported by the device     */
    uint32_t disp_scanout;
    uint32_t res_id;
    uint32_t w, h;                             /* active scanout geometry    */
    uint32_t sq_x;                             /* current square x           */
    uint32_t frame;                            /* animation frames applied   */
    uint64_t flushes;                          /* RESOURCE_FLUSH issued      */
    uint64_t errors;
    uint64_t last_tick;
} g;

static volatile uint8_t g_lock;                /* SMP: one command at a time */

/* ===================== small helpers ===================== */
static inline void vgpu_lock(void)
{
    while (__atomic_test_and_set(&g_lock, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}

static inline void vgpu_unlock(void)
{
    __atomic_clear(&g_lock, __ATOMIC_RELEASE);
}

/* Cache maintenance around DMA.  QEMU's device model is coherent with the CPU
 * so this is a no-op in practice, but on real silicon the ring/buffers must be
 * written back before the device reads them and invalidated after it writes. */
static void vgpu_clflush(const void *addr, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)((uintptr_t)addr & ~(uintptr_t)63);
    const uint8_t *end = (const uint8_t *)addr + len;

    while (p < end) {
        __asm__ volatile("clflush (%0)" :: "r"(p) : "memory");
        p += 64;
    }
}

/* ===================== PCI config space ===================== */
static uint32_t vgpu_pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8)
                  | (uint32_t)(off & 0xFCu);
    uint32_t v;

    __asm__ volatile("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)VGPU_PCI_CFG_ADDR));
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)VGPU_PCI_CFG_DATA));
    return v;
}

static void vgpu_pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8)
                  | (uint32_t)(off & 0xFCu);

    __asm__ volatile("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)VGPU_PCI_CFG_ADDR));
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"((uint16_t)VGPU_PCI_CFG_DATA));
}

/* Byte/dword readers that do not assume the capability pointer is dword
 * aligned: read the containing dword and shift. */
static uint8_t vgpu_pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t d = vgpu_pci_read32(bus, dev, fn, (uint8_t)(off & 0xFCu));
    return (uint8_t)((d >> ((uint32_t)(off & 3u) * 8u)) & 0xFFu);
}

static uint32_t vgpu_pci_read_unaligned32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    return (uint32_t)vgpu_pci_read8(bus, dev, fn, (uint8_t)(off + 0u))
         | ((uint32_t)vgpu_pci_read8(bus, dev, fn, (uint8_t)(off + 1u)) << 8)
         | ((uint32_t)vgpu_pci_read8(bus, dev, fn, (uint8_t)(off + 2u)) << 16)
         | ((uint32_t)vgpu_pci_read8(bus, dev, fn, (uint8_t)(off + 3u)) << 24);
}

/* Base address of BAR `bar`.  Memory BARs only; returns 0 for an I/O BAR or an
 * unmapped BAR (callers treat 0 as "not usable"). */
static uint64_t vgpu_pci_bar_base(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t bar)
{
    uint8_t  off = (uint8_t)(VGPU_CFG_BAR0 + 4u * bar);
    uint32_t lo  = vgpu_pci_read32(bus, dev, fn, off);

    if ((lo & 0x1u) != 0u)                 /* I/O space BAR: not used here  */
        return 0;
    if ((lo & ~0xFu) == 0u)                /* not assigned                  */
        return 0;

    if (((lo >> 1) & 0x3u) == 0x2u) {      /* 64-bit memory BAR             */
        uint32_t hi = vgpu_pci_read32(bus, dev, fn, (uint8_t)(off + 4u));
        return (uint64_t)(lo & ~0xFu) | ((uint64_t)hi << 32);
    }
    return (uint64_t)(lo & ~0xFu);
}

/* Inspect one PCI function: does it look like a virtio-gpu display function?
 * Returns 1 for a modern (0x1050) match, 2 for a legacy-only (0x1010) match,
 * 0 otherwise.  The class code must be display (0x03) so a virtio function
 * with a different device type can never be mistaken for the GPU. */
static int vgpu_pci_match(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t *did_out)
{
    uint32_t id = vgpu_pci_read32(bus, dev, fn, VGPU_CFG_VENDOR);
    uint32_t did, cls;

    if ((id & 0xFFFFu) != VGPU_PCI_VENDOR)
        return 0;
    did = (id >> 16) & 0xFFFFu;
    if (did != VGPU_PCI_DEV_MODERN && did != VGPU_PCI_DEV_LEGACY)
        return 0;
    cls = (vgpu_pci_read32(bus, dev, fn, VGPU_CFG_CLASS) >> 24) & 0xFFu;
    if (cls != VGPU_PCI_CLASS_DISPLAY)
        return 0;
    *did_out = (uint16_t)did;
    return (did == VGPU_PCI_DEV_MODERN) ? 1 : 2;
}

/* Scan PCI for a virtio-gpu display function.  A modern device (0x1050) wins
 * over a legacy-only one (0x1010) so the diagnostic can say which exists. */
static int vgpu_pci_find(uint8_t *bus_out, uint8_t *dev_out, uint8_t *fn_out,
                         uint16_t *did_out)
{
    uint8_t  c_bus = 0, c_dev = 0, c_fn = 0;
    uint16_t c_did = 0;
    int      have_legacy = 0;
    uint32_t bus, dev, fn;

    for (bus = 0; bus < 256u; bus++) {
        for (dev = 0; dev < 32u; dev++) {
            uint32_t hdr = vgpu_pci_read32((uint8_t)bus, (uint8_t)dev, 0u,
                                           VGPU_CFG_HEADER);
            /* Extra functions only exist when header-type bit 23 is set. */
            uint32_t fn_max = (hdr & 0x00800000u) ? 8u : 1u;

            for (fn = 0; fn < fn_max; fn++) {
                uint16_t did = 0;
                int m = vgpu_pci_match((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, &did);

                if (m == 1) {
                    *bus_out = (uint8_t)bus; *dev_out = (uint8_t)dev;
                    *fn_out = (uint8_t)fn;   *did_out = did;
                    return 0;
                }
                if (m == 2 && !have_legacy) {
                    c_bus = (uint8_t)bus; c_dev = (uint8_t)dev;
                    c_fn = (uint8_t)fn;   c_did = did;
                    have_legacy = 1;
                }
            }
        }
    }

    if (have_legacy) {
        *bus_out = c_bus; *dev_out = c_dev; *fn_out = c_fn; *did_out = c_did;
        return 0;
    }
    return -1;
}

/* Walk the PCI capability list and record the four modern virtio structures.
 * Returns the number of virtio capabilities found. */
static uint32_t vgpu_scan_caps(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint32_t status = vgpu_pci_read32(bus, dev, fn, VGPU_CFG_STATUS);
    uint32_t off, guard = 0;
    uint32_t n = 0;

    g.common.found = g.notify.found = g.isr.found = g.device.found = 0;
    g.notify_mult = 0;

    if ((status & VGPU_STATUS_CAP_LIST) == 0u)
        return 0;

    off = vgpu_pci_read32(bus, dev, fn, VGPU_CFG_CAP_PTR) & 0xFCu;
    while (off >= 0x40u && off < 0x100u && guard++ < 48u) {
        uint8_t  id   = vgpu_pci_read8(bus, dev, fn, (uint8_t)off);
        uint8_t  next = vgpu_pci_read8(bus, dev, fn, (uint8_t)(off + 1u)) & 0xFCu;
        uint8_t  len  = vgpu_pci_read8(bus, dev, fn, (uint8_t)(off + 2u));

        if (id == VGPU_CAP_VNDR && len >= 16u) {
            uint8_t  ctype = vgpu_pci_read8(bus, dev, fn, (uint8_t)(off + 3u));
            uint8_t  bar   = vgpu_pci_read8(bus, dev, fn, (uint8_t)(off + 4u));
            uint32_t co    = vgpu_pci_read_unaligned32(bus, dev, fn, (uint8_t)(off + 8u));
            uint32_t cl    = vgpu_pci_read_unaligned32(bus, dev, fn, (uint8_t)(off + 12u));
            struct vgpu_cap *slot = NULL;

            if (ctype == VGPU_CAP_COMMON) slot = &g.common;
            else if (ctype == VGPU_CAP_NOTIFY) slot = &g.notify;
            else if (ctype == VGPU_CAP_ISR) slot = &g.isr;
            else if (ctype == VGPU_CAP_DEVICE) slot = &g.device;

            if (slot) {
                slot->cfg_type = ctype;
                slot->bar = bar;
                slot->offset = co;
                slot->length = cl;
                slot->found = 1;
                n++;
                if (ctype == VGPU_CAP_NOTIFY && len >= 20u)
                    g.notify_mult = vgpu_pci_read_unaligned32(bus, dev, fn,
                                                             (uint8_t)(off + 16u));
            }
        }
        if (next == 0u || next == off)
            break;
        off = next;
    }
    return n;
}

/* ===================== virtio-gpu command engine ===================== */
/* Submit one command on control queue 0 and wait for its response.  The caller
 * fills g_cmd and passes its length; the response header type must equal
 * `expect_type`.  Returns 0, or -1 with g.errors incremented. */
static int vgpu_submit(uint32_t cmdlen, uint32_t expect_type, uint32_t *resp_len_out,
                       const char *what)
{
    uint32_t spins;
    uint16_t now;
    uint32_t rtype;
    uint32_t used_len;

    if (!g.up || g.qsize == 0u)
        return -1;
    if (cmdlen == 0u || cmdlen > VGPU_CMD_BYTES)
        return -1;

    memset(g_resp, 0, VGPU_RESP_BYTES);

    g.desc[0].addr  = (uint64_t)(uintptr_t)g_cmd.bytes;
    g.desc[0].len   = cmdlen;
    g.desc[0].flags = VGPU_DESC_F_NEXT;
    g.desc[0].next  = 1u;
    g.desc[1].addr  = (uint64_t)(uintptr_t)g_resp;
    g.desc[1].len   = VGPU_RESP_BYTES;
    g.desc[1].flags = VGPU_DESC_F_WRITE;
    g.desc[1].next  = 0u;

    vgpu_clflush(g_cmd.bytes, cmdlen);
    vgpu_clflush(g_resp, VGPU_RESP_BYTES);
    __asm__ volatile("mfence" ::: "memory");

    /* Publish the chain, then advance the available index, then ring the
     * doorbell.  Each step is fenced so the device never sees a half-written
     * ring entry. */
    g.avail[2u + (uint32_t)(g.avail_idx % g.qsize)] = 0u;   /* head = desc 0 */
    __asm__ volatile("mfence" ::: "memory");
    g.avail_idx = (uint16_t)(g.avail_idx + 1u);
    g.avail[1] = g.avail_idx;
    __asm__ volatile("mfence" ::: "memory");

    *(volatile uint16_t *)(void *)(g.notify_area
        + (uint32_t)g.q_notify_off * g.notify_mult) = 0u;   /* queue 0 */

    spins = VGPU_POLL_SPINS;
    for (;;) {
        vgpu_clflush((const void *)g.used_idx, 2u);
        now = *g.used_idx;
        if (now != g.used_seen)
            break;
        if (--spins == 0u) {
            console_printf("[vga2d] cmd 0x%x (%s) timed out, device disabled\n",
                           (uint64_t)expect_type, what);
            g.errors++;
            g.up = 0;
            g.enabled = 0;
            return -1;
        }
        __asm__ volatile("pause");
    }

    vgpu_clflush((const void *)&g.used_ring[g.used_seen % g.qsize],
                 (uint32_t)sizeof(struct vgpu_used_elem));
    used_len = g.used_ring[g.used_seen % g.qsize].len;
    g.used_seen = now;
    (void)*(volatile uint8_t *)g.isr_area;       /* read-to-clear */

    vgpu_clflush(g_resp, VGPU_RESP_BYTES);
    if (used_len < (uint32_t)sizeof(struct vgpu_ctrl_hdr)) {
        console_printf("[vga2d] cmd 0x%x (%s): short response %u bytes\n",
                       (uint64_t)expect_type, what, (uint64_t)used_len);
        g.errors++;
        return -1;
    }

    rtype = ((volatile struct vgpu_ctrl_hdr *)g_resp)->type;
    if (rtype != expect_type) {
        /* Strict: an unknown/error response is a failure.  Treating 0x1200.. as
         * "tolerated" is what lets a rejected command look successful. */
        console_printf("[vga2d] cmd 0x%x (%s): resp BAD type=0x%x\n",
                       (uint64_t)expect_type, what, (uint64_t)rtype);
        g.errors++;
        return -1;
    }
    if (resp_len_out)
        *resp_len_out = used_len;
    return 0;
}

/* Individual commands; the header of g_cmd is zeroed by vgpu_cmd_begin(). */
static void vgpu_cmd_begin(uint32_t type)
{
    memset(g_cmd.bytes, 0, sizeof(g_cmd.bytes));
    g_cmd.hdr.type = type;
    g_cmd.hdr.flags = 0;
    g_cmd.hdr.fence_id = 0;
    g_cmd.hdr.ctx_id = 0;
    g_cmd.hdr.padding = 0;
}

static int vgpu_cmd_get_display_info(void)
{
    uint32_t rlen = 0;
    uint32_t i, w = 0, h = 0, scanout = 0;
    int      found = 0;

    vgpu_cmd_begin(VGPU_CMD_GET_DISPLAY_INFO);
    if (vgpu_submit((uint32_t)sizeof(struct vgpu_ctrl_hdr),
                    VGPU_RESP_OK_DISPLAY_INFO, &rlen, "get_display_info") != 0)
        return -1;

    if (rlen < (uint32_t)sizeof(struct vgpu_resp_display_info)) {
        console_printf("[vga2d] display info short: %u of %u bytes\n",
                       (uint64_t)rlen,
                       (uint64_t)sizeof(struct vgpu_resp_display_info));
        g.errors++;
        return -1;
    }

    for (i = 0; i < VGPU_MAX_SCANOUTS; i++) {
        struct vgpu_display_one *pm =
            &((struct vgpu_resp_display_info *)g_resp)->pmodes[i];
        if (pm->enabled) {
            w = pm->r.width;
            h = pm->r.height;
            scanout = i;
            found = 1;
            break;
        }
    }

    if (!found || w == 0u || h == 0u) {
        console_puts("[vga2d] display info: no enabled scanout, assuming 1024x768\n");
        w = VIRTIO_GPU_MAX_WIDTH;
        h = VIRTIO_GPU_MAX_HEIGHT;
        scanout = 0;
    }

    g.disp_w = w;
    g.disp_h = h;
    g.disp_scanout = scanout;
    console_printf("[vga2d] display %ux%u scanout=%u\n",
                   (uint64_t)w, (uint64_t)h, (uint64_t)scanout);
    return 0;
}

static int vgpu_cmd_resource_create_2d(uint32_t w, uint32_t h)
{
    vgpu_cmd_begin(VGPU_CMD_RESOURCE_CREATE_2D);
    g_cmd.create_2d.resource_id = VGPU_RESOURCE_ID;
    g_cmd.create_2d.format = VGPU_FORMAT_B8G8R8A8_UNORM;
    g_cmd.create_2d.width = w;
    g_cmd.create_2d.height = h;
    return vgpu_submit((uint32_t)sizeof(struct vgpu_cmd_create_2d),
                       VGPU_RESP_OK_NODATA, NULL, "resource_create_2d");
}

static int vgpu_cmd_attach_backing(void)
{
    vgpu_cmd_begin(VGPU_CMD_RESOURCE_ATTACH_BACKING);
    g_cmd.attach.resource_id = VGPU_RESOURCE_ID;
    g_cmd.attach.nr_entries = 1u;
    g_cmd.attach.entries[0].addr = (uint64_t)(uintptr_t)g_fb;
    g_cmd.attach.entries[0].length = (uint32_t)sizeof(g_fb);
    g_cmd.attach.entries[0].padding = 0u;
    /* 48 bytes for one entry: 24 (hdr) + 4 (res) + 4 (nr) + 16 (entry).  The
     * static_assert above pins it; a struct 8 bytes longer would make QEMU read
     * the entry at the wrong offset. */
    return vgpu_submit((uint32_t)sizeof(struct vgpu_cmd_attach_backing),
                       VGPU_RESP_OK_NODATA, NULL, "resource_attach_backing");
}

static int vgpu_cmd_set_scanout(uint32_t w, uint32_t h)
{
    vgpu_cmd_begin(VGPU_CMD_SET_SCANOUT);
    g_cmd.set_scanout.r.x = 0;
    g_cmd.set_scanout.r.y = 0;
    g_cmd.set_scanout.r.width = w;
    g_cmd.set_scanout.r.height = h;
    g_cmd.set_scanout.scanout_id = VGPU_SCANOUT_ID;
    g_cmd.set_scanout.resource_id = VGPU_RESOURCE_ID;
    return vgpu_submit((uint32_t)sizeof(struct vgpu_cmd_set_scanout),
                       VGPU_RESP_OK_NODATA, NULL, "set_scanout");
}

static int vgpu_cmd_transfer(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    vgpu_cmd_begin(VGPU_CMD_TRANSFER_TO_HOST_2D);
    g_cmd.transfer.r.x = x;
    g_cmd.transfer.r.y = y;
    g_cmd.transfer.r.width = w;
    g_cmd.transfer.r.height = h;
    /* Byte offset of the rect's top-left corner inside the backing store. */
    g_cmd.transfer.offset = (uint64_t)((uint64_t)y * g.w + x) * 4ull;
    g_cmd.transfer.resource_id = VGPU_RESOURCE_ID;
    return vgpu_submit((uint32_t)sizeof(struct vgpu_cmd_transfer_to_host_2d),
                       VGPU_RESP_OK_NODATA, NULL, "transfer_to_host_2d");
}

static int vgpu_cmd_flush_full(void)
{
    vgpu_cmd_begin(VGPU_CMD_RESOURCE_FLUSH);
    g_cmd.flush.r.x = 0;
    g_cmd.flush.r.y = 0;
    g_cmd.flush.r.width = g.w;
    g_cmd.flush.r.height = g.h;
    g_cmd.flush.resource_id = VGPU_RESOURCE_ID;
    return vgpu_submit((uint32_t)sizeof(struct vgpu_cmd_resource_flush),
                       VGPU_RESP_OK_NODATA, NULL, "resource_flush");
}

/* ===================== frame content =====================
 * Eight solid colour bands, one line of 8x16 text (SutureOS's own VGA font,
 * kernel/drivers/font.c - no third-party artwork), and a 64x64 white square
 * that slides horizontally.  The bands keep the host-side assertion exact
 * (compare the pixel to a known value), the text makes the frame readable as a
 * display output, and the moving square gives an unambiguous "the frame really
 * is animating" signal.  The text is drawn once and deliberately avoids pure
 * white (0xFFFFFF) so a host-side "find the white square" scan cannot pick it
 * up. */
static const uint32_t g_band_color[8] = {
    0x302018u, 0xA03020u, 0xA07020u, 0x608020u,
    0x208040u, 0x207080u, 0x204080u, 0x602080u
};

static uint32_t vgpu_band_at(uint32_t y)
{
    uint32_t idx = (y * 8u) / (g.h ? g.h : 1u);
    if (idx > 7u)
        idx = 7u;
    return g_band_color[idx];
}

static void vgpu_fill_bands(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t yy, xx;

    for (yy = y; yy < y + h && yy < g.h; yy++) {
        uint32_t c = vgpu_band_at(yy);
        uint32_t *row = g_fb + (uintptr_t)yy * g.w;
        for (xx = x; xx < x + w && xx < g.w; xx++)
            row[xx] = c;
    }
}

static void vgpu_fill_solid(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c)
{
    uint32_t yy, xx;

    for (yy = y; yy < y + h && yy < g.h; yy++) {
        uint32_t *row = g_fb + (uintptr_t)yy * g.w;
        for (xx = x; xx < x + w && xx < g.w; xx++)
            row[xx] = c;
    }
}

/* Transparent-background 8x16 text with the kernel's own font (MSB = leftmost
 * pixel, 16 bytes per glyph, ASCII 32..126). */
static void vgpu_text(uint32_t x, uint32_t y, const char *s, uint32_t color)
{
    while (*s) {
        uint8_t ch = (uint8_t)*s++;
        if (ch >= 32u && ch <= 126u) {
            const uint8_t *glyph = &font8x16[(uint32_t)(ch - 32u) * 16u];
            uint32_t row, col;
            for (row = 0; row < 16u && (y + row) < g.h; row++) {
                uint8_t bits = glyph[row];
                if (!bits)
                    continue;
                for (col = 0; col < 8u && (x + col) < g.w; col++) {
                    if (bits & (uint8_t)(0x80u >> col))
                        g_fb[(uintptr_t)(y + row) * g.w + x + col] = color;
                }
            }
        }
        x += 8u;
        if (x + 8u > g.w)
            break;
    }
}

/* Everything that does not change between frames: the eight bands and the
 * three title lines.  Drawn once when the backend is turned on. */
static void vgpu_paint_static(void)
{
    char line[64];

    vgpu_fill_bands(0u, 0u, g.w, g.h);
    vgpu_text(20u, 20u, "SutureOS virtio-gpu 2D backend", 0xE8E8E8u);
    (void)snprintf(line, sizeof(line), "scanout 0  %ux%u  B8G8R8A8",
                   (unsigned)g.w, (unsigned)g.h);
    vgpu_text(20u, 44u, line, 0xC8C8C8u);
    vgpu_text(20u, 68u, "TRANSFER_TO_HOST_2D + RESOURCE_FLUSH", 0xB0B0B0u);
}

static uint32_t vgpu_square_x(uint32_t frame)
{
    uint32_t span;

    if (g.w < VGPU_SQ + 128u)
        return 0u;
    span = g.w - VGPU_SQ - 64u;
    /* 53 is coprime with the usual span values, so the square visits many
     * distinct positions instead of cycling through a short orbit. */
    return 32u + (frame * 53u) % span;
}

static uint32_t vgpu_square_y(void)
{
    return (g.h > VGPU_SQ) ? (g.h - VGPU_SQ) / 2u : 0u;
}

/* ===================== bring-up ===================== */
int virtio_gpu_probe(void)
{
    uint8_t bus = 0, dev = 0, fn = 0;
    uint16_t did = 0;

    return (vgpu_pci_find(&bus, &dev, &fn, &did) == 0) ? 1 : 0;
}

/* Wait for device_status to reach the value the driver wrote (bounded). */
static int vgpu_wait_status(uint8_t want, uint32_t spins)
{
    while (g.cfg->device_status != want) {
        if (--spins == 0u)
            return -1;
        __asm__ volatile("pause");
    }
    return 0;
}

/* Map the BAR that holds the modern structures and turn the capability
 * offsets into pointers. */
static int vgpu_map_caps(void)
{
    uint64_t base;
    uint32_t span = 0;

    if (!g.common.found || !g.notify.found || !g.device.found) {
        console_printf("[vga2d] missing modern caps (common=%d notify=%d isr=%d device=%d)\n",
                       (uint64_t)g.common.found, (uint64_t)g.notify.found,
                       (uint64_t)g.isr.found, (uint64_t)g.device.found);
        return -1;
    }

    console_printf("[vga2d] caps common=bar%u+0x%x notify=bar%u+0x%x isr=bar%u+0x%x device=bar%u+0x%x\n",
                   (uint64_t)g.common.bar, (uint64_t)g.common.offset,
                   (uint64_t)g.notify.bar, (uint64_t)g.notify.offset,
                   (uint64_t)g.isr.bar, (uint64_t)g.isr.offset,
                   (uint64_t)g.device.bar, (uint64_t)g.device.offset);

    if (g.common.bar != g.notify.bar || g.common.bar != g.device.bar) {
        console_puts("[vga2d] structures live in different BARs; unsupported layout\n");
        return -1;
    }

    base = g.bar_base[g.common.bar];
    if (base == 0) {
        console_printf("[vga2d] BAR%u is not an assigned memory BAR\n",
                       (uint64_t)g.common.bar);
        return -1;
    }

    /* Map the whole window covering every structure (2 MiB identity pages). */
    if (g.common.offset + g.common.length > span) span = g.common.offset + g.common.length;
    if (g.notify.offset + g.notify.length > span) span = g.notify.offset + g.notify.length;
    if (g.isr.offset + g.isr.length > span)       span = g.isr.offset + g.isr.length;
    if (g.device.offset + g.device.length > span) span = g.device.offset + g.device.length;
    span = (span + 0xFFFu) & ~0xFFFu;
    if (span == 0u)
        span = 0x1000u;

    if (mmu_map_mmio_identity(base, (uint64_t)span) != 0) {
        console_printf("[vga2d] mmu_map_mmio_identity(0x%x, 0x%x) failed\n",
                       base, (uint64_t)span);
        return -1;
    }
    console_printf("[vga2d] mmio bar%u base=0x%x window=0x%x\n",
                   (uint64_t)g.common.bar, base, (uint64_t)span);

    g.cfg = (struct vgpu_common_cfg *)(void *)(uintptr_t)(base + g.common.offset);
    g.notify_area = (uint8_t *)(uintptr_t)(base + g.notify.offset);
    g.isr_area = (uint8_t *)(uintptr_t)(base + g.isr.offset);
    g.dev_cfg = (struct vgpu_device_cfg *)(void *)(uintptr_t)(base + g.device.offset);
    return 0;
}

static int vgpu_negotiate_features(void)
{
    uint32_t lo, hi;

    /* Reset: writing 0 to device_status and waiting for it to read 0. */
    g.cfg->device_status = 0u;
    if (vgpu_wait_status(0u, 1000000u) != 0) {
        console_puts("[vga2d] device did not reset\n");
        return -1;
    }

    g.cfg->device_status = VGPU_ST_ACK;
    g.cfg->device_status = (uint8_t)(VGPU_ST_ACK | VGPU_ST_DRIVER);

    g.cfg->device_feature_select = 0u;
    lo = g.cfg->device_feature;
    g.cfg->device_feature_select = 1u;
    hi = g.cfg->device_feature;
    g.features_lo = lo;
    g.features_hi = hi;

    console_printf("[vga2d] features lo=0x%x hi=0x%x\n",
                   (uint64_t)lo, (uint64_t)hi);

    /* VIRTIO_F_VERSION_1 is bit 32 => the low bit of the high feature word. */
    if ((hi & (1u << VGPU_FEATURE_VERSION1_BIT)) == 0u) {
        console_puts("[vga2d] device does not offer VIRTIO_F_VERSION_1 "
                     "(legacy-only virtio-gpu, modern 1.0 required)\n");
        g.cfg->device_status = (uint8_t)(VGPU_ST_ACK | VGPU_ST_DRIVER | VGPU_ST_FAILED);
        return -1;
    }

    g.cfg->driver_feature_select = 0u;
    g.cfg->driver_feature = 0u;                       /* no optional features  */
    g.cfg->driver_feature_select = VGPU_FEATURE_VERSION1_SEL;
    g.cfg->driver_feature = (uint32_t)(1u << VGPU_FEATURE_VERSION1_BIT);

    g.cfg->device_status = (uint8_t)(VGPU_ST_ACK | VGPU_ST_DRIVER | VGPU_ST_FEATURES_OK);
    if ((g.cfg->device_status & VGPU_ST_FEATURES_OK) == 0u) {
        console_puts("[vga2d] device rejected FEATURES_OK\n");
        g.cfg->device_status = (uint8_t)(VGPU_ST_ACK | VGPU_ST_DRIVER |
                                         VGPU_ST_FEATURES_OK | VGPU_ST_FAILED);
        return -1;
    }
    return 0;
}

/* Configure control queue 0: report the three ring addresses, enable it and
 * remember its doorbell offset. */
static int vgpu_setup_queue(void)
{
    uint16_t qsz;
    uint32_t avail_off, used_off;

    g.cfg->queue_select = 0u;
    qsz = g.cfg->queue_size;
    if (qsz == 0u) {
        console_puts("[vga2d] control queue 0 does not exist\n");
        return -1;
    }
    if (qsz > VGPU_QUEUE_MAX)
        qsz = VGPU_QUEUE_MAX;
    g.qsize = qsz;

    /* Lay out desc / avail / used inside the static arena.  The used ring is
     * pushed to a 4 KiB boundary (not required by the 1.0 spec, but it is what
     * every virtio device expects and it keeps the ring cache-line safe). */
    avail_off = 16u * (uint32_t)qsz;
    used_off  = (avail_off + 4u + 2u * (uint32_t)qsz + 0xFFFu) & ~0xFFFu;
    if (used_off + 4u + 8u * (uint32_t)qsz > VGPU_VQ_BYTES) {
        console_puts("[vga2d] virtqueue arena too small for the queue size\n");
        return -1;
    }

    memset(g_vq, 0, sizeof(g_vq));
    g.desc      = (struct vgpu_desc *)(void *)g_vq;
    g.avail     = (volatile uint16_t *)(void *)(g_vq + avail_off);
    g.used      = (volatile uint16_t *)(void *)(g_vq + used_off);
    g.used_idx  = g.used + 1;
    g.used_ring = (volatile struct vgpu_used_elem *)(void *)(g_vq + used_off + 4u);
    g.avail_idx = 0u;
    g.used_seen = 0u;

    g.cfg->queue_size = g.qsize;
    /* No MSI-X: this driver polls the used ring, and INTx is disabled in the
     * PCI command register, so the device must never assert an interrupt. */
    g.cfg->queue_msix_vector = 0xFFFFu;
    g.cfg->queue_desc   = (uint64_t)(uintptr_t)g_vq;
    g.cfg->queue_driver = (uint64_t)(uintptr_t)(g_vq + avail_off);
    g.cfg->queue_device = (uint64_t)(uintptr_t)(g_vq + used_off);
    g.cfg->queue_enable = 1u;
    g.q_notify_off = g.cfg->queue_notify_off;

    console_printf("[vga2d] queue0 size=%u desc=0x%x avail=0x%x used=0x%x notify_off=%u mult=%u\n",
                   (uint64_t)g.qsize,
                   (uint64_t)(uintptr_t)g_vq,
                   (uint64_t)(uintptr_t)(g_vq + avail_off),
                   (uint64_t)(uintptr_t)(g_vq + used_off),
                   (uint64_t)g.q_notify_off, (uint64_t)g.notify_mult);
    return 0;
}

int virtio_gpu_init(void)
{
    uint8_t  bus = 0, dev = 0, fn = 0;
    uint16_t did = 0;
    uint32_t cmd;

    if (g.up)
        return 0;

    if (!g.found) {
        if (vgpu_pci_find(&bus, &dev, &fn, &did) != 0) {
            console_puts("[vga2d] no virtio-gpu on PCI\n");
            return -1;
        }
        g.bus = bus; g.dev = dev; g.fn = fn; g.did = did; g.found = 1;

        /* Memory decode (the modern structures live in a memory BAR) + bus
         * master (the device DMAs the rings and the scanout backing).  INTx is
         * masked because this driver polls the used ring. */
        cmd = vgpu_pci_read32(bus, dev, fn, VGPU_CFG_COMMAND);
        vgpu_pci_write32(bus, dev, fn, VGPU_CFG_COMMAND,
                         cmd | VGPU_CMD_MEM_SPACE | VGPU_CMD_BUS_MASTER |
                         VGPU_CMD_INTX_DISABLE);

        g.n_caps = vgpu_scan_caps(bus, dev, fn);
        console_printf("[vga2d] pci %u:%u.%u id=%x:%x modern caps=%u\n",
                       (uint64_t)bus, (uint64_t)dev, (uint64_t)fn,
                       (uint64_t)VGPU_PCI_VENDOR, (uint64_t)did,
                       (uint64_t)g.n_caps);

        if (did == VGPU_PCI_DEV_LEGACY) {
            console_puts("[vga2d] device is legacy-only (0x1010); "
                         "modern virtio 1.0 required\n");
            return -1;
        }

        {
            uint8_t i;
            for (i = 0; i < 6u; i++)
                g.bar_base[i] = vgpu_pci_bar_base(bus, dev, fn, i);
        }

        if (vgpu_map_caps() != 0)
            return -1;
    } else if (!g.cfg) {
        /* A previous attempt stopped before the BAR was mapped. */
        return -1;
    }

    if (vgpu_negotiate_features() != 0)
        return -1;
    if (vgpu_setup_queue() != 0)
        return -1;

    g.cfg->device_status = (uint8_t)(VGPU_ST_ACK | VGPU_ST_DRIVER |
                                     VGPU_ST_FEATURES_OK | VGPU_ST_DRIVER_OK);
    if ((g.cfg->device_status & VGPU_ST_DRIVER_OK) == 0u) {
        console_puts("[vga2d] device did not accept DRIVER_OK\n");
        return -1;
    }

    g.num_scanouts = g.dev_cfg->num_scanouts;
    console_printf("[vga2d] driver ok status=0x%x num_scanouts=%u events=0x%x\n",
                   (uint64_t)g.cfg->device_status,
                   (uint64_t)g.num_scanouts,
                   (uint64_t)g.dev_cfg->events_read);
    if (g.num_scanouts == 0u) {
        console_puts("[vga2d] device reports no scanout, nothing to present to\n");
        return -1;
    }

    /* The control queue is live from here on, so vgpu_submit() may run.  The
     * flag is raised before GET_DISPLAY_INFO (which itself goes through the
     * queue) and dropped again if that first command fails. */
    g.up = 1;
    g.errors = 0;
    g.flushes = 0;
    g.frame = 0;

    if (vgpu_cmd_get_display_info() != 0) {
        g.up = 0;
        return -1;
    }
    return 0;
}

/* ===================== scanout + presentation ===================== */
/* Draw frame 0 and push the whole frame, then point the scanout at the
 * resource.  Called once per `vga2d on`. */
static int vgpu_attach_scanout(void)
{
    uint32_t w = g.disp_w, h = g.disp_h;

    if (w > VIRTIO_GPU_MAX_WIDTH)
        w = VIRTIO_GPU_MAX_WIDTH;
    if (h > VIRTIO_GPU_MAX_HEIGHT)
        h = VIRTIO_GPU_MAX_HEIGHT;
    if (w == 0u || h == 0u) {
        w = VIRTIO_GPU_MAX_WIDTH;
        h = VIRTIO_GPU_MAX_HEIGHT;
    }
    if (w != g.disp_w || h != g.disp_h)
        console_printf("[vga2d] clamp %ux%u -> %ux%u (static backing store limit)\n",
                       (uint64_t)g.disp_w, (uint64_t)g.disp_h,
                       (uint64_t)w, (uint64_t)h);

    g.w = w;
    g.h = h;

    if (vgpu_cmd_resource_create_2d(w, h) != 0)
        return -1;
    g.res_id = VGPU_RESOURCE_ID;
    console_printf("[vga2d] resource created id=%u %ux%u b8g8r8a8\n",
                   (uint64_t)g.res_id, (uint64_t)w, (uint64_t)h);

    if (vgpu_cmd_attach_backing() != 0)
        return -1;
    console_printf("[vga2d] backing attached addr=0x%x bytes=%u\n",
                   (uint64_t)(uintptr_t)g_fb, (uint64_t)sizeof(g_fb));

    if (vgpu_cmd_set_scanout(w, h) != 0)
        return -1;
    console_printf("[vga2d] scanout %u <- resource %u (%ux%u)\n",
                   (uint64_t)VGPU_SCANOUT_ID, (uint64_t)g.res_id,
                   (uint64_t)w, (uint64_t)h);

    /* First frame: full-screen band fill plus the square at frame 0. */
    vgpu_paint_static();
    g.sq_x = vgpu_square_x(0u);
    vgpu_fill_solid(g.sq_x, vgpu_square_y(), VGPU_SQ, VGPU_SQ, VGPU_SQ_COLOR);
    g.frame = 0u;

    if (vgpu_cmd_transfer(0u, 0u, g.w, g.h) != 0)
        return -1;
    if (vgpu_cmd_flush_full() != 0)
        return -1;
    g.flushes++;
    console_printf("[vga2d] flush #%u (full frame %ux%u)\n",
                   (uint64_t)g.flushes, (uint64_t)g.w, (uint64_t)g.h);
    return 0;
}

int virtio_gpu_on(void)
{
    if (!g.up) {
        if (virtio_gpu_init() != 0)
            return -1;
    }
    if (g.enabled) {
        console_puts("[vga2d] already on\n");
        return 0;
    }

    vgpu_lock();
    if (g.res_id == 0u) {
        if (vgpu_attach_scanout() != 0) {
            vgpu_unlock();
            console_puts("[vga2d] on failed\n");
            return -1;
        }
    } else {
        /* Re-enable on the resource that is still attached: repaint frame 0
         * so the screen is not frozen on a stale animation step. */
        vgpu_paint_static();
        g.frame = 0u;
        g.sq_x = vgpu_square_x(0u);
        vgpu_fill_solid(g.sq_x, vgpu_square_y(), VGPU_SQ, VGPU_SQ, VGPU_SQ_COLOR);
        if (vgpu_cmd_transfer(0u, 0u, g.w, g.h) != 0 ||
            vgpu_cmd_flush_full() != 0) {
            vgpu_unlock();
            console_puts("[vga2d] on failed (re-attach)\n");
            return -1;
        }
        g.flushes++;
        console_printf("[vga2d] flush #%u (re-attach)\n", (uint64_t)g.flushes);
    }
    vgpu_unlock();

    g.last_tick = g_tick_count;
    g.enabled = 1;
    console_printf("[vga2d] on (%ux%u scanout=%u, %u fps paced by the 100 Hz tick)\n",
                   (uint64_t)g.w, (uint64_t)g.h, (uint64_t)VGPU_SCANOUT_ID,
                   (uint64_t)(100u / VGPU_FRAME_TICKS));
    return 0;
}

int virtio_gpu_off(void)
{
    if (!g.up) {
        console_puts("[vga2d] off (device is not initialized, nothing to stop)\n");
        return 0;
    }
    if (!g.enabled) {
        console_puts("[vga2d] off (already off)\n");
        return 0;
    }
    g.enabled = 0;
    /* The resource and the scanout stay attached on purpose: the last frame
     * remains visible, and the host-side acceptance can prove the counter
     * stopped without the screen going dark. */
    console_printf("[vga2d] off after %u flush(es)\n", (uint64_t)g.flushes);
    return 0;
}

void virtio_gpu_service(void)
{
    uint32_t oldx, nx, y, lo, hi;
    uint64_t now;

    if (!g.enabled || !g.up)
        return;

    now = g_tick_count;
    if ((now - g.last_tick) < (uint64_t)VGPU_FRAME_TICKS)
        return;
    g.last_tick = now;

    y = vgpu_square_y();
    oldx = g.sq_x;
    g.frame++;
    nx = vgpu_square_x(g.frame);
    if (nx == oldx)
        return;

    vgpu_lock();

    /* Erase the old square back to its band colours, draw the new one, then
     * transfer only the strip that changed (2 x 64 x 64 x 4 bytes instead of
     * the full 3 MiB frame). */
    vgpu_fill_bands(oldx, y, VGPU_SQ, VGPU_SQ);
    vgpu_fill_solid(nx, y, VGPU_SQ, VGPU_SQ, VGPU_SQ_COLOR);
    g.sq_x = nx;

    lo = (oldx < nx) ? oldx : nx;
    hi = ((oldx > nx) ? oldx : nx) + VGPU_SQ;

    if (vgpu_cmd_transfer(lo, y, hi - lo, VGPU_SQ) == 0 &&
        vgpu_cmd_flush_full() == 0) {
        g.flushes++;
        console_printf("[vga2d] flush #%u x=%u\n",
                       (uint64_t)g.flushes, (uint64_t)nx);
    } else {
        g.enabled = 0;
        console_puts("[vga2d] presentation failed, backend stopped\n");
    }

    vgpu_unlock();
}

void virtio_gpu_status(void)
{
    if (!g.found && !g.up) {
        console_puts("[vga2d] state: not initialized "
                     "(type `vga2d on` to attach scanout 0)\n");
        if (virtio_gpu_probe())
            console_puts("[vga2d] a virtio-gpu PCI function is present\n");
        else
            console_puts("[vga2d] no virtio-gpu PCI function found\n");
        return;
    }
    console_printf("[vga2d] state: %s pci=%u:%u.%u id=%x:%x\n",
                   g.enabled ? "on" : (g.up ? "off" : "failed"),
                   (uint64_t)g.bus, (uint64_t)g.dev, (uint64_t)g.fn,
                   (uint64_t)VGPU_PCI_VENDOR, (uint64_t)g.did);
    console_printf("[vga2d] display=%ux%u scanout=%u resource=%u geometry=%ux%u\n",
                   (uint64_t)g.disp_w, (uint64_t)g.disp_h,
                   (uint64_t)g.disp_scanout, (uint64_t)g.res_id,
                   (uint64_t)g.w, (uint64_t)g.h);
    console_printf("[vga2d] queue0 size=%u flushes=%u frames=%u errors=%u\n",
                   (uint64_t)g.qsize, (uint64_t)g.flushes,
                   (uint64_t)g.frame, (uint64_t)g.errors);
}
