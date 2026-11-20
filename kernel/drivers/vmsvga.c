/* SutureOS module: VMware SVGA II (vmsvga) 2D display backend
 *
 * Origin and licensing
 * --------------------
 * SutureOS original, written from the public VMware SVGA II register
 * specification (the `svga_reg.h` register/command numbering that VMware
 * publishes with open-vm-tools and that the Linux `vmwgfx` driver
 * implements) plus the observed behaviour of QEMU's `-device vmware-svga`
 * model, which was inspected from the outside (QMP `info pci` /`info qtree`
 * for the PCI layout, and the read-back values of every register this driver
 * logs).  No third-party source file was copied.  SutureOS is GPL-3.0.
 *
 * Hard constraints from the porting brief
 * --------------------------------------
 *  - Boot presentation / terminal / GUI must not change, and no boot-time
 *    self-test may be added.  Therefore nothing here runs at boot: kmain does
 *    not call this file.  The only entry points are the shell command
 *    `vmsvga [on|off|status]` and `vmsvga_service()`, which the shell idle
 *    loop calls and which returns on its first branch when the backend is
 *    off.  On a plain `-vga std` boot not one `[vmsvga]` line is printed and
 *    no PCI function matches (vendor 0x15AD device 0x0405 is absent).
 *  - This is an *extra* display output, not a replacement for the
 *    framebuffer console: fbcon / kernel/term / kernel/gui are never touched.
 *  - The scan buffer is a static .bss array (no PMM/heap dependency), capped
 *    at 1024x768x4 = 3 MiB, exactly like the virtio-gpu backend.
 *
 * Device model (what the hardware/firmware actually looks like)
 * ------------------------------------------------------------
 * PCI 15ad:0405, class 0x03 (display):
 *   BAR0 = I/O port window (16 bytes): the SVGA II index and value register
 *          pair.  Register access is a 32-bit write of the register number to
 *          port `bar0 + 0`, then a 32-bit read/write of the value at port
 *          `bar0 + 1`.  There is no MMIO register file on this device, so the
 *          register interface is verified by round-tripping SVGA_REG_ID
 *          (0x90000002 = SVGA_ID_2) instead of assuming it.
 *   BAR1 = VRAM / framebuffer window (16 MiB by default).  SVGA_REG_FB_START
 *          reports its physical address and SVGA_REG_FB_OFFSET the offset of
 *          the visible framebuffer inside it (0 on this device).
 *   BAR2 = command FIFO window (64 KiB).  SVGA_REG_MEM_START reports its
 *          physical address, SVGA_REG_MEM_SIZE its size.
 *
 * The FIFO is a ring of 32-bit words with a 4-word header, all offsets in
 * **bytes**: [0]=MIN, [1]=MAX, [2]=NEXT (guest write cursor), [3]=STOP
 * (device read cursor).  Commands are pushed at NEXT and published by
 * updating NEXT; SVGA_REG_SYNC then makes the device drain the ring and
 * SVGA_REG_BUSY reads back 0 when it is done.  Only SVGA_CMD_UPDATE (1) is
 * used: [1, x, y, w, h] tells the device to copy that rectangle of VRAM to
 * the screen.  The accelerated commands (RECT_FILL/RECT_COPY) are advertised
 * in SVGA_REG_CAPABILITIES but deliberately NOT used here: an unverified
 * command number desynchronises the FIFO and kills the display, which is the
 * one failure mode this backend must never cause.
 *
 * Resolution policy: the device's current WIDTH/HEIGHT is preferred, but the
 * firmware leaves the SVGA device in a VGA *text* mode (720x400), which is
 * not a 32bpp scanout, and a mode larger than the static backing store cannot
 * be presented at all.  Both cases are logged and the driver programs (or
 * clamps to) 1024x768 rather than silently pretending.
 */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/io.h>

#include "vmsvga.h"
#include "font.h"   /* font8x16: SutureOS's own 8x16 VGA font for the title line */
#include "mmu.h"    /* mmu_map_mmio_identity() for BAR1 (VRAM) and BAR2 (FIFO) */

/* System tick (100 Hz).  Declared exactly like kernel/shell/shell.c does; the
 * definition lives in kernel/interrupts/init.c. */
extern volatile uint64_t g_tick_count;

/* ===================== PCI config space (mechanism #1) =====================
 * DWORD accesses only: writing 0xCF8 byte-by-byte does not stick on QEMU.
 * Same idiom as virtio_gpu.c / virtio_blk.c / rtl8139.c. */
#define VW_PCI_CFG_ADDR   0xCF8u
#define VW_PCI_CFG_DATA   0xCFCu

#define VW_CFG_VENDOR     0x00u
#define VW_CFG_COMMAND    0x04u
#define VW_CFG_CLASS      0x08u
#define VW_CFG_HEADER     0x0Cu
#define VW_CFG_BAR0       0x10u

#define VW_CMD_IO_SPACE      (1u << 0)   /* decode the BAR0 port window     */
#define VW_CMD_MEM_SPACE     (1u << 1)   /* decode the BAR1/BAR2 windows    */
#define VW_CMD_INTX_DISABLE  (1u << 10)  /* pure polling, no IRQ handler    */

/* VMware PCI IDs.  0x15AD is VMware's vendor id; 0x0405 is the SVGA II
 * display adapter (the sibling 0x0710 is the SVGA II in "plain VGA" mode,
 * which this backend does not drive). */
#define VW_PCI_VENDOR        0x15ADu
#define VW_PCI_DEV_SVGA2     0x0405u
#define VW_PCI_CLASS_DISPLAY 0x03u

/* ===================== SVGA II registers (public specification) =========
 * Register numbers, not byte offsets: they are written to the index port. */
