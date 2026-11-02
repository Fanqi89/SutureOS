/* SutureOS: 16x16 CJK bitmap font lookup + blit (see cjk16.h). */
#include <stitch/types.h>
#include <stitch/string.h>

#include "cjk16.h"

extern const uint8_t font_cjk_bin_start[];
extern const uint8_t font_cjk_bin_end[];

#define CJK_HDR   8u
#define CJK_ENTRY 36u                    /* u32 codepoint + 32 bitmap bytes */

static const uint8_t *g_blob = NULL;
static uint32_t g_count = 0;

int cjk16_init(void) {
    const uint8_t *b = font_cjk_bin_start;
    const uintptr_t total = (uintptr_t)(font_cjk_bin_end - font_cjk_bin_start);
    if (total < CJK_HDR) return -1;
    if (b[0] != 'S' || b[1] != 'F' || b[2] != 'C' || b[3] != 'J') return -1;
    const uint32_t count = (uint32_t)b[4] | ((uint32_t)b[5] << 8) |
                           ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    if (count == 0) return -1;
    if ((uintptr_t)CJK_HDR + (uintptr_t)count * CJK_ENTRY > total) return -1;
    g_blob = b + CJK_HDR;
    g_count = count;
    return 0;
}

int cjk16_count(void) { return (int)g_count; }

static uint32_t entry_cp(uint32_t i) {
    const uint8_t *e = g_blob + (uintptr_t)i * CJK_ENTRY;
    return (uint32_t)e[0] | ((uint32_t)e[1] << 8) |
           ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
}

const uint8_t *cjk16_lookup(uint32_t codepoint) {
    if (!g_blob || g_count == 0) return NULL;
    uint32_t lo = 0, hi = g_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        const uint32_t cp = entry_cp(mid);
        if (cp == codepoint)
            return g_blob + (uintptr_t)mid * CJK_ENTRY + 4u;
        if (cp < codepoint) lo = mid + 1u;
        else                hi = mid;
    }
    return NULL;
}

int cjk16_is_wide(uint32_t cp) {
    if (cp < 0x1100u) return 0;
    if (cp <= 0x115Fu) return 1;
    if (cp >= 0x2E80u && cp <= 0x303Eu) return 1;
    if (cp >= 0x3041u && cp <= 0x33FFu) return 1;
    if (cp >= 0x3400u && cp <= 0x4DBFu) return 1;
    if (cp >= 0x4E00u && cp <= 0x9FFFu) return 1;
    if (cp >= 0xA000u && cp <= 0xA4CFu) return 1;
    if (cp >= 0xAC00u && cp <= 0xD7A3u) return 1;
    if (cp >= 0xF900u && cp <= 0xFAFFu) return 1;
    if (cp >= 0xFE30u && cp <= 0xFE4Fu) return 1;
    if (cp >= 0xFF00u && cp <= 0xFF60u) return 1;
    if (cp >= 0xFFE0u && cp <= 0xFFE6u) return 1;
    return 0;
}

int cjk16_draw(uint32_t *fb, uint32_t pitch_px, uint32_t fb_w, uint32_t fb_h,
               int px, int py, uint32_t codepoint, uint32_t fg, uint32_t bg) {
    if (!fb || pitch_px == 0) return 0;
    const uint8_t *g = cjk16_lookup(codepoint);
    if (!g) return 0;
    if (px < -16 || py < -16) return 0;
    for (int row = 0; row < 16; row++) {
        const int y = py + row;
        if (y < 0 || (uint32_t)y >= fb_h) continue;
        const uint32_t bits = ((uint32_t)g[row * 2] << 8) | (uint32_t)g[row * 2 + 1];
        uint32_t *line = fb + (uint32_t)y * pitch_px;
        for (int col = 0; col < 16; col++) {
            const int x = px + col;
            if (x < 0 || (uint32_t)x >= fb_w) continue;
            line[x] = (bits & (0x8000u >> col)) ? fg : bg;
        }
    }
    return 1;
}
