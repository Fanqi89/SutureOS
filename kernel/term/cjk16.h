/* SutureOS: 16x16 CJK bitmap font (GB2312 subset).
 *
 * The glyph blob is generated at build time by scripts/mkfont_cjk.py from the
 * SIL OFL-1.1 font `ZhengGeDianHei-16.ttf` (see docs/licenses/OFL-1.1.txt and
 * docs/provenance.md) and embedded with objcopy.
 *
 * Blob format: 'SFCJ', u32 count, then count x {u32 cp; u8 bitmap[32]} sorted by
 * codepoint. Bitmap rows are 16 bits, most-significant bit = leftmost pixel.
 */
#ifndef SUTUREOS_CJK16_H
#define SUTUREOS_CJK16_H

#include <stitch/types.h>

int cjk16_init(void);
int cjk16_count(void);
const uint8_t *cjk16_lookup(uint32_t codepoint);
int cjk16_is_wide(uint32_t codepoint);

/* Blit one glyph into a 32bpp framebuffer at (px,py). Returns 1 when drawn,
 * 0 when the codepoint is not in the subset or the target is out of bounds. */
int cjk16_draw(uint32_t *fb, uint32_t pitch_px, uint32_t fb_w, uint32_t fb_h,
               int px, int py, uint32_t codepoint, uint32_t fg, uint32_t bg);

#endif /* SUTUREOS_CJK16_H */