#define SVGA_REG_ID                 0u
#define SVGA_REG_ENABLE             1u
#define SVGA_REG_WIDTH              2u
#define SVGA_REG_HEIGHT             3u
#define SVGA_REG_MAX_WIDTH          4u
#define SVGA_REG_MAX_HEIGHT         5u
#define SVGA_REG_DEPTH              6u
#define SVGA_REG_BITS_PER_PIXEL     7u
#define SVGA_REG_RED_MASK           9u
#define SVGA_REG_GREEN_MASK         10u
#define SVGA_REG_BLUE_MASK          11u
#define SVGA_REG_BYTES_PER_LINE     12u
#define SVGA_REG_FB_START           13u
#define SVGA_REG_FB_OFFSET          14u
#define SVGA_REG_VRAM_SIZE          15u
#define SVGA_REG_FB_SIZE            16u
#define SVGA_REG_CAPABILITIES       17u
#define SVGA_REG_MEM_START          18u
#define SVGA_REG_MEM_SIZE           19u
#define SVGA_REG_CONFIG_DONE        20u
#define SVGA_REG_SYNC               21u
#define SVGA_REG_BUSY               22u
#define SVGA_REG_PITCHLOCK          32u

#define SVGA_ID_2                   0x90000002u
#define SVGA_ID_1                   0x90000001u
#define SVGA_ID_0                   0x90000000u

/* Capability bits, printed so the log shows what this device can do (the
 * accelerated commands are advertised but intentionally unused, see above). */
#define SVGA_CAP_RECT_FILL          (1u << 0)
#define SVGA_CAP_RECT_COPY          (1u << 1)
#define SVGA_CAP_CURSOR             (1u << 5)
#define SVGA_CAP_CURSOR_BYPASS      (1u << 6)
#define SVGA_CAP_CURSOR_BYPASS_2    (1u << 7)
#define SVGA_CAP_EXTENDED_FIFO      (1u << 15)

/* The single command this backend issues: copy a VRAM rectangle to the
 * screen.  5 dwords: {SVGA_CMD_UPDATE, x, y, width, height}. */
#define SVGA_CMD_UPDATE             1u
#define VW_CMD_UPDATE_DWORDS        5u
#define VW_CMD_UPDATE_BYTES         (VW_CMD_UPDATE_DWORDS * 4u)

/* ===================== FIFO ring ===================== */
#define VW_FIFO_MIN_BYTES   8192u   /* first usable byte: the 4-dword header
                                     * plus room for the extended-FIFO words */
#define VW_FIFO_HEADER_BYTES 16u    /* MIN / MAX / NEXT / STOP                */
#define VW_FIFO_MIN_GAP     (16u * 1024u)  /* the ring must be at least this big */

/* Bounded polls.  QEMU's model drains the FIFO synchronously inside the
 * SVGA_REG_SYNC write, so the poll body is normally never entered; the bound
 * only stops a wedged device from hanging the shell forever.  Two port I/Os
 * per register read = two VM-exits on a hypervisor, so the spin count stays
 * small and a pause is inserted between reads. */
#define VW_SYNC_SPINS       4000u

/* Frame pacing: one UPDATE every 5 system ticks (100 Hz) = 50 ms = 20 fps,
 * the same cadence as the virtio-gpu backend.  Each frame only repaints and
 * re-pushes the strip the moving square occupies. */
#define VW_FRAME_TICKS      5u

/* Animation geometry: a 64x64 white square sliding horizontally over eight
 * solid colour bands.  Both are exact-value friendly so the host-side
 * acceptance can assert pixels instead of eyeballing a screenshot. */
#define VW_SQ               64u
#define VW_SQ_COLOR         0xFFFFFFu

/* Smallest geometry the driver will adopt as "the device already has a real
 * scanout mode".  Smaller than this is the firmware's own surface, not a
 * graphics mode: this device reports 720x400 while it sits in the BIOS VGA
 * text mode, and 640x480 while it still shows the "guest has not initialized
 * the display" placeholder that the hypervisor creates for a fresh console.
 * Both must be replaced by a mode the driver programs, otherwise the device
 * compares our mode against that stale surface, decides nothing changed and
 * keeps presenting it. */
#define VW_MIN_WIDTH        800u
#define VW_MIN_HEIGHT       600u

/* ===================== 8 colour bands + title text =====================
 * The bands make the host-side assertions exact (compare a pixel to a known
 * value) and give the moving square a background to be erased back into. */
static const uint32_t g_band_color[8] = {
    0x302018u, 0xA03020u, 0xA07020u, 0x608020u,
    0x208040u, 0x207080u, 0x204080u, 0x602080u
};

/* ===================== static scan buffer (no allocator) =================
 * 1024x768x4 = 3 MiB of .bss, the brief's hard cap.  The kernel is
 * identity-mapped, so writing this buffer never depends on the PMM or heap. */
static uint32_t g_fb[VMSVGA_MAX_WIDTH * VMSVGA_MAX_HEIGHT]
    __attribute__((aligned(64)));

/* ===================== driver state ===================== */
static struct {
    int      found;          /* a VMware SVGA II PCI function was seen      */
    int      up;             /* registers + FIFO are usable                 */
    int      enabled;        /* `vmsvga on` in effect -> service runs       */

    uint8_t  bus, dev, fn;
    uint16_t did;
    uint16_t io_base;        /* BAR0 (I/O port window for index/value)      */

