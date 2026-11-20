/* SutureOS v0.59.0: present the last committed Wayland wl_surface on screen.
 *
 * The minimal Wayland server lives in kernel/ipc/wl_server.c and exposes the
 * last committed surface as a shared-memory bitmap. This file is the "compositor
 * check": it blits that surface into the linear framebuffer with integer
 * nearest-neighbour scaling, draws a 2 px accent border and waits for ESC (or a
 * bounded timeout) before handing the screen back to the terminal.
 *
 * The server symbol is declared weak so this file builds (and the `wlshow`
 * command honestly reports "no committed surface") even in a tree where the
 * minimal Wayland server has not been integrated yet.
 */
#include <stitch/console.h>

extern uint64_t limine_fb_addr;
extern uint64_t limine_fb_width;
extern uint64_t limine_fb_height;
extern uint64_t limine_fb_pitch;
extern uint64_t limine_fb_bpp;

extern volatile uint64_t g_tick_count;
extern int mbos_kbd_poll(void);
extern void console_screen_enable(int on);
extern int term_available(void);
extern void term_clear(void);
extern void fbcon_clear(void);

/* Implemented by kernel/ipc/wl_server.c (the minimal Wayland server). It used to
 * be a weak stub so this file could build before the server landed; now the real
 * definition is always linked, and a weak duplicate would just confuse the
 * global-symbol scan. */
extern int wl_server_latest_surface(int *w, int *h, int *stride, const uint8_t **px);

int wl_view_show(void) {
    int sw = 0, sh = 0, stride = 0;
    const uint8_t *px = 0;
    /* the server returns 1 when a surface has been committed, 0/-errno otherwise */
    if (wl_server_latest_surface(&sw, &sh, &stride, &px) != 1 || !px ||
        sw <= 0 || sh <= 0 || stride < sw * 4) {
        console_printf("[wl] show: no committed wl_surface (run `run wlclient` first)\n");
        return -1;
    }
    if (!limine_fb_addr || limine_fb_bpp != 32) {
        console_printf("[wl] show: need a 32bpp linear framebuffer\n");
        return -2;
    }
    const int W = (int)limine_fb_width;
    const int H = (int)limine_fb_height;
    const int pitch = (int)(limine_fb_pitch / 4u);
    uint32_t *fb = (uint32_t *)(uintptr_t)limine_fb_addr;

    console_screen_enable(0);
    /* dark backdrop */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            fb[y * pitch + x] = 0x000000;
        }
    }
    /* integer nearest-neighbour scale, centred */
    int scale = 1;
    while (sw * (scale + 1) <= W - 80 && sh * (scale + 1) <= H - 80) {
        scale++;
    }
    const int dw = sw * scale;
    const int dh = sh * scale;
    const int ox = (W - dw) / 2;
    const int oy = (H - dh) / 2;
    for (int y = 0; y < dh; y++) {
        const int sy = y / scale;
        const uint32_t *srow = (const uint32_t *)(const void *)(px + sy * stride);
        uint32_t *drow = fb + (oy + y) * pitch + ox;
        for (int x = 0; x < dw; x++) {
            drow[x] = srow[x / scale] & 0x00ffffffu;
        }
    }
    /* 2 px accent border so the surface edge is unmistakable on a screenshot */
    for (int x = ox; x < ox + dw; x++) {
        fb[(oy - 1) * pitch + x] = 0x4f8dff;
        fb[(oy - 2) * pitch + x] = 0x4f8dff;
        fb[(oy + dh) * pitch + x] = 0x4f8dff;
        fb[(oy + dh + 1) * pitch + x] = 0x4f8dff;
    }
    for (int y = oy; y < oy + dh; y++) {
        fb[y * pitch + ox - 1] = 0x4f8dff;
        fb[y * pitch + ox - 2] = 0x4f8dff;
        fb[y * pitch + ox + dw] = 0x4f8dff;
        fb[y * pitch + ox + dw + 1] = 0x4f8dff;
    }
    console_printf("[wl] show %dx%d scale=%d at %d,%d (stride %d)\n",
                   sw, sh, scale, ox, oy, stride);

    /* ESC (make code 0x01) dismisses; bounded so a headless run cannot hang */
    const uint64_t t0 = g_tick_count;
    for (;;) {
        int sc;
        int esc = 0;
        while ((sc = mbos_kbd_poll()) >= 0) {
            if (sc == 0x01) {
                esc = 1;
            }
        }
        if (esc || (g_tick_count - t0) > 1500u) {
            break;
        }
        for (volatile int spin = 0; spin < 4000; spin++) {
        }
    }
    console_screen_enable(1);
    if (term_available()) term_clear();
    else fbcon_clear();
    console_printf("[wl] show: back to terminal\n");
    return 0;
}

int shell_cmd_wlshow(int argc, char **argv) {
    (void)argc; (void)argv;
    return wl_view_show();
}
