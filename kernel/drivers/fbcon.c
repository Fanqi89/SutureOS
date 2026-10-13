/* SutureOS: UEFI GOP Framebuffer Console with serial fallback */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "fbcon.h"
#include "font.h"
#include "intr.h"  /* for timer ticks */

static uint32_t *fb = NULL;          /* 帧缓冲虚拟地址 */
static uint64_t fb_phys = 0;         /* 物理地址 */
static uint32_t fb_w = 0, fb_h = 0, fb_pitch = 0, fb_bpp = 0;
static uint32_t cursor_x = 0, cursor_y = 0;
static uint32_t fg_color = 0xFFFFFF, bg_color = 0x000000;
static uint8_t font_w = 8, font_h = 16;
static uint32_t cols = 0, rows = 0;

/* 软光标状态 */
static bool cursor_visible = true;
static bool cursor_blink_enabled = true;
static uint32_t cursor_blink_ticks = 0;
static uint32_t cursor_blink_rate = 30;  /* ~0.5s at 60Hz timer */
static uint8_t cursor_shape = 1;  /* 0=block, 1=underline */

/* 前向声明：串口回退 */
extern void console_putc(char c);
extern void console_puts(const char *s);

/* 前向声明 */
static void fbcon_draw_glyph(uint32_t x, uint32_t y, uint8_t ch, uint32_t fg, uint32_t bg);
static void fbcon_scroll(void);
static void fbcon_draw_cursor(void);
static void fbcon_erase_cursor(void);
static void fbcon_update_cursor(void);

/* 像素格式处理：假设 32bpp RGB/BGR */
static inline uint32_t pack_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    return (r << 16) | (g << 8) | b;
}

void fbcon_init(uint64_t phys, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp)
{
    /* BIOS 模式下 phys 可能为 0 或 > 1 GiB，直接返回 */
    if (!phys || phys >= 0x40000000 || w == 0 || h == 0) {
        console_puts("[fbcon] skipped (invalid phys or BIOS mode)\n");
        return;
    }
    
    fb_phys = phys;
    fb_w = w; fb_h = h; fb_pitch = pitch; fb_bpp = bpp;
    cols = w / font_w;
    rows = h / font_h;

    /* 1:1 映射：物理地址 = 虚拟地址（内核已建立 1 GiB 恒等映射） */
    fb = (uint32_t *)(uintptr_t)phys;

    fbcon_clear();
}

void fbcon_clear(void)
{
    if (!fb) return;
    uint32_t c = pack_pixel(bg_color >> 16, bg_color >> 8, bg_color);
    for (uint32_t y = 0; y < fb_h; y++) {
        uint32_t *line = (uint32_t *)((uint8_t *)fb + y * fb_pitch);
        for (uint32_t x = 0; x < fb_w; x++) line[x] = c;
    }
    cursor_x = cursor_y = 0;
    fbcon_draw_cursor();  /* 初始化后立即绘制软光标，覆盖固件光标 */
}

static void fbcon_draw_glyph(uint32_t x, uint32_t y, uint8_t ch, uint32_t fg, uint32_t bg)
{
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *glyph = font8x16 + (ch - 32) * 16;
    for (uint32_t row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        uint32_t *line = (uint32_t *)((uint8_t *)fb + (y + row) * fb_pitch) + x;
        for (uint32_t col = 0; col < 8; col++) {
            line[col] = (bits & 0x80) ? fg : bg;
            bits <<= 1;
        }
    }
}

/* 统一输出入口：有 fb 走 fb，无 fb 走串口 */
void fbcon_putc(char c)
{
    if (fb) {
        if (c == '\n') { cursor_x = 0; cursor_y++; }
        else if (c == '\r') { cursor_x = 0; }
        else if (c == '\b') {
            if (cursor_x) cursor_x--;
            else if (cursor_y) { cursor_y--; cursor_x = cols - 1; }
            fbcon_draw_glyph(cursor_x * font_w, cursor_y * font_h, ' ', fg_color, bg_color);
        }
        else if (c >= 32 && c < 127) {
            fbcon_draw_glyph(cursor_x * font_w, cursor_y * font_h, c, fg_color, bg_color);
            cursor_x++;
        }
        if (cursor_x >= cols) { cursor_x = 0; cursor_y++; }
        if (cursor_y >= rows) fbcon_scroll();
    } else {
        console_putc(c);  /* 回退到串口 */
    }
}

void fbcon_puts(const char *s)
{
    while (*s) fbcon_putc(*s++);
}

static void fbcon_scroll(void)
{
    if (!fb) return;
    fbcon_erase_cursor();  /* 滚动前先擦除光标 */
    uint32_t line_bytes = fb_pitch;
    uint8_t *dst = (uint8_t *)fb;
    uint8_t *src = dst + line_bytes * font_h;
    uint32_t move_bytes = line_bytes * (rows - 1) * font_h;
    memcpy(dst, src, move_bytes);
    /* 清最后一行 */
    memset(dst + move_bytes, 0, line_bytes * font_h);
    cursor_y = rows - 1;
    fbcon_draw_cursor();
}

/* ==================== 软光标实现 ==================== */

static void fbcon_draw_cursor(void)
{
    if (!fb || !cursor_visible) return;
    
    uint32_t cx = cursor_x * font_w;
    uint32_t cy = cursor_y * font_h;
    
    if (cursor_shape == 0) {
        /* 块状光标：全字符区域反色 */
        for (uint32_t row = 0; row < font_h; row++) {
            uint32_t *line = (uint32_t *)((uint8_t *)fb + (cy + row) * fb_pitch) + cx;
            for (uint32_t col = 0; col < font_w; col++) {
                line[col] = ~line[col];
            }
        }
    } else {
        /* 下划线光标：只画底部两行 */
        for (uint32_t row = font_h - 2; row < font_h; row++) {
            uint32_t *line = (uint32_t *)((uint8_t *)fb + (cy + row) * fb_pitch) + cx;
            for (uint32_t col = 0; col < font_w; col++) {
                line[col] = fg_color;
            }
        }
    }
}

static void fbcon_erase_cursor(void)
{
    if (!fb || !cursor_visible) return;
    
    uint32_t cx = cursor_x * font_w;
    uint32_t cy = cursor_y * font_h;
    
    if (cursor_shape == 0) {
        /* 块状光标：再次反色恢复 */
        for (uint32_t row = 0; row < font_h; row++) {
            uint32_t *line = (uint32_t *)((uint8_t *)fb + (cy + row) * fb_pitch) + cx;
            for (uint32_t col = 0; col < font_w; col++) {
                line[col] = ~line[col];
            }
        }
    } else {
        /* 下划线光标：恢复背景色 */
        uint32_t bg = pack_pixel(bg_color >> 16, bg_color >> 8, bg_color);
        for (uint32_t row = font_h - 2; row < font_h; row++) {
            uint32_t *line = (uint32_t *)((uint8_t *)fb + (cy + row) * fb_pitch) + cx;
            for (uint32_t col = 0; col < font_w; col++) {
                line[col] = bg;
            }
        }
    }
}

static void fbcon_update_cursor(void)
{
    if (!fb) return;
    fbcon_erase_cursor();
    fbcon_draw_cursor();
}

/* 定时器中断调用：实现光标闪烁 */
void fbcon_timer_tick(void)
{
    if (!cursor_blink_enabled || !fb) return;
    cursor_blink_ticks++;
    if (cursor_blink_ticks >= cursor_blink_rate) {
        cursor_blink_ticks = 0;
        cursor_visible = !cursor_visible;
        fbcon_draw_cursor();
    }
}