    uint64_t fb_phys;        /* BAR1 (or SVGA_REG_FB_START + FB_OFFSET)      */
    uint64_t fifo_phys;      /* BAR2 (or SVGA_REG_MEM_START)                */
    uint64_t bar1, bar2;     /* BAR1 / BAR2 as the PCI function decodes them */
    uint32_t fb_size;        /* SVGA_REG_FB_SIZE                            */
    uint32_t vram_size;      /* SVGA_REG_VRAM_SIZE                          */
    uint32_t fifo_size;      /* SVGA_REG_MEM_SIZE                           */
    uint32_t fb_offset;      /* SVGA_REG_FB_OFFSET                          */
    uint32_t caps;           /* SVGA_REG_CAPABILITIES                       */
    uint32_t max_w, max_h;   /* SVGA_REG_MAX_WIDTH / HEIGHT                 */

    uint8_t *fbdev;          /* mapped VRAM window (BAR1)                   */
    volatile uint32_t *fifo; /* mapped FIFO window (BAR2)                   */

    uint32_t fifo_min;       /* first usable FIFO byte                      */
    uint32_t fifo_max;       /* one past the last usable FIFO byte          */
    uint32_t next;           /* SVGA_FIFO_NEXT (guest write cursor, bytes)  */

    uint32_t dev_w, dev_h;   /* geometry the device reported before we set  */
    uint32_t w, h;           /* active scanout geometry                     */
    uint32_t bypl;           /* SVGA_REG_BYTES_PER_LINE (VRAM row stride)   */
    uint32_t sq_x;           /* current square x                            */
    uint32_t frame;          /* animation frames applied                    */
    uint64_t updates;        /* SVGA_CMD_UPDATE commands issued             */
    uint64_t errors;
    uint64_t last_tick;
} g;

static volatile uint8_t g_lock;    /* one command sequence at a time */

/* ===================== small helpers ===================== */
static inline void vw_lock(void)
{
    while (__atomic_test_and_set(&g_lock, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}

static inline void vw_unlock(void)
{
    __atomic_clear(&g_lock, __ATOMIC_RELEASE);
}

static inline void vw_barrier(void)
{
    __asm__ volatile("mfence" ::: "memory");
}

/* ===================== PCI config space ===================== */
static uint32_t vw_pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8)
                  | (uint32_t)(off & 0xFCu);
    uint32_t v;

    __asm__ volatile("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)VW_PCI_CFG_ADDR));
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)VW_PCI_CFG_DATA));
    return v;
}

static void vw_pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8)
                  | (uint32_t)(off & 0xFCu);

    __asm__ volatile("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)VW_PCI_CFG_ADDR));
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"((uint16_t)VW_PCI_CFG_DATA));
}

/* Read the base address of BAR `bar`.  Returns 1 for an I/O BAR (base is a
 * port number), 2 for a 32-bit memory BAR, 3 for a 64-bit memory BAR (which
 * occupies two BAR slots) and 0 for an unimplemented BAR. */
static int vw_pci_bar(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t bar,
                      uint64_t *base_out)
{
    uint8_t  off = (uint8_t)(VW_CFG_BAR0 + 4u * bar);
    uint32_t lo  = vw_pci_read32(bus, dev, fn, off);

    *base_out = 0;
    if (lo == 0u || lo == 0xFFFFFFFFu)          /* not implemented */
        return 0;

    if ((lo & 0x1u) != 0u) {                    /* I/O space BAR   */
        *base_out = (uint64_t)(lo & ~0x3u);
        return (*base_out == 0u) ? 0 : 1;
    }

    if (((lo >> 1) & 0x3u) == 0x2u) {           /* 64-bit memory   */
        uint32_t hi = vw_pci_read32(bus, dev, fn, (uint8_t)(off + 4u));
        *base_out = ((uint64_t)hi << 32) | (uint64_t)(lo & ~0xFu);
        return (*base_out == 0u) ? 0 : 3;
    }

    *base_out = (uint64_t)(lo & ~0xFu);         /* 32-bit memory   */
    return (*base_out == 0u) ? 0 : 2;
}

/* Does this PCI function look like a VMware SVGA II display adapter?  Both the
 * vendor id and the device id are matched, and the class code must be display
 * (0x03), so no unrelated VMware function can be mistaken for the GPU. */
static int vw_pci_match(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t *did_out)
{
    uint32_t id = vw_pci_read32(bus, dev, fn, VW_CFG_VENDOR);
    uint32_t did, cls;

    if ((id & 0xFFFFu) != VW_PCI_VENDOR)
        return 0;
    did = (id >> 16) & 0xFFFFu;
    if (did != VW_PCI_DEV_SVGA2)
        return 0;
    cls = (vw_pci_read32(bus, dev, fn, VW_CFG_CLASS) >> 24) & 0xFFu;
    if (cls != VW_PCI_CLASS_DISPLAY)
        return 0;
    *did_out = (uint16_t)did;
    return 1;
}

