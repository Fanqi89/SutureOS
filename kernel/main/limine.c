/* SutureOS: framebuffer hand-off (multiboot1 information structure)
 *
 * The kernel is booted through Limine in MULTIBOOT1 mode (boot/limine.conf has
 * `protocol: multiboot`), so `mbi` points at a multiboot1 information
 * structure - NOT at a Limine tag stream. The previous version walked it as
 * Limine tags (producing garbage like "tag type=260992") and therefore always
 * ended with limine_fb_addr = 0, leaving fbcon/GUI/terminal without anything
 * to draw on.
 *
 * boot.S now sets multiboot header flag bit 2 (video mode request:
 * 1024x768x32 linear), and this file reads the resulting framebuffer fields
 * (multiboot1 spec: flags bit 12, fields at offsets 88..111) and identity-maps
 * the window into the page tables, because a linear framebuffer usually lives
 * above the 1 GiB boot identity map (e.g. 0xFD000000).
 */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "mmu.h"

uint64_t limine_fb_addr = 0, limine_fb_width = 0, limine_fb_height = 0,
         limine_fb_pitch = 0, limine_fb_bpp = 0;

#define MB_FLAG_FRAMEBUFFER (1u << 12)

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* Identity-map the framebuffer window with 2 MiB pages and only then publish
 * it to the rest of the kernel: an unmapped framebuffer would #PF on the first
 * pixel write, which is exactly the trap fbcon/GUI used to guard against by
 * refusing addresses >= 1 GiB. */
static int limine_map_fb(uint64_t addr, uint32_t pitch, uint32_t height)
{
    uint64_t bytes = (uint64_t)pitch * (uint64_t)height;
    console_printf("[fb] mapping %llu KiB at %p into the page tables\n",
                   (unsigned long long)(bytes / 1024), (void *)addr);
    if (mmu_map_mmio_identity(addr, bytes) != 0) {
        console_puts("[fb] ERROR: cannot map framebuffer window\n");
        return -1;
    }
    return 0;
}

void limine_parse_fb(uint64_t mbi)
{
    if (!mbi) {
        console_puts("[fb] no multiboot information pointer\n");
        return;
    }

    const uint8_t *b = (const uint8_t *)(uintptr_t)mbi;
    uint32_t flags = rd32(b + 0);
    console_printf("[fb] multiboot1 info at %p flags=0x%x mem_lower=%u KiB mem_upper=%u KiB\n",
                   (void *)mbi, (uint64_t)flags,
                   (uint64_t)rd32(b + 4), (uint64_t)rd32(b + 8));

    if (!(flags & MB_FLAG_FRAMEBUFFER)) {
        console_puts("[fb] no framebuffer info (multiboot flags bit 12 clear)\n");
        return;
    }

    /* multiboot1 framebuffer fields (valid when flags bit 12 is set) */
    uint64_t addr  = rd64(b + 88);
    uint32_t pitch = rd32(b + 96);
    uint32_t width = rd32(b + 100);
    uint32_t height = rd32(b + 104);
    uint8_t  bpp   = b[108];
    uint8_t  type  = b[109];

    console_printf("[fb] raw: addr=%p %ux%u pitch=%u bpp=%u type=%u\n",
                   (void *)addr, (uint64_t)width, (uint64_t)height,
                   (uint64_t)pitch, (uint64_t)bpp, (uint64_t)type);

    if (!addr || width == 0 || height == 0 || width > 8192 || height > 8192) {
        console_puts("[fb] ERROR: implausible framebuffer geometry, ignored\n");
        return;
    }
    if (bpp != 32 && bpp != 24) {
        console_printf("[fb] unsupported colour depth %u bpp (need 24 or 32)\n",
                       (uint64_t)bpp);
        return;
    }
    if (pitch < (uint32_t)(width * (bpp / 8))) {
        console_puts("[fb] ERROR: pitch smaller than one scanline, ignored\n");
        return;
    }
    if (limine_map_fb(addr, pitch, height) != 0) return;

    limine_fb_addr   = addr;
    limine_fb_width  = width;
    limine_fb_height = height;
    limine_fb_pitch  = pitch;
    limine_fb_bpp    = bpp;
    console_printf("[fb] ready: %ux%u %u bpp pitch=%u at %p\n",
                   (uint64_t)width, (uint64_t)height, (uint64_t)bpp,
                   (uint64_t)pitch, (void *)addr);
}
