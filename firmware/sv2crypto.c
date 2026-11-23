/*
 * sv2crypto.c - see sv2crypto.h.
 *
 * Written for clarity and small size on an 8-bit chip, not speed: these run
 * a handful of times per connection and once per network message, never in
 * the mining loop.
 */

#include "sv2crypto.h"

#include <string.h>

/* ======================================================================== */
/* SHA-256                                                                  */
/* ======================================================================== */

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t rotr32(uint32_t x, uint8_t n)
{
    return (x >> n) | (x << (32 - n));
}

static uint32_t rotl32(uint32_t x, uint8_t n)
{
    return (x << n) | (x >> (32 - n));
}

static void sha256_compress(sha256_ctx *ctx)
{
    uint32_t w[16];             /* a rolling window of the message schedule */
    uint32_t s[8];

    for (uint8_t i = 0; i < 16; i++) {
        const uint8_t *p = ctx->block + 4 * i;
        w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
               (uint32_t)p[2] << 8 | p[3];
    }
    memcpy(s, ctx->h, sizeof s);

    for (uint8_t i = 0; i < 64; i++) {
        if (i >= 16) {
            uint32_t w15 = w[(i + 1) & 15], w2 = w[(i + 14) & 15];
            uint32_t s0 = rotr32(w15, 7) ^ rotr32(w15, 18) ^ (w15 >> 3);
            uint32_t s1 = rotr32(w2, 17) ^ rotr32(w2, 19) ^ (w2 >> 10);
            w[i & 15] += s0 + w[(i + 9) & 15] + s1;
        }
        uint32_t a = s[0], e = s[4];
        uint32_t t1 = s[7] + (rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25)) +
                      ((e & s[5]) ^ (~e & s[6])) + sha256_k[i] + w[i & 15];
        uint32_t t2 = (rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22)) +
                      ((a & s[1]) ^ (a & s[2]) ^ (s[1] & s[2]));
        s[7] = s[6]; s[6] = s[5]; s[5] = s[4]; s[4] = s[3] + t1;
        s[3] = s[2]; s[2] = s[1]; s[1] = s[0]; s[0] = t1 + t2;
    }
    for (uint8_t i = 0; i < 8; i++)
        ctx->h[i] += s[i];
}

void sha256_init(sha256_ctx *ctx)
{
    static const uint32_t start[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(ctx->h, start, sizeof start);
    ctx->used = 0;
    ctx->total = 0;
}

void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len)
{
    ctx->total += (uint32_t)len;
    while (len--) {
        ctx->block[ctx->used++] = *data++;
        if (ctx->used == 64) {
            sha256_compress(ctx);
            ctx->used = 0;
        }
    }
}

void sha256_final(sha256_ctx *ctx, uint8_t out[SHA256_BYTES])
{
    uint32_t bits = ctx->total << 3;        /* inputs are far below 512 MB */

    /* Padding: a 1 bit, zeros, then the length in bits as 8 bytes. */
    ctx->block[ctx->used++] = 0x80;
    if (ctx->used > 56) {
        memset(ctx->block + ctx->used, 0, 64 - ctx->used);
        sha256_compress(ctx);
        ctx->used = 0;
    }
    memset(ctx->block + ctx->used, 0, 60 - ctx->used);
    ctx->block[60] = (uint8_t)(bits >> 24);
    ctx->block[61] = (uint8_t)(bits >> 16);
    ctx->block[62] = (uint8_t)(bits >> 8);
    ctx->block[63] = (uint8_t)bits;
    sha256_compress(ctx);

    for (uint8_t i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)(ctx->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)ctx->h[i];
    }
}

/* ======================================================================== */
/* HMAC-SHA256 and Noise's HKDF                                             */
/* ======================================================================== */

/* HMAC over two pieces of data (the second may be empty), which is exactly
 * what HKDF needs. */
static void hmac2(const uint8_t *key, size_t key_len,
                  const uint8_t *d1, size_t d1_len,
                  const uint8_t *d2, size_t d2_len,
                  uint8_t out[SHA256_BYTES])
{
    sha256_ctx ctx;
    uint8_t pad[64];
    uint8_t inner[SHA256_BYTES];

    memset(pad, 0x36, sizeof pad);
    for (size_t i = 0; i < key_len; i++)
        pad[i] ^= key[i];
    sha256_init(&ctx);
    sha256_update(&ctx, pad, sizeof pad);
    sha256_update(&ctx, d1, d1_len);
    sha256_update(&ctx, d2, d2_len);
    sha256_final(&ctx, inner);

    for (uint8_t i = 0; i < sizeof pad; i++)
        pad[i] ^= 0x36 ^ 0x5c;
    sha256_init(&ctx);
    sha256_update(&ctx, pad, sizeof pad);
    sha256_update(&ctx, inner, sizeof inner);
    sha256_final(&ctx, out);
}

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t out[SHA256_BYTES])
{
    hmac2(key, key_len, data, data_len, NULL, 0, out);
}