/* Scan PCI for the SVGA II display function. */
static int vw_pci_find(uint8_t *bus_out, uint8_t *dev_out, uint8_t *fn_out,
                       uint16_t *did_out)
{
    uint32_t bus, dev, fn;

    for (bus = 0; bus < 256u; bus++) {
        for (dev = 0; dev < 32u; dev++) {
            uint32_t hdr = vw_pci_read32((uint8_t)bus, (uint8_t)dev, 0u,
                                         VW_CFG_HEADER);
            /* Extra functions only exist when header-type bit 23 is set. */
            uint32_t fn_max = (hdr & 0x00800000u) ? 8u : 1u;

            for (fn = 0; fn < fn_max; fn++) {
                uint16_t did = 0;
                if (vw_pci_match((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, &did)) {
                    *bus_out = (uint8_t)bus; *dev_out = (uint8_t)dev;
                    *fn_out = (uint8_t)fn;   *did_out = did;
                    return 0;
                }
            }
        }
    }
    return -1;
}

/* ===================== register access (index/value port pair) =========
 * SVGA II exposes no MMIO register file: the register number goes to the
 * index port and the value is read/written at the value port, both 32-bit
 * accesses at consecutive I/O ports inside BAR0.  Whether that assumption
 * holds is *probed*, not assumed: vw_iface_probe() round-trips SVGA_REG_ID and
 * also reads SVGA_REG_MAX_WIDTH/HEIGHT, which a wrong interface reports as
 * 0xFFFFFFFF (an unmapped port read). */
static void vw_reg_write(uint32_t reg, uint32_t value)
{
    outl((uint16_t)(g.io_base + 0u), reg);      /* SVGA_INDEX_PORT */
    outl((uint16_t)(g.io_base + 1u), value);    /* SVGA_VALUE_PORT */
}

static uint32_t vw_reg_read(uint32_t reg)
{
    outl((uint16_t)(g.io_base + 0u), reg);
    return inl((uint16_t)(g.io_base + 1u));
}

/* ===================== FIFO ===================== */
static int vw_wait_idle(const char *what);   /* defined below */

/* Push one SVGA_CMD_UPDATE.  NEXT is in bytes, always 4-byte aligned; the
 * command is written before NEXT is published so the device never reads a
 * half-written command. */
static void vw_fifo_push_update(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    volatile uint32_t *p;

    /* Keep one command of headroom below MAX: QEMU (and the specification)
     * reject a NEXT/STOP outside [MIN, MAX), and a NEXT that lands exactly on
     * MAX would disable the FIFO. */
    if (g.next < g.fifo_min || g.next + VW_CMD_UPDATE_BYTES > g.fifo_max) {
        /* Ring full (or wrapped): drain, then restart both cursors at MIN.
         * STOP is only reset while the ring is empty, otherwise the device
         * would see a bogus NEXT-STOP delta. */
        (void)vw_wait_idle("fifo wrap");
        g.next = g.fifo_min;
        g.fifo[3] = g.fifo_min;                 /* SVGA_FIFO_STOP */
        g.fifo[2] = g.next;                     /* SVGA_FIFO_NEXT */
        vw_barrier();
    }

    p = (volatile uint32_t *)(void *)((uint8_t *)g.fifo + g.next);
    p[0] = SVGA_CMD_UPDATE;
    p[1] = x;
    p[2] = y;
    p[3] = w;
    p[4] = h;
    vw_barrier();

    g.next += VW_CMD_UPDATE_BYTES;
    if (g.next >= g.fifo_max)
        g.next = g.fifo_min;
    g.fifo[2] = g.next;                         /* publish */
    vw_barrier();
}

/* Write SVGA_REG_SYNC and wait for SVGA_REG_BUSY to drop back to 0.  This is
 * the round trip that proves the command channel really works: QEMU drains the
 * ring inside the SYNC write, so a working device answers 0 immediately. */
static int vw_wait_idle(const char *what)
{
    uint32_t spins = VW_SYNC_SPINS;

    vw_reg_write(SVGA_REG_SYNC, 1u);
    for (;;) {
        if (vw_reg_read(SVGA_REG_BUSY) == 0u)
            return 0;
        if (--spins == 0u) {
            console_printf("[vmsvga] sync TIMEOUT during %s (fifo next=%u max=%u stop=%u)\n",
                           what, (uint64_t)g.next, (uint64_t)g.fifo_max,
                           (uint64_t)(g.fifo ? g.fifo[3] : 0u));
            g.errors++;
            return -1;
        }
        /* Two port I/Os per poll are two VM-exits on a hypervisor: spread
         * them out instead of hammering the port. */
        for (int i = 0; i < 16; i++)
            __asm__ volatile("pause");
    }
}

/* ===================== frame content ===================== */
static uint32_t vw_band_at(uint32_t y)
{
    uint32_t idx = (y * 8u) / (g.h ? g.h : 1u);

    if (idx > 7u)
        idx = 7u;
    return g_band_color[idx];
}

static void vw_fill_bands(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t yy, xx;

    for (yy = y; yy < y + h && yy < g.h; yy++) {
        uint32_t c = vw_band_at(yy);
        uint32_t *row = g_fb + (uintptr_t)yy * g.w;
        for (xx = x; xx < x + w && xx < g.w; xx++)
            row[xx] = c;
    }
}

static void vw_fill_solid(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c)
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
static void vw_text(uint32_t x, uint32_t y, const char *s, uint32_t color)
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
static void vw_paint_static(void)
{
    char line[64];

    vw_fill_bands(0u, 0u, g.w, g.h);
    vw_text(20u, 20u, "SutureOS VMware SVGA II 2D backend", 0xE8E8E8u);
    (void)snprintf(line, sizeof(line), "scanout %ux%u  BGRX8888  pitch %u",
                   (unsigned)g.w, (unsigned)g.h, (unsigned)g.bypl);
    vw_text(20u, 44u, line, 0xC8C8C8u);
    vw_text(20u, 68u, "SVGA_CMD_UPDATE over the BAR2 command FIFO", 0xB0B0B0u);
}

static uint32_t vw_square_x(uint32_t frame)
{
    uint32_t span;

    if (g.w < VW_SQ + 128u)
        return 0u;
    span = g.w - VW_SQ - 64u;
    /* 53 is coprime with the usual span values, so the square visits many
     * distinct positions instead of cycling through a short orbit. */
    return 32u + (frame * 53u) % span;
}

static uint32_t vw_square_y(void)
{
    return (g.h > VW_SQ) ? (g.h - VW_SQ) / 2u : 0u;
}

/* ===================== presentation =====================
 * Copy a rectangle of the static scan buffer into VRAM honouring the device's
 * own row stride (SVGA_REG_BYTES_PER_LINE), then tell the device the rectangle
 * changed and wait for it to finish. */
static void vw_blit_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t r;

    if (g.fbdev == NULL || g.bypl == 0u || w == 0u || h == 0u)
        return;
    if (x >= g.w || y >= g.h)
        return;
    if (x + w > g.w)
        w = g.w - x;
    if (y + h > g.h)
        h = g.h - y;

    for (r = 0; r < h; r++) {
        memcpy(g.fbdev + (uint64_t)(y + r) * g.bypl + (uint64_t)x * 4u,
               &g_fb[(uintptr_t)(y + r) * g.w + x],
               (size_t)w * 4u);
    }
    vw_barrier();
}

