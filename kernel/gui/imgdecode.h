/* SutureOS module: image decoder interface (GUI apps).
 *
 * Two decoders are wired in, both taken as algorithm references from the
 * project collection (see docs/licensing-audit.md):
 *   - BMP  : HBOS src/bmp.c (GPL-3.0) - BITMAPINFOHEADER, 8/24/32 bpp, BI_RGB
 *   - QOI  : PlantOS src/plic/qoi.c (MIT) - "Quite OK Image" spec
 *
 * The decoded pixels are 0x00RRGGBB words, top-down, tightly packed - exactly
 * what gui_fill_rect() wants, so the viewer can scale with nearest neighbour
 * without another conversion table.
 */
#ifndef SUTUREOS_IMGDECODE_H
#define SUTUREOS_IMGDECODE_H

#include <stitch/types.h>

#define IMG_FMT_NONE 0
#define IMG_FMT_BMP  1
#define IMG_FMT_QOI  2

typedef struct {
    int       w;
    int       h;
    uint32_t *px;          /* w*h words, caller-owned buffer            */
    int       fmt;         /* IMG_FMT_*                                 */
    int       bpp;         /* source bits per pixel (diagnostics)       */
} img_decoded_t;

/* Decode `len` bytes at `data` into `out->px` (capacity `max_px` pixels).
 * Returns 1 on success (out->w/h/fmt/bpp filled), 0 on any unsupported or
 * malformed input. Never writes past max_px pixels. */
int img_decode_bmp(const uint8_t *data, uint32_t len, img_decoded_t *out,
                   uint32_t max_px);
int img_decode_qoi(const uint8_t *data, uint32_t len, img_decoded_t *out,
                   uint32_t max_px);

const char *img_fmt_name(int fmt);   /* "bmp" / "qoi" / "none" */

#endif /* SUTUREOS_IMGDECODE_H */