void noise_hkdf2(const uint8_t chaining_key[SHA256_BYTES],
                 const uint8_t *material, size_t material_len,
                 uint8_t out1[SHA256_BYTES], uint8_t out2[SHA256_BYTES])
{
    uint8_t prk[SHA256_BYTES];
    static const uint8_t one = 1, two = 2;

    hmac_sha256(chaining_key, SHA256_BYTES, material, material_len, prk);
    hmac2(prk, sizeof prk, &one, 1, NULL, 0, out1);
    hmac2(prk, sizeof prk, out1, SHA256_BYTES, &two, 1, out2);
}

/* ======================================================================== */
/* ChaCha20                                                                 */
/* ======================================================================== */

/* One quarter round on x[a], x[b], x[c], x[d]. A function, not a macro
 * written out 8 times: on the chip that saves ~2.5 KB of flash for a little
 * speed (encryption is a few milliseconds per message either way). */
#ifdef __GNUC__
__attribute__((noinline))
#endif
static void quarter(uint32_t *x, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    x[a] += x[b]; x[d] ^= x[a]; x[d] = rotl32(x[d], 16);
    x[c] += x[d]; x[b] ^= x[c]; x[b] = rotl32(x[b], 12);
    x[a] += x[b]; x[d] ^= x[a]; x[d] = rotl32(x[d], 8);
    x[c] += x[d]; x[b] ^= x[c]; x[b] = rotl32(x[b], 7);
}

static uint32_t load32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* One 64-byte block of key stream for the given block number. */
static void chacha20_block(const uint8_t key[32], uint64_t counter,
                           uint32_t block_number, uint8_t out[64])
{
    uint32_t start[16], x[16];

    start[0] = 0x61707865; start[1] = 0x3320646e;      /* "expand 32-byte k" */
    start[2] = 0x79622d32; start[3] = 0x6b206574;
    for (uint8_t i = 0; i < 8; i++)
        start[4 + i] = load32_le(key + 4 * i);
    start[12] = block_number;
    start[13] = 0;                                      /* nonce: 4 zero bytes */
    start[14] = (uint32_t)counter;                      /* then the counter */
    start[15] = (uint32_t)(counter >> 32);

    memcpy(x, start, sizeof x);
    for (uint8_t round = 0; round < 10; round++) {
        for (uint8_t i = 0; i < 4; i++)         /* columns: 0 4 8 12, 1 5 9 13, ... */
            quarter(x, i, 4 + i, 8 + i, 12 + i);
        for (uint8_t i = 0; i < 4; i++)         /* diagonals: 0 5 10 15, 1 6 11 12, ... */
            quarter(x, i, 4 + ((i + 1) & 3), 8 + ((i + 2) & 3), 12 + ((i + 3) & 3));
    }
    for (uint8_t i = 0; i < 16; i++) {
        uint32_t v = x[i] + start[i];
        out[4 * i]     = (uint8_t)v;
        out[4 * i + 1] = (uint8_t)(v >> 8);
        out[4 * i + 2] = (uint8_t)(v >> 16);
        out[4 * i + 3] = (uint8_t)(v >> 24);
    }
}

/* XOR len bytes with the key stream, starting at block 1 (block 0 is used
 * for the Poly1305 key). The same operation encrypts and decrypts. */
static void chacha20_xor(const uint8_t key[32], uint64_t counter,
                         const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t stream[64];
    uint32_t block_number = 1;

    while (len) {
        size_t n = len < 64 ? len : 64;
        chacha20_block(key, counter, block_number++, stream);
        for (size_t i = 0; i < n; i++)
            out[i] = in[i] ^ stream[i];
        in += n;
        out += n;
        len -= n;
    }
}

/* ======================================================================== */
/* Poly1305, with the 130-bit numbers held as 17 bytes (low byte first).    */
/* This form uses only 8x8-bit multiplies, which is what an AVR has.        */
/* ======================================================================== */

typedef struct {
    uint8_t r[17];
    uint8_t h[17];
    uint8_t pad[17];
    uint8_t buffer[16];
    uint8_t used;
} poly1305_ctx;

static void poly_add(uint8_t h[17], const uint8_t c[17])
{
    uint16_t u = 0;
    for (uint8_t i = 0; i < 17; i++) {
        u += (uint16_t)h[i] + c[i];
        h[i] = (uint8_t)u;
        u >>= 8;
    }
}

/* h = (h + block) * r, reduced modulo 2^130 - 5 (loosely). high_bit is 1
 * for a full 16-byte block and 0 for the padded last one. */