static int vw_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    vw_blit_rect(x, y, w, h);
    vw_fifo_push_update(x, y, w, h);
    if (vw_wait_idle("update") != 0)
        return -1;
    g.updates++;
    return 0;
}

/* ===================== bring-up ===================== */
int vmsvga_probe(void)
{
    uint8_t bus = 0, dev = 0, fn = 0;
    uint16_t did = 0;

    return (vw_pci_find(&bus, &dev, &fn, &did) == 0) ? 1 : 0;
}

/* Probe the index/value port pair and latch SVGA_ID_2.  Honest-failure rule:
 * if the ID does not round-trip, nothing else is touched and the reason is
 * printed - never "assume the interface and hope". */
static int vw_iface_probe(void)
{
    uint32_t id_in, id_out;

    id_in = vw_reg_read(SVGA_REG_ID);
    console_printf("[vmsvga] id read 0x%x (index/value ports at 0x%x/0x%x)\n",
                   (uint64_t)id_in, (uint64_t)(g.io_base + 0u),
                   (uint64_t)(g.io_base + 1u));

    vw_reg_write(SVGA_REG_ID, SVGA_ID_2);
    id_out = vw_reg_read(SVGA_REG_ID);
    if (id_out != SVGA_ID_2) {
        /* Older silicon only knows SVGA_ID_1; accept it, but say which. */
        vw_reg_write(SVGA_REG_ID, SVGA_ID_1);
        id_out = vw_reg_read(SVGA_REG_ID);
        if (id_out != SVGA_ID_1) {
            console_printf("[vmsvga] index/value interface not usable: "
                           "SVGA_REG_ID wrote 0x%x read 0x%x (ports 0x%x/0x%x)\n",
                           (uint64_t)SVGA_ID_2, (uint64_t)id_out,
                           (uint64_t)(g.io_base + 0u),
                           (uint64_t)(g.io_base + 1u));
            g.errors++;
            return -1;
        }
        console_puts("[vmsvga] note: device latched SVGA_ID_1 (no SVGA_ID_2)\n");
    }
    console_printf("[vmsvga] id=0x%x accepted (SVGA_ID_2=%s)\n",
                   (uint64_t)id_out,
                   id_out == SVGA_ID_2 ? "yes" : "no, id 1 only");
    return 0;
}

/* Read and record the register file.  Every value that the rest of the driver
 * depends on (framebuffer address/size, FIFO address/size, capability bits) is
 * taken from the device, never hard-coded. */
static void vw_read_ident(void)
{
    g.max_w = vw_reg_read(SVGA_REG_MAX_WIDTH);
    g.max_h = vw_reg_read(SVGA_REG_MAX_HEIGHT);
    g.fb_phys = (uint64_t)vw_reg_read(SVGA_REG_FB_START);
    g.fb_offset = vw_reg_read(SVGA_REG_FB_OFFSET);
    g.fb_size = vw_reg_read(SVGA_REG_FB_SIZE);
    g.vram_size = vw_reg_read(SVGA_REG_VRAM_SIZE);
    g.fifo_phys = (uint64_t)vw_reg_read(SVGA_REG_MEM_START);
    g.fifo_size = vw_reg_read(SVGA_REG_MEM_SIZE);
    g.caps = vw_reg_read(SVGA_REG_CAPABILITIES);
    g.dev_w = vw_reg_read(SVGA_REG_WIDTH);
    g.dev_h = vw_reg_read(SVGA_REG_HEIGHT);
}

/* Choose and program the scanout mode.
 *
 * The device's current WIDTH/HEIGHT is preferred when it already looks like a
 * real graphics scanout (>= 800x600 and inside the static scan buffer).  When
 * it does not - it reads back 720x400 in the BIOS text mode and 640x480 while
 * it still shows the hypervisor's "guest has not initialized the display"
 * placeholder - 1024x768 is programmed instead, because the device only
 * latches a scanout mode when one is programmed and otherwise keeps comparing
 * our mode against that stale surface.  Every decision is logged with the
 * value it was based on, and no value is used that was not read back. */
