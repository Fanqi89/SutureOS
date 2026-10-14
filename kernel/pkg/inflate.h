/* SutureOS: shared RFC 1951 (DEFLATE) inflate
 *
 * Extracted from kernel/pkg/deb.c (which originally carried the only copy of
 * this decompressor in the tree) so that the SquashFS driver can decode gzip
 * compressed data/metadata blocks WITHOUT a second hand-written inflate.
 * The algorithm itself is unchanged: puff-style canonical Huffman decoding,
 * stored + fixed + dynamic blocks, no dictionary support (neither .deb
 * members nor SquashFS gzip blocks ever use one).
 *
 * Original license: GPL-3.0 (same as the project); the deb.c version was
 * written from scratch against the public RFC 1951 specification.
 */
#ifndef STITCH_INFLATE_H
#define STITCH_INFLATE_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Inflate a RAW deflate stream (RFC 1951, no zlib/gzip wrapper).
 * Returns the number of output bytes, INFLATE_ERR_INVAL or
 * INFLATE_ERR_OUTPUT_FULL on failure. */
int inflate_raw(const uint8_t *src, uint32_t srclen, uint8_t *dst, uint32_t dstcap);

/* Inflate a zlib stream (RFC 1950): 2-byte header, raw deflate, 4-byte
 * Adler-32 trailer. The Adler-32 is verified when it is present; a stream that
 * is too short to carry a trailer is rejected.
 * Returns the number of output bytes or a negative INFLATE_ERR_*. */
int inflate_zlib(const uint8_t *src, uint32_t srclen, uint8_t *dst, uint32_t dstcap);

#define INFLATE_OK              0
#define INFLATE_ERR_INVAL      (-1)   /* malformed stream */
#define INFLATE_ERR_OUTPUT_FULL (-2)  /* dstcap too small */

#ifdef __cplusplus
}
#endif

#endif /* STITCH_INFLATE_H */
