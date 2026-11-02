/* SutureOS: QOI ("Quite OK Image") decoder for the GUI image viewer.
 *
 * Algorithm reference: PlantOS src/plic/qoi.c (MIT). Rewritten for SutureOS:
 * caller-provided output buffer + explicit pixel capacity, and every opcode is
 * bounds-checked so a truncated file cannot read past its own bytes.
 *
 * The QOI spec (qoiformat.org) is small enough to follow directly:
 *   header: 'q','o','i','f', width(u32 BE), height(u32 BE), channels, colorspace
 *   ops   : RGB (0xFE + 3), RGBA (0xFF + 4), INDEX (0x00..0x3F),
 *           DIFF (0x40..0x7F), LUMA (0x80..0xBF), RUN (0xC0..0xFD)
 */
#include "imgdecode.h"

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int img_decode_qoi(const uint8_t *data, uint32_t len, img_decoded_t *out,
                   uint32_t max_px) {
    if (!data || !out || len < 22) return 0;
    if (data[0] != 'q' || data[1] != 'o' || data[2] != 'i' || data[3] != 'f')
        return 0;

    const uint32_t w = be32(data + 4);
    const uint32_t h = be32(data + 8);
    if (w == 0 || h == 0 || w > 0x4000u || h > 0x4000u) return 0;
    if ((uint64_t)w * h > (uint64_t)max_px) return 0;

    uint32_t index[64];
    for (int i = 0; i < 64; i++) index[i] = 0u;      /* all zero, alpha 0 */
    uint32_t cur = 0xFF000000u;                       /* (0,0,0,255)       */
    uint32_t pos = 14;
    uint64_t npix = (uint64_t)w * h;
    uint64_t n = 0;
    uint32_t run = 0;

    while (n < npix) {
        if (run > 0) {
            run--;
        } else {
            if (pos >= len) return 0;
            const uint8_t b1 = data[pos++];
            if (b1 == 0xFEu) {                        /* RGB  */
                if (pos + 3 > len) return 0;
                cur = (cur & 0xFF000000u) |
                      ((uint32_t)data[pos] << 16) |
                      ((uint32_t)data[pos + 1] << 8) | (uint32_t)data[pos + 2];
                pos += 3;
            } else if (b1 == 0xFFu) {                 /* RGBA */
                if (pos + 4 > len) return 0;
                cur = ((uint32_t)data[pos] << 24) |
                      ((uint32_t)data[pos + 1] << 16) |
                      ((uint32_t)data[pos + 2] << 8) | (uint32_t)data[pos + 3];
                pos += 4;
            } else if (b1 < 0x40u) {                  /* INDEX */
                cur = index[b1];
            } else if (b1 < 0x80u) {                  /* DIFF  */
                const int dr = (int)((b1 >> 4) & 0x03u) - 2;
                const int dg = (int)((b1 >> 2) & 0x03u) - 2;
                const int db = (int)(b1 & 0x03u) - 2;
                int r = (int)((cur >> 16) & 0xFFu) + dr;
                int g = (int)((cur >> 8) & 0xFFu) + dg;
                int b = (int)(cur & 0xFFu) + db;
                cur = (cur & 0xFF000000u) |
                      ((uint32_t)(r & 0xFF) << 16) |
                      ((uint32_t)(g & 0xFF) << 8) | (uint32_t)(b & 0xFF);
            } else if (b1 < 0xC0u) {                  /* LUMA  */
                if (pos >= len) return 0;
                const uint8_t b2 = data[pos++];
                const int dg = (int)(b1 & 0x3Fu) - 32;
                const int dr_dg = (int)((b2 >> 4) & 0x0Fu) - 8;
                const int db_dg = (int)(b2 & 0x0Fu) - 8;
                int r = (int)((cur >> 16) & 0xFFu) + dg + dr_dg;
                int g = (int)((cur >> 8) & 0xFFu) + dg;
                int b = (int)(cur & 0xFFu) + dg + db_dg;
                cur = (cur & 0xFF000000u) |
                      ((uint32_t)(r & 0xFF) << 16) |
                      ((uint32_t)(g & 0xFF) << 8) | (uint32_t)(b & 0xFF);
            } else {                                  /* RUN   */
                run = (uint32_t)(b1 & 0x3Fu);
            }
            index[(cur * 0x9E3779B1u >> 26) & 0x3Fu] = cur;
        }
        /* QOI is big-endian RGBA; the GUI wants 0x00RRGGBB. Alpha is dropped
         * because the compositor has no alpha blending. */
        if (n < npix) {
            out->px[n] = cur & 0x00FFFFFFu;
            n++;
        }
    }

    out->w = (int)w;
    out->h = (int)h;
    out->fmt = IMG_FMT_QOI;
    out->bpp = 32;
    return 1;
}
