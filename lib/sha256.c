/* SutureOS: SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256 implementation.
 *
 * Straight FIPS 180-4 SHA-256 (no tables beyond K, no allocation, no libc), plus
 * the two constructions sudo needs. Every routine is self-tested against known
 * answers in sha256_selftest() so a broken implementation cannot silently let
 * every password through.
 */
#include <stitch/sha256.h>
#include <stitch/string.h>

/* ---- FIPS 180-4 constants ---- */
static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)   (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x)      (ROTR32(x, 2) ^ ROTR32(x, 13) ^ ROTR32(x, 22))
#define BSIG1(x)      (ROTR32(x, 6) ^ ROTR32(x, 11) ^ ROTR32(x, 25))
#define SSIG0(x)      (ROTR32(x, 7) ^ ROTR32(x, 18) ^ ((x) >> 3))
#define SSIG1(x)      (ROTR32(x, 17) ^ ROTR32(x, 19) ^ ((x) >> 10))

static void sha256_compress(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + BSIG1(e) + CH(e, f, g) + K[i] + w[i];
        t2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0;
    ctx->buflen = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    ctx->bitlen += (uint64_t)len * 8u;
    while (len > 0) {
        const uint32_t space = SHA256_BLOCK_SIZE - ctx->buflen;
        const uint32_t take = (len < space) ? (uint32_t)len : space;
        memcpy(ctx->buffer + ctx->buflen, p, take);
        ctx->buflen += take;
        p += take;
        len -= take;
        if (ctx->buflen == SHA256_BLOCK_SIZE) {
            sha256_compress(ctx->state, ctx->buffer);
            ctx->buflen = 0;
        }
    }
}

void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE])
{
    const uint64_t bitlen = ctx->bitlen;
    int i;

    /* 0x80 terminator, then zero padding to 56 mod 64, then the length. */
    ctx->buffer[ctx->buflen++] = 0x80;
    if (ctx->buflen > 56) {
        while (ctx->buflen < SHA256_BLOCK_SIZE) ctx->buffer[ctx->buflen++] = 0;
        sha256_compress(ctx->state, ctx->buffer);
        ctx->buflen = 0;
    }
    while (ctx->buflen < 56) ctx->buffer[ctx->buflen++] = 0;
    for (i = 0; i < 8; i++) {
        ctx->buffer[56 + i] = (uint8_t)(bitlen >> (56 - 8 * i));
    }
    sha256_compress(ctx->state, ctx->buffer);

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
    memset(ctx, 0, sizeof(*ctx));
}

void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

void hmac_sha256(const void *key, size_t keylen,
                 const void *msg, size_t msglen,
                 uint8_t out[SHA256_DIGEST_SIZE])
{
    uint8_t k[SHA256_BLOCK_SIZE];
    uint8_t ipad[SHA256_BLOCK_SIZE];
    uint8_t opad[SHA256_BLOCK_SIZE];
    uint8_t inner[SHA256_DIGEST_SIZE];
    sha256_ctx_t ctx;
    uint8_t *tmp;
    size_t i;

    memset(k, 0, sizeof(k));
    if (keylen > SHA256_BLOCK_SIZE) {
        sha256(key, keylen, k);            /* long key: hash it first */
    } else if (keylen > 0) {
        memcpy(k, key, keylen);
    }
    for (i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36u);
        opad[i] = (uint8_t)(k[i] ^ 0x5cu);
    }

    sha256_init(&ctx);
    sha256_update(&ctx, ipad, sizeof(ipad));
    sha256_update(&ctx, msg, msglen);
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, sizeof(opad));
    sha256_update(&ctx, inner, sizeof(inner));
    sha256_final(&ctx, out);

    tmp = k;
    memset(tmp, 0, SHA256_BLOCK_SIZE);
    memset(ipad, 0, sizeof(ipad));
    memset(opad, 0, sizeof(opad));
    memset(inner, 0, sizeof(inner));
}

int pbkdf2_sha256(const char *pass, size_t passlen,
                  const void *salt, size_t saltlen,
                  uint32_t iterations,
                  uint8_t *out, size_t outlen)
{
    uint8_t u[SHA256_DIGEST_SIZE];
    uint8_t t[SHA256_DIGEST_SIZE];
    uint8_t block[4 + 256];              /* salt || INT(i) ; salt <= 256 here */
    uint32_t iter;
    size_t k;

    if (out == NULL || outlen == 0 || outlen > SHA256_DIGEST_SIZE) return -1;
    if (iterations == 0) iterations = 1;
    if (saltlen > 256) return -1;

    memcpy(block, salt, saltlen);
    block[saltlen]     = 0;
    block[saltlen + 1] = 0;
    block[saltlen + 2] = 0;
    block[saltlen + 3] = 1;              /* block index = 1 (outlen <= 32) */

    /* U1 = HMAC(pass, salt || INT(1)) */
    hmac_sha256(pass, passlen, block, saltlen + 4, u);
    memcpy(t, u, sizeof(t));

    for (iter = 1; iter < iterations; iter++) {
        hmac_sha256(pass, passlen, u, sizeof(u), u);
        for (k = 0; k < sizeof(t); k++) t[k] ^= u[k];
    }
    memcpy(out, t, outlen);

    memset(u, 0, sizeof(u));
    memset(t, 0, sizeof(t));
    memset(block, 0, sizeof(block));
    return 0;
}

void sha256_hex(const uint8_t digest[SHA256_DIGEST_SIZE], char out[65])
{
    static const char hexd[] = "0123456789abcdef";
    int i;
    for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
        out[i * 2]     = hexd[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = hexd[digest[i] & 0xF];
    }
    out[64] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int hex_to_bytes(const char *hex, uint8_t *out, size_t outlen)
{
    size_t i;
    for (i = 0; i < outlen; i++) {
        const int hi = hexval(hex[i * 2]);
        const int lo = hexval(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* ---- known-answer tests ---- */
static int hexeq(const uint8_t *d, const char *want)
{
    char got[65];
    sha256_hex(d, got);
    return strcmp(got, want) == 0;
}

int sha256_selftest(void)
{
    uint8_t d[SHA256_DIGEST_SIZE];
    uint8_t out[32];

    /* FIPS 180-4 examples */
    sha256("abc", 3, d);
    if (!hexeq(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")) return 0;

    sha256("", 0, d);
    if (!hexeq(d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")) return 0;

    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    if (!hexeq(d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")) return 0;

    /* HMAC-SHA256 RFC 4231 case 1: key=0x0b*20, data="Hi There" */
    {
        static const uint8_t key[20] = {
            0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
            0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b
        };
        hmac_sha256(key, sizeof(key), "Hi There", 8, d);
        if (!hexeq(d, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7")) return 0;
    }

    /* PBKDF2-HMAC-SHA256 RFC 7914 style vector: P="password", S="salt", c=1, dkLen=32 */
    if (pbkdf2_sha256("password", 8, "salt", 4, 1, out, sizeof(out)) != 0) return 0;
    if (!hexeq(out, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b")) return 0;

    /* and c=2 so the iteration loop is exercised too */
    if (pbkdf2_sha256("password", 8, "salt", 4, 2, out, sizeof(out)) != 0) return 0;
    if (!hexeq(out, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43")) return 0;

    return 1;
}
