/* SutureOS: shared RFC 1951 (DEFLATE) inflate - implementation
 *
 * Extracted verbatim-in-behaviour from kernel/pkg/deb.c (see inflate.h for
 * why). deb.c now calls inflate_raw() here, and kernel/fs/squashfs.c calls
 * inflate_raw()/inflate_zlib() for COMPRESSOR_GZIP data and metadata blocks.
 *
 * Original license: GPL-3.0 (same as the project).
 * Written from scratch against RFC 1951 / RFC 1950; no third-party code.
 */
#include "inflate.h"
#include <stitch/types.h>
#include <stitch/string.h>

/* ==================== bit reader ==================== */

struct inf_bits {
    const uint8_t *in;
    uint32_t inlen;
    uint32_t inpos;
    uint64_t bitbuf;
    int bitcnt;
};

static int inf_getbyte(struct inf_bits *s) {
    if (s->inpos >= s->inlen) return -1;
    return s->in[s->inpos++];
}

static int inf_bits_need(struct inf_bits *s, int need) {
    while (s->bitcnt < need) {
        int b = inf_getbyte(s);
        if (b < 0) return -1;
        s->bitbuf |= (uint64_t)b << s->bitcnt;
        s->bitcnt += 8;
    }
    int v = (int)(s->bitbuf & ((1u << need) - 1));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}

/* puff-style canonical Huffman decoding */
struct inf_huff {
    int count[16];   /* number of codes of each length */
    int symbol[288]; /* symbols ordered by code */
};

static int inf_huff_construct(struct inf_huff *h, const uint8_t *lengths, int n) {
    int offs[16];
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    if (h->count[0] == n) return 0;   /* no codes at all */

    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;      /* over-subscribed */
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) {
        offs[len + 1] = offs[len] + h->count[len];
    }
    for (int i = 0; i < n; i++) {
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = i;
    }
    return left;   /* >0 means incomplete but usable */
}

static int inf_decode(struct inf_bits *s, const struct inf_huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        int b = inf_bits_need(s, 1);
        if (b < 0) return -1;
        code |= b;
        int cnt = h->count[len];
        if (code - cnt < first) {
            return h->symbol[index + (code - first)];
        }
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* length/distance tables */
static const uint16_t inf_len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,
    163,195,227,258 };
static const uint8_t inf_len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t inf_dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,
    2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t inf_dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

struct inf_out {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
};

static int inf_put(struct inf_out *o, uint8_t b) {
    if (o->len >= o->cap) return -1;
    o->buf[o->len++] = b;
    return 0;
}

static int inf_block(struct inf_bits *s, struct inf_out *o,
                     const struct inf_huff *lencode, const struct inf_huff *distcode) {
    for (;;) {
        int sym = inf_decode(s, lencode);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (inf_put(o, (uint8_t)sym) < 0) return -2;   /* output full */
        } else if (sym == 256) {
            return 0;                                       /* end of block */
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = inf_len_base[sym] + inf_bits_need(s, inf_len_extra[sym]);
            int dsym = inf_decode(s, distcode);
            if (dsym < 0 || dsym >= 30) return -1;
            int dist = inf_dist_base[dsym] + inf_bits_need(s, inf_dist_extra[dsym]);
            if (len < 0 || dist <= 0 || (uint32_t)dist > o->len) return -1;
            uint32_t from = o->len - (uint32_t)dist;
            for (int i = 0; i < len; i++) {
                if (inf_put(o, o->buf[from + i]) < 0) return -2;
            }
        }
    }
}

static int inf_fixed_tables(struct inf_huff *lencode, struct inf_huff *distcode) {
    uint8_t lengths[288];
    for (int i = 0; i < 144; i++) lengths[i] = 8;
    for (int i = 144; i < 256; i++) lengths[i] = 9;
    for (int i = 256; i < 280; i++) lengths[i] = 7;
    for (int i = 280; i < 288; i++) lengths[i] = 8;
    if (inf_huff_construct(lencode, lengths, 288) < 0) return -1;
    for (int i = 0; i < 30; i++) lengths[i] = 5;
    if (inf_huff_construct(distcode, lengths, 30) < 0) return -1;
    return 0;
}

