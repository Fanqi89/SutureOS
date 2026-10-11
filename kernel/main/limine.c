/* SutureOS: Parse Limine Framebuffer Tag (correct multiboot tag format) */
#include <stitch/types.h>
#include <stitch/console.h>

uint64_t limine_fb_addr = 0, limine_fb_width = 0, limine_fb_height = 0,
         limine_fb_pitch = 0, limine_fb_bpp = 0;

/* Limine tag IDs */
#define LIMINE_TAG_FRAMEBUFFER 8

/* multiboot tag header: type(u32), size(u32) */
struct limine_tag_hdr {
    uint32_t type;
    uint32_t size;
};

struct limine_framebuffer {
    uint64_t address;
    uint32_t width, height, pitch;
    uint16_t bpp;
    uint8_t  memory_model;
    uint8_t  red_mask_size, red_mask_shift;
    uint8_t  green_mask_size, green_mask_shift;
    uint8_t  blue_mask_size, blue_mask_shift;
    uint8_t  reserved;
} __attribute__((packed));

void limine_parse_fb(uint64_t mbi)
{
    if (!mbi) {
        console_puts("[limine] mbi=0\n");
        return;
    }
    console_printf("[limine] mbi=%p parsing tags...\n", (void*)mbi);
    
    /* multiboot info: total_size(u32), reserved(u32), then tags... */
    uint8_t *ptr = (uint8_t *)(mbi + 8);
    int count = 0;
    while (count < 32) {
        struct limine_tag_hdr *hdr = (struct limine_tag_hdr *)ptr;
        if (hdr->type == 0 && hdr->size == 0) {
            console_puts("[limine] end tag\n");
            break;  /* end tag */
        }
        console_printf("[limine] tag type=%u size=%u\n", hdr->type, hdr->size);
        if (hdr->type == LIMINE_TAG_FRAMEBUFFER) {
            struct limine_framebuffer *fb = (struct limine_framebuffer *)(ptr + 8);
            limine_fb_addr   = fb->address;
            limine_fb_width  = fb->width;
            limine_fb_height = fb->height;
            limine_fb_pitch  = fb->pitch;
            limine_fb_bpp    = fb->bpp;
            console_printf("[limine] FB: addr=%p %ux%u pitch=%u bpp=%u model=%u\n",
                           (void*)fb->address, fb->width, fb->height,
                           fb->pitch, fb->bpp, fb->memory_model);
            break;
        }
        /* advance to next tag (size includes header, aligned to 8) */
        uint32_t sz = (hdr->size + 7) & ~7;
        ptr += sz;
        count++;
    }
    if (!limine_fb_addr) {
        console_puts("[limine] No framebuffer tag (BIOS mode?)\n");
    }
}