static int vw_setup_mode(void)
{
    uint32_t w = g.dev_w, h = g.dev_h, bpp;
    int      reprogrammed = 0;

    if (w > VMSVGA_MAX_WIDTH || h > VMSVGA_MAX_HEIGHT ||
        (uint64_t)w * (uint64_t)h * 4ull > (uint64_t)sizeof(g_fb)) {
        uint32_t cw = w, ch = h;
        if (cw > VMSVGA_MAX_WIDTH)  cw = VMSVGA_MAX_WIDTH;
        if (ch > VMSVGA_MAX_HEIGHT) ch = VMSVGA_MAX_HEIGHT;
        console_printf("[vmsvga] clamp %ux%u -> %ux%u (static scan buffer is %ux%u)\n",
                       (uint64_t)w, (uint64_t)h, (uint64_t)cw, (uint64_t)ch,
                       (uint64_t)VMSVGA_MAX_WIDTH, (uint64_t)VMSVGA_MAX_HEIGHT);
        w = cw;
        h = ch;
        reprogrammed = 1;
    } else if (w < VW_MIN_WIDTH || h < VW_MIN_HEIGHT) {
        console_printf("[vmsvga] device reports %ux%u - the firmware's own "
                       "surface, not a latched graphics mode; program %ux%u\n",
                       (uint64_t)w, (uint64_t)h,
                       (uint64_t)VMSVGA_MAX_WIDTH, (uint64_t)VMSVGA_MAX_HEIGHT);
        w = VMSVGA_MAX_WIDTH;
        h = VMSVGA_MAX_HEIGHT;
        reprogrammed = 1;
    }

    /* The mode registers are written even when the device's own geometry is
     * kept: the device only latches a scanout mode when WIDTH/HEIGHT are
     * programmed, and writing it back is what makes the device compare and
     * install a scanout surface at all. */
    vw_reg_write(SVGA_REG_WIDTH, w);
    vw_reg_write(SVGA_REG_HEIGHT, h);
    if (!reprogrammed)
        console_printf("[vmsvga] keeping device mode %ux%u (latched by writing it back)\n",
                       (uint64_t)w, (uint64_t)h);
    /* This device only supports 32 bits per pixel: writing anything else is
     * rejected and clears CONFIG_DONE, so it is written unconditionally and
     * read back below. */
    vw_reg_write(SVGA_REG_BITS_PER_PIXEL, 32u);

    {
        uint32_t w2 = vw_reg_read(SVGA_REG_WIDTH);
        uint32_t h2 = vw_reg_read(SVGA_REG_HEIGHT);
        bpp = vw_reg_read(SVGA_REG_BITS_PER_PIXEL);
        if (w2 != w || h2 != h || bpp != 32u) {
            console_printf("[vmsvga] mode rejected: asked %ux%u/32, read back %ux%u/%u\n",
                           (uint64_t)w, (uint64_t)h, (uint64_t)w2, (uint64_t)h2,
                           (uint64_t)bpp);
            g.errors++;
            return -1;
        }
    }

    g.w = w;
    g.h = h;
    g.bypl = vw_reg_read(SVGA_REG_BYTES_PER_LINE);
    if (g.bypl < g.w * 4u)
        g.bypl = g.w * 4u;              /* never trust a smaller stride */

    console_printf("[vmsvga] mode %ux%u bpp=%u bypl=%u (device mode was %ux%u)\n",
                   (uint64_t)g.w, (uint64_t)g.h, (uint64_t)bpp, (uint64_t)g.bypl,
                   (uint64_t)g.dev_w, (uint64_t)g.dev_h);
    return 0;
}

/* Map BAR1 (VRAM) and BAR2 (FIFO) with 2 MiB identity pages: both windows can
 * sit above the 1 GiB boot identity map, which is exactly the case where a raw
 * dereference faults (see the framebuffer/ACPI notes in docs). */
static int vw_map_windows(void)
{
    uint64_t fb_map, fifo_map;

    if (g.fb_phys == 0u) {
        console_puts("[vmsvga] SVGA_REG_FB_START is 0: no framebuffer window\n");
        return -1;
    }
    if (g.fifo_phys == 0u || g.fifo_size < VW_FIFO_MIN_GAP + VW_FIFO_MIN_BYTES) {
        console_printf("[vmsvga] FIFO unusable: addr=0x%x size=%u\n",
                       g.fifo_phys, (uint64_t)g.fifo_size);
        return -1;
    }

    fb_map = (uint64_t)g.bypl * (uint64_t)g.h;
    fb_map = (fb_map + 0x1FFFFFull) & ~0x1FFFFFull;      /* round up to 2 MiB */
    if (fb_map == 0u)
        fb_map = 0x200000ull;

    if (mmu_map_mmio_identity(g.fb_phys, fb_map) != 0) {
        console_printf("[vmsvga] mmu_map_mmio_identity(0x%x, 0x%x) failed (VRAM)\n",
                       g.fb_phys, fb_map);
        return -1;
    }

    fifo_map = ((uint64_t)g.fifo_size + 0x1FFFFFull) & ~0x1FFFFFull;
    if (mmu_map_mmio_identity(g.fifo_phys, fifo_map) != 0) {
        console_printf("[vmsvga] mmu_map_mmio_identity(0x%x, 0x%x) failed (FIFO)\n",
                       g.fifo_phys, fifo_map);
        return -1;
    }

    g.fbdev = (uint8_t *)(uintptr_t)g.fb_phys;
    g.fifo = (volatile uint32_t *)(uintptr_t)g.fifo_phys;

    console_printf("[vmsvga] mapped vram=0x%x+0x%x fifo=0x%x+0x%x\n",
                   g.fb_phys, fb_map, g.fifo_phys, fifo_map);
    return 0;
}

/* Lay out the command ring: header words at the front, the command area from
 * fifo_min to fifo_max, all cursors in BYTES. */