static int inf_dynamic_tables(struct inf_bits *s, struct inf_huff *lencode,
                              struct inf_huff *distcode) {
    static const uint8_t order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    int hlit = inf_bits_need(s, 5) + 257;
    int hdist = inf_bits_need(s, 5) + 1;
    int hclen = inf_bits_need(s, 4) + 4;
    if (hlit > 288 || hdist > 30) return -1;

    uint8_t clen[19];
    for (int i = 0; i < 19; i++) clen[i] = 0;
    for (int i = 0; i < hclen; i++) {
        int v = inf_bits_need(s, 3);
        if (v < 0) return -1;
        clen[order[i]] = (uint8_t)v;
    }
    struct inf_huff cltree;
    if (inf_huff_construct(&cltree, clen, 19) < 0) return -1;

    uint8_t lengths[288 + 30];
    int n = 0;
    while (n < hlit + hdist) {
        int sym = inf_decode(s, &cltree);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (n == 0) return -1;
            int prev = lengths[n - 1];
            int rep = 3 + inf_bits_need(s, 2);
            if (rep < 0) return -1;
            while (rep-- && n < hlit + hdist) lengths[n++] = (uint8_t)prev;
        } else if (sym == 17) {
            int rep = 3 + inf_bits_need(s, 3);
            if (rep < 0) return -1;
            while (rep-- && n < hlit + hdist) lengths[n++] = 0;
        } else if (sym == 18) {
            int rep = 11 + inf_bits_need(s, 7);
            if (rep < 0) return -1;
            while (rep-- && n < hlit + hdist) lengths[n++] = 0;
        } else {
            return -1;
        }
    }
    if (inf_huff_construct(lencode, lengths, hlit) < 0) return -1;
    if (inf_huff_construct(distcode, lengths + hlit, hdist) < 0) return -1;
    return 0;
}

/* Inflate a raw deflate stream. Returns total output length or negative. */
int inflate_raw(const uint8_t *src, uint32_t srclen, uint8_t *dst, uint32_t dstcap) {
    struct inf_bits s = { src, srclen, 0, 0, 0 };
    struct inf_out o = { dst, dstcap, 0 };
    int last;
    if (!src || !dst) return INFLATE_ERR_INVAL;
    do {
        last = inf_bits_need(&s, 1);
        int type = inf_bits_need(&s, 2);
        if (last < 0 || type < 0) return INFLATE_ERR_INVAL;
        if (type == 0) {
            /* stored: skip to byte boundary, copy LEN bytes */
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.inpos + 4 > s.inlen) return INFLATE_ERR_INVAL;
            uint32_t len = (uint32_t)s.in[s.inpos] | ((uint32_t)s.in[s.inpos + 1] << 8);
            s.inpos += 4;
            if (s.inpos + len > s.inlen) return INFLATE_ERR_INVAL;
            if (o.len + len > o.cap) return INFLATE_ERR_OUTPUT_FULL;
            memcpy(o.buf + o.len, s.in + s.inpos, len);
            o.len += len;
            s.inpos += len;
        } else {
            struct inf_huff lencode, distcode;
            int rc;
            if (type == 1) rc = inf_fixed_tables(&lencode, &distcode);
            else if (type == 2) rc = inf_dynamic_tables(&s, &lencode, &distcode);
            else return INFLATE_ERR_INVAL;
            if (rc < 0) return INFLATE_ERR_INVAL;
            rc = inf_block(&s, &o, &lencode, &distcode);
            if (rc == -2) return INFLATE_ERR_OUTPUT_FULL;
            if (rc < 0) return INFLATE_ERR_INVAL;
        }
    } while (!last);
    return (int)o.len;
}

/* ==================== zlib wrapper (RFC 1950) ==================== */

static uint32_t inf_adler32(const uint8_t *p, uint32_t len) {
    uint32_t a = 1, b = 0;
    for (uint32_t i = 0; i < len; i++) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

int inflate_zlib(const uint8_t *src, uint32_t srclen, uint8_t *dst, uint32_t dstcap) {
    uint32_t cmf, flg;
    int n;

    if (!src || srclen < 6) return INFLATE_ERR_INVAL;
    cmf = src[0];
    flg = src[1];
    if ((cmf & 0x0Fu) != 8) return INFLATE_ERR_INVAL;       /* CM must be deflate */
    if ((cmf >> 4) > 7) return INFLATE_ERR_INVAL;           /* window > 32 KiB */
    if (((cmf << 8) | flg) % 31u != 0) return INFLATE_ERR_INVAL;
    if (flg & 0x20) return INFLATE_ERR_INVAL;               /* FDICT unsupported */

    n = inflate_raw(src + 2, srclen - 6, dst, dstcap);
    if (n < 0) return n;

    /* verify the Adler-32 trailer */
    {
        uint32_t want = ((uint32_t)src[srclen - 4] << 24) |
                        ((uint32_t)src[srclen - 3] << 16) |
                        ((uint32_t)src[srclen - 2] << 8) |
                        (uint32_t)src[srclen - 1];
        if (inf_adler32(dst, (uint32_t)n) != want) return INFLATE_ERR_INVAL;
    }
    return n;
}
