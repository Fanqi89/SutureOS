/* SutureOS: BMP decoder for the GUI image viewer.
 *
 * Algorithm reference: HBOS src/bmp.c (GPL-3.0, same license as this project).
 * Rewritten for SutureOS: no malloc, no stdio - the caller passes the output
 * buffer and its pixel capacity, so a malformed/crafted header can never make
 * the decoder walk off the end of the image or of the heap.
 *
 * Supported: "BM" file header, BITMAPINFOHEADER (>= 40 bytes) or V4/V5 (the
 * fields we need share the same offsets), BI_RGB (uncompressed), 1/4/8 bpp
 * palette, 16/24/32 bpp direct colour, top-down (negative height) and
 * bottom-up rows. Everything else is rejected with 0 (the app then shows a
 * built-in pattern and says so on screen).
 */
#include "imgdecode.h"

static uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *img_fmt_name(int fmt) {
    switch (fmt) {
    case IMG_FMT_BMP: return "bmp";
    case IMG_FMT_QOI: return "qoi";
    default:          return "none";
    }
}

int img_decode_bmp(const uint8_t *data, uint32_t len, img_decoded_t *out,
                   uint32_t max_px) {
    if (!data || !out || len < 54) return 0;
    if (data[0] != 'B' || data[1] != 'M') return 0;

    const uint32_t pix_off = rd32(data + 10);
    const uint32_t hdr_size = rd32(data + 14);
    if (hdr_size < 40 || 14u + hdr_size > len) return 0;

    const int32_t  w_signed = (int32_t)rd32(data + 18);
    const int32_t  h_signed = (int32_t)rd32(data + 22);
    const uint32_t bpp = rd16(data + 28);
    const uint32_t compression = rd32(data + 30);
    if (compression != 0) return 0;                 /* BI_RGB only          */
    if (w_signed <= 0 || h_signed == 0) return 0;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
        return 0;

    const int top_down = (h_signed < 0) ? 1 : 0;
    const uint32_t w = (uint32_t)w_signed;
    const uint32_t h = (uint32_t)(top_down ? -h_signed : h_signed);
    if (w == 0 || h == 0) return 0;
    if ((uint64_t)w * h > (uint64_t)max_px) return 0;

    const uint32_t stride = ((w * bpp + 31u) / 32u) * 4u;
    if (pix_off >= len) return 0;
    if ((uint64_t)pix_off + (uint64_t)stride * h > (uint64_t)len) return 0;

    /* Palette (1/4/8 bpp) lives right after the DIB header, 4 bytes per entry. */
    const uint8_t *palette = data + 14u + hdr_size;
    uint32_t pal_entries = 0;
    if (bpp <= 8) {
        pal_entries = (bpp == 1) ? 2u : (bpp == 4 ? 16u : 256u);
        if (14u + hdr_size + pal_entries * 4u > len) return 0;
    }

    for (uint32_t y = 0; y < h; y++) {
        const uint32_t src_row = top_down ? y : (h - 1u - y);
        const uint8_t *row = data + pix_off + (uint64_t)src_row * stride;
        uint32_t *dst = out->px + (uint64_t)y * w;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t r, g, b;
            if (bpp == 24) {
                const uint8_t *p = row + x * 3u;
                b = p[0]; g = p[1]; r = p[2];
            } else if (bpp == 32) {
                const uint8_t *p = row + x * 4u;
                b = p[0]; g = p[1]; r = p[2];
            } else if (bpp == 16) {
                /* 5-5-5, the DIB default when no bitfields mask is present */
                const uint32_t v = rd16(row + x * 2u);
                r = ((v >> 10) & 0x1Fu) * 255u / 31u;
                g = ((v >> 5) & 0x1Fu) * 255u / 31u;
                b = (v & 0x1Fu) * 255u / 31u;
            } else if (bpp == 8) {
                const uint8_t idx = row[x];
                const uint8_t *e = palette + (uint32_t)idx * 4u;
                if (idx >= pal_entries) { b = g = r = 0; }
                else { b = e[0]; g = e[1]; r = e[2]; }
            } else if (bpp == 4) {
                const uint8_t byte = row[x / 2u];
                const uint32_t idx = (x & 1u) ? (byte & 0x0Fu) : (byte >> 4);
                const uint8_t *e = palette + idx * 4u;
                b = e[0]; g = e[1]; r = e[2];
            } else {                                  /* 1 bpp */
                const uint8_t byte = row[x / 8u];
                const uint32_t idx = (byte >> (7u - (x & 7u))) & 1u;
                const uint8_t *e = palette + idx * 4u;
                b = e[0]; g = e[1]; r = e[2];
            }
            dst[x] = ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu);
        }
    }

    out->w = (int)w;
    out->h = (int)h;
    out->fmt = IMG_FMT_BMP;
    out->bpp = (int)bpp;
    return 1;
}