static void poly_block(poly1305_ctx *ctx, const uint8_t m[16], uint8_t high_bit)
{
    uint8_t c[17];
    uint32_t hr[17];
    uint32_t u;

    memcpy(c, m, 16);
    c[16] = high_bit;
    poly_add(ctx->h, c);

    /* Multiply. A product that lands at byte 17 or above is 2^136 or more;
     * 2^130 is worth 5 modulo the prime, so 2^136 is worth 5 * 64 = 320. */
    for (uint8_t i = 0; i < 17; i++) {
        u = 0;
        for (uint8_t j = 0; j <= i; j++)
            u += (uint16_t)ctx->h[j] * ctx->r[i - j];
        for (uint8_t j = i + 1; j < 17; j++)
            u += (uint32_t)((uint16_t)ctx->h[j] * ctx->r[i + 17 - j]) * 320;
        hr[i] = u;
    }

    /* Carry the 17 column sums back into 17 bytes, folding the top down. */
    u = 0;
    for (uint8_t i = 0; i < 16; i++) {
        u += hr[i];
        ctx->h[i] = (uint8_t)u;
        u >>= 8;
    }
    u += hr[16];
    ctx->h[16] = (uint8_t)(u & 0x03);
    u >>= 2;
    u += u << 2;                            /* times 5 */
    for (uint8_t i = 0; i < 16; i++) {
        u += ctx->h[i];
        ctx->h[i] = (uint8_t)u;
        u >>= 8;
    }
    ctx->h[16] += (uint8_t)u;
}

static void poly_init(poly1305_ctx *ctx, const uint8_t key[32])
{
    memcpy(ctx->r, key, 16);
    ctx->r[3] &= 0x0f;  ctx->r[7] &= 0x0f;  ctx->r[11] &= 0x0f;  ctx->r[15] &= 0x0f;
    ctx->r[4] &= 0xfc;  ctx->r[8] &= 0xfc;  ctx->r[12] &= 0xfc;
    ctx->r[16] = 0;
    memcpy(ctx->pad, key + 16, 16);
    ctx->pad[16] = 0;
    memset(ctx->h, 0, sizeof ctx->h);
    ctx->used = 0;
}

static void poly_update(poly1305_ctx *ctx, const uint8_t *data, size_t len)
{
    while (len--) {
        ctx->buffer[ctx->used++] = *data++;
        if (ctx->used == 16) {
            poly_block(ctx, ctx->buffer, 1);
            ctx->used = 0;
        }
    }
}

/* The AEAD pads each section with zeros to a multiple of 16 bytes. */
static void poly_pad16(poly1305_ctx *ctx)
{
    static const uint8_t zero = 0;
    while (ctx->used)
        poly_update(ctx, &zero, 1);
}

static void poly_final(poly1305_ctx *ctx, uint8_t tag[16])
{
    static const uint8_t minus_p[17] = {
        5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xfc
    };
    uint8_t original[17];
    uint8_t keep;

    /* (In the AEAD the input is always a multiple of 16 bytes, so there is
     * never a partial block left over here.) */

    /* Full reduction: subtract the prime; keep the result unless it went
     * negative. */
    memcpy(original, ctx->h, 17);
    poly_add(ctx->h, minus_p);
    keep = (uint8_t)-(ctx->h[16] >> 7);     /* 0xff if negative */
    for (uint8_t i = 0; i < 17; i++)
        ctx->h[i] ^= keep & (original[i] ^ ctx->h[i]);

    poly_add(ctx->h, ctx->pad);
    memcpy(tag, ctx->h, 16);
}

/* ======================================================================== */
/* ChaCha20-Poly1305                                                        */
/* ======================================================================== */

static void store64_le(uint8_t *p, uint64_t v)
{
    for (uint8_t i = 0; i < 8; i++) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

/* The tag covers: associated data, padding, ciphertext, padding, and both
 * lengths as 8-byte numbers. */
static void aead_tag(const uint8_t key[32], uint64_t counter,
                     const uint8_t *ad, size_t ad_len,
                     const uint8_t *cipher, size_t len, uint8_t tag[16])
{
    poly1305_ctx poly;
    uint8_t block0[64];
    uint8_t lengths[16];

    chacha20_block(key, counter, 0, block0);    /* first 32 bytes = MAC key */
    poly_init(&poly, block0);
    poly_update(&poly, ad, ad_len);
    poly_pad16(&poly);
    poly_update(&poly, cipher, len);
    poly_pad16(&poly);
    store64_le(lengths, ad_len);
    store64_le(lengths + 8, len);
    poly_update(&poly, lengths, sizeof lengths);
    poly_final(&poly, tag);
}

void aead_seal(const uint8_t key[AEAD_KEY_BYTES], uint64_t counter,
               const uint8_t *ad, size_t ad_len,
               const uint8_t *plain, size_t len, uint8_t *out)
{
    chacha20_xor(key, counter, plain, len, out);
    aead_tag(key, counter, ad, ad_len, out, len, out + len);
}

uint8_t aead_open(const uint8_t key[AEAD_KEY_BYTES], uint64_t counter,
                  const uint8_t *ad, size_t ad_len,
                  const uint8_t *cipher, size_t len, uint8_t *out)
{
    uint8_t tag[16];
    uint8_t difference = 0;

    aead_tag(key, counter, ad, ad_len, cipher, len, tag);
    /* Compare every byte regardless, so timing says nothing about where a
     * forged tag first differs. */
    for (uint8_t i = 0; i < 16; i++)
        difference |= tag[i] ^ cipher[len + i];
    if (difference)
        return 0;
    chacha20_xor(key, counter, cipher, len, out);
    return 1;
}