static int vw_fifo_config(void)
{
    g.fifo_min = VW_FIFO_MIN_BYTES;
    g.fifo_max = g.fifo_size;
    if (g.fifo_max < g.fifo_min + VW_FIFO_MIN_GAP) {
        console_printf("[vmsvga] FIFO too small: size=%u min=%u gap=%u\n",
                       (uint64_t)g.fifo_max, (uint64_t)g.fifo_min,
                       (uint64_t)VW_FIFO_MIN_GAP);
        g.errors++;
        return -1;
    }
    /* the cursors must stay 4-byte aligned and below MAX */
    g.fifo_max &= ~0x3u;
    g.next = g.fifo_min;

    vw_reg_write(SVGA_REG_CONFIG_DONE, 0u);
    g.fifo[0] = g.fifo_min;         /* SVGA_FIFO_MIN  */
    g.fifo[1] = g.fifo_max;         /* SVGA_FIFO_MAX  */
    g.fifo[2] = g.next;             /* SVGA_FIFO_NEXT */
    g.fifo[3] = g.next;             /* SVGA_FIFO_STOP */
    vw_barrier();
    vw_reg_write(SVGA_REG_CONFIG_DONE, 1u);

    {
        uint32_t done = vw_reg_read(SVGA_REG_CONFIG_DONE);
        const char *sync = "FAIL";

        if (done != 1u) {
            console_printf("[vmsvga] CONFIG_DONE did not stick (read %u)\n",
                           (uint64_t)done);
            g.errors++;
            return -1;
        }
        if (vw_wait_idle("fifo cfg") == 0)
            sync = "ok";
        console_printf("[vmsvga] fifo cfg min=%u max=%u units=bytes next=%u sync=%s\n",
                       (uint64_t)g.fifo_min, (uint64_t)g.fifo_max,
                       (uint64_t)g.next, sync);
        if (sync[0] != 'o')
            return -1;
    }
    return 0;
}

int vmsvga_init(void)
{
    uint8_t  bus = 0, dev = 0, fn = 0;
    uint16_t did = 0;
    uint64_t bar0 = 0;
    int      k0;
    uint32_t cmd;

    if (g.up)
        return 0;

    if (!g.found) {
        if (vw_pci_find(&bus, &dev, &fn, &did) != 0) {
            console_puts("[vmsvga] no VMware SVGA II on PCI (id 15ad:0405)\n");
            return -1;
        }
        g.bus = bus; g.dev = dev; g.fn = fn; g.did = did; g.found = 1;

        k0 = vw_pci_bar(bus, dev, fn, 0u, &bar0);
        (void)vw_pci_bar(bus, dev, fn, 1u, &g.bar1);
        (void)vw_pci_bar(bus, dev, fn, 2u, &g.bar2);
        console_printf("[vmsvga] pci %u:%u.%u id=%x:%04x bar0=0x%x bar1=0x%x bar2=0x%x\n",
                       (uint64_t)bus, (uint64_t)dev, (uint64_t)fn,
                       (uint64_t)VW_PCI_VENDOR, (uint64_t)did,
                       bar0, g.bar1, g.bar2);

        if (k0 != 1) {
            console_printf("[vmsvga] BAR0 is not an I/O window (kind=%d); the "
                           "index/value register pair needs it\n", (uint64_t)k0);
            return -1;
        }
        g.io_base = (uint16_t)bar0;

        /* I/O decode for the register ports, memory decode for VRAM/FIFO.
         * INTx is masked: this driver polls, so the device must stay quiet. */
        cmd = vw_pci_read32(bus, dev, fn, VW_CFG_COMMAND);
        vw_pci_write32(bus, dev, fn, VW_CFG_COMMAND,
                       cmd | VW_CMD_IO_SPACE | VW_CMD_MEM_SPACE |
                       VW_CMD_INTX_DISABLE);

        if (vw_iface_probe() != 0)
            return -1;
    }

    /* Re-reading the register file on every attempt keeps a failed bring-up
     * retryable (`vmsvga on` again after a fixed problem). */
    vw_read_ident();

    if (vw_setup_mode() != 0)
        return -1;

    /* SVGA_REG_FB_START / MEM_START are authoritative for where the windows
     * are; BAR1/BAR2 are the same addresses on this device.  Fall back to the
     * BAR value only when the register reads back empty, and say so. */
    if (g.fb_phys == 0u && g.bar1 != 0u) {
        g.fb_phys = g.bar1;
        console_printf("[vmsvga] SVGA_REG_FB_START empty, using BAR1=0x%x\n",
                       g.fb_phys);
    }
    if (g.fifo_phys == 0u && g.bar2 != 0u) {
        g.fifo_phys = g.bar2;
        console_printf("[vmsvga] SVGA_REG_MEM_START empty, using BAR2=0x%x\n",
                       g.fifo_phys);
    }

    if (vw_map_windows() != 0)
        return -1;

    if (vw_fifo_config() != 0)
        return -1;

    /* Take the display over only after the mode and the command channel are
     * proven usable, so a failed bring-up leaves the firmware's screen alone. */
    vw_reg_write(SVGA_REG_ENABLE, 1u);
    if (vw_reg_read(SVGA_REG_ENABLE) != 1u) {
        console_puts("[vmsvga] ENABLE did not stick\n");
        g.errors++;
        return -1;
    }

    console_printf("[vmsvga] id=0x%x regs w=%u h=%u bpp=32 fb_offset=%u fb_size=%u "
                   "fifo=0x%x size=%u\n",
                   (uint64_t)SVGA_ID_2, (uint64_t)g.w, (uint64_t)g.h,
                   (uint64_t)g.fb_offset, (uint64_t)g.fb_size,
                   g.fifo_phys, (uint64_t)g.fifo_size);
    console_printf("[vmsvga] caps=0x%x max=%ux%u vram=%u bypl=%u "
                   "(rect_fill=%u rect_copy=%u cursor=%u extended_fifo=%u)\n",
                   (uint64_t)g.caps, (uint64_t)g.max_w, (uint64_t)g.max_h,
                   (uint64_t)g.vram_size, (uint64_t)g.bypl,
                   (uint64_t)((g.caps & SVGA_CAP_RECT_FILL) ? 1u : 0u),
                   (uint64_t)((g.caps & SVGA_CAP_RECT_COPY) ? 1u : 0u),
                   (uint64_t)((g.caps & SVGA_CAP_CURSOR) ? 1u : 0u),
                   (uint64_t)((g.caps & SVGA_CAP_EXTENDED_FIFO) ? 1u : 0u));
    console_printf("[vmsvga] masks r=%x g=%x b=%x (BGRX8888 expected)\n",
                   (uint64_t)vw_reg_read(SVGA_REG_RED_MASK),
                   (uint64_t)vw_reg_read(SVGA_REG_GREEN_MASK),
                   (uint64_t)vw_reg_read(SVGA_REG_BLUE_MASK));

    g.up = 1;
    g.errors = 0;
    g.updates = 0;
    g.frame = 0;
    return 0;
}

