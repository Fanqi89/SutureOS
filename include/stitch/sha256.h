/* SutureOS: SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256.
 *
 * Why this exists: `sudo` needs to verify a user's password, and the tree had no
 * hash at all (only CRC-32 for the filesystem images). Linux's PAM stack uses
 * crypt(3)/yescrypt; that is not available here (no libc, no userland), so this
 * file provides the closest honest equivalent: salted PBKDF2-HMAC-SHA256 with a
 * configurable iteration count, stored in a shadow-style record.
 *
 * Limitations (stated on purpose, not hidden):
 *   - SHA-256 is implemented from FIPS 180-4, no hardware acceleration;
 *   - PBKDF2 iteration count is low by modern standards (see USERDB_ITERATIONS)
 *     because the kernel has no way to run it in the background;
 *   - there is no secure RNG yet (salts are mixed from tick/rdtsc/name).
 */
#ifndef STITCH_SHA256_H
#define STITCH_SHA256_H

#include <stitch/types.h>

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE  64

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buffer[SHA256_BLOCK_SIZE];
    uint32_t buflen;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

/* One-shot convenience wrappers. */
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);
void hmac_sha256(const void *key, size_t keylen,
                 const void *msg, size_t msglen,
                 uint8_t out[SHA256_DIGEST_SIZE]);

/* PBKDF2-HMAC-SHA256 (RFC 2898). outlen <= SHA256_DIGEST_SIZE for our use. */
int pbkdf2_sha256(const char *pass, size_t passlen,
                  const void *salt, size_t saltlen,
                  uint32_t iterations,
                  uint8_t *out, size_t outlen);

/* Hex helpers (lowercase, NUL-terminated). */
void sha256_hex(const uint8_t digest[SHA256_DIGEST_SIZE], char out[65]);
int  hex_to_bytes(const char *hex, uint8_t *out, size_t outlen);

/* Known-answer self test (FIPS vectors). Returns 1 on success. */
int sha256_selftest(void);

#endif /* STITCH_SHA256_H */