/* ===================== opt-in control ===================== */
int vmsvga_on(void)
{
    if (!g.up) {
        if (vmsvga_init() != 0)
            return -1;
    }
    if (g.enabled) {
        console_puts("[vmsvga] already on\n");
        return 0;
    }

    vw_lock();
    /* Draw frame 0 and push the whole frame, so the very first presented
     * picture is complete even if the device never refreshed before. */
    vw_paint_static();
    g.frame = 0u;
    g.sq_x = vw_square_x(0u);
    vw_fill_solid(g.sq_x, vw_square_y(), VW_SQ, VW_SQ, VW_SQ_COLOR);
    if (vw_present(0u, 0u, g.w, g.h) != 0) {
        vw_unlock();
        console_puts("[vmsvga] on failed: the first full-frame update was rejected\n");
        return -1;
    }
    vw_unlock();

    g.last_tick = g_tick_count;
    g.enabled = 1;
    console_printf("[vmsvga] update #%u full %ux%u\n",
                   (uint64_t)g.updates, (uint64_t)g.w, (uint64_t)g.h);
    console_printf("[vmsvga] on (%ux%u)\n", (uint64_t)g.w, (uint64_t)g.h);
    return 0;
}

int vmsvga_off(void)
{
    if (!g.up) {
        console_puts("[vmsvga] off (device is not initialized, nothing to stop)\n");
        return 0;
    }
    if (!g.enabled) {
        console_puts("[vmsvga] off (already off)\n");
        return 0;
    }
    g.enabled = 0;
    /* The scanout stays enabled and the last frame stays in VRAM; whatever
     * happens on screen next is up to the caller.  The shell re-enables the
     * terminal after this (which repaints its grid over the frame), so the
     * host-side acceptance asserts that the terminal has the screen back
     * rather than that the frame is still visible. */
    console_printf("[vmsvga] off after %u updates\n", (uint64_t)g.updates);
    return 0;
}

void vmsvga_service(void)
{
    uint32_t oldx, nx, y, lo, hi;
    uint64_t now;

    if (!g.enabled || !g.up)
        return;

    now = g_tick_count;
    if ((now - g.last_tick) < (uint64_t)VW_FRAME_TICKS)
        return;
    g.last_tick = now;

    y = vw_square_y();
    oldx = g.sq_x;
    g.frame++;
    nx = vw_square_x(g.frame);
    if (nx == oldx)
        return;

    vw_lock();

    /* Erase the old square back to its band colours and draw the new one. */
    vw_fill_bands(oldx, y, VW_SQ, VW_SQ);
    vw_fill_solid(nx, y, VW_SQ, VW_SQ, VW_SQ_COLOR);
    g.sq_x = nx;

    /* One UPDATE for the union strip that changed (at most 2*64 pixels wide),
     * not for the whole 3 MiB frame. */
    lo = (oldx < nx) ? oldx : nx;
    hi = ((oldx > nx) ? oldx : nx) + VW_SQ;
    if (vw_present(lo, y, hi - lo, VW_SQ) == 0) {
        console_printf("[vmsvga] update #%u x=%u\n",
                       (uint64_t)g.updates, (uint64_t)nx);
    } else {
        g.enabled = 0;
        console_puts("[vmsvga] presentation failed, backend stopped\n");
    }

    vw_unlock();
}

void vmsvga_status(void)
{
    if (!g.found && !g.up) {
        console_puts("[vmsvga] state: not initialized "
                     "(type `vmsvga on` to attach the scanout)\n");
        if (vmsvga_probe())
            console_puts("[vmsvga] a VMware SVGA II PCI function (15ad:0405) is present\n");
        else
            console_puts("[vmsvga] no VMware SVGA II PCI function found\n");
        return;
    }

    console_printf("[vmsvga] state: %s pci=%u:%u.%u id=%x:%04x\n",
                   g.enabled ? "on" : (g.up ? "off" : "failed"),
                   (uint64_t)g.bus, (uint64_t)g.dev, (uint64_t)g.fn,
                   (uint64_t)VW_PCI_VENDOR, (uint64_t)g.did);
    console_printf("[vmsvga] display=%ux%u (device reported %ux%u) scanout=%ux%u "
                   "bypl=%u caps=0x%x\n",
                   (uint64_t)g.w, (uint64_t)g.h, (uint64_t)g.dev_w,
                   (uint64_t)g.dev_h, (uint64_t)g.w, (uint64_t)g.h,
                   (uint64_t)g.bypl, (uint64_t)g.caps);
    console_printf("[vmsvga] fifo=%u/%u next=%u fb=0x%x size=%u updates=%u frames=%u errors=%u\n",
                   (uint64_t)g.fifo_min, (uint64_t)g.fifo_max, (uint64_t)g.next,
                   g.fb_phys, (uint64_t)g.fb_size, (uint64_t)g.updates,
                   (uint64_t)g.frame, (uint64_t)g.errors);
}
