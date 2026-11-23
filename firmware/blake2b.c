/*
 * blake2b.c - BLAKE2b hash (RFC 7693), unkeyed. See blake2b.h.
 *
 * Shape of the algorithm:
 *   - The state is eight 64-bit words, h[0..7].
 *   - Input is consumed in 128-byte blocks. Each block is stirred into the
 *     state by compress().
 *   - compress() runs 12 rounds. Each round calls the mixing step G eight
 *     times. G uses only three operations: 64-bit add, XOR, and rotate.
 *   - The digest is the final state written out as bytes.
 */

#include "blake2b.h"

#include <string.h>

/* Starting state. Same constants as SHA-512: the fractional parts of the
 * square roots of the first eight primes. */
static const uint64_t IV[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

/* Which message word each G call uses, per round. Rounds 10 and 11 reuse
 * rows 0 and 1. */
static const uint8_t SIGMA[10][16] = {
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
    { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
    {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
    {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
    {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
    { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
    { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
    {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
    { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
};

static uint64_t rotr64(uint64_t x, uint8_t n)
{
    return (x >> n) | (x << (64 - n));
}

/* Read 8 bytes as a little-endian 64-bit word, whatever the CPU's own order. */
static uint64_t load64(const uint8_t *p)
{
    uint64_t w = 0;
    for (int8_t i = 7; i >= 0; i--) {
        w = (w << 8) | p[i];
    }
    return w;
}

/* The mixing step: stirs two message words (x, y) into four state words. */
static void G(uint64_t *v, uint8_t a, uint8_t b, uint8_t c, uint8_t d,
              uint64_t x, uint64_t y)
{
    v[a] = v[a] + v[b] + x;
    v[d] = rotr64(v[d] ^ v[a], 32);
    v[c] = v[c] + v[d];
    v[b] = rotr64(v[b] ^ v[c], 24);
    v[a] = v[a] + v[b] + y;
    v[d] = rotr64(v[d] ^ v[a], 16);
    v[c] = v[c] + v[d];
    v[b] = rotr64(v[b] ^ v[c], 63);
}

/* Stir the 128-byte block in ctx->buf into the state. */
static void compress(blake2b_ctx *ctx, uint8_t is_last_block)
{
    uint64_t m[16];     /* the block as sixteen 64-bit words */
    uint64_t v[16];     /* working copy: state on top, constants below */

    for (uint8_t i = 0; i < 16; i++) {
        m[i] = load64(ctx->buf + 8 * i);
    }

    for (uint8_t i = 0; i < 8; i++) {
        v[i]     = ctx->h[i];
        v[i + 8] = IV[i];
    }
    v[12] ^= ctx->t[0];             /* mix in how many bytes came before, */
    v[13] ^= ctx->t[1];             /* so identical blocks hash differently */
    if (is_last_block) {
        v[14] = ~v[14];             /* mark the end of the message */
    }

    for (uint8_t round = 0; round < 12; round++) {
        const uint8_t *s = SIGMA[round % 10];

        G(v, 0, 4,  8, 12, m[s[ 0]], m[s[ 1]]);    /* four columns */
        G(v, 1, 5,  9, 13, m[s[ 2]], m[s[ 3]]);
        G(v, 2, 6, 10, 14, m[s[ 4]], m[s[ 5]]);
        G(v, 3, 7, 11, 15, m[s[ 6]], m[s[ 7]]);
        G(v, 0, 5, 10, 15, m[s[ 8]], m[s[ 9]]);    /* four diagonals */
        G(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        G(v, 2, 7,  8, 13, m[s[12]], m[s[13]]);
        G(v, 3, 4,  9, 14, m[s[14]], m[s[15]]);
    }

    for (uint8_t i = 0; i < 8; i++) {
        ctx->h[i] ^= v[i] ^ v[i + 8];
    }
}

static void add_to_count(blake2b_ctx *ctx, uint8_t n)
{
    ctx->t[0] += n;
    if (ctx->t[0] < n) {
        ctx->t[1]++;                /* carry into the high word */
    }
}

void blake2b_init(blake2b_ctx *ctx, uint8_t outlen)
{
    memset(ctx, 0, sizeof *ctx);
    for (uint8_t i = 0; i < 8; i++) {
        ctx->h[i] = IV[i];
    }
    /* Parameter word: digest length, no key, default tree settings. This
     * makes a 32-byte digest unrelated to a truncated 64-byte one. */
    ctx->h[0] ^= 0x01010000ULL ^ outlen;
    ctx->outlen = outlen;
}

void blake2b_update(blake2b_ctx *ctx, const void *in, size_t inlen)
{
    const uint8_t *p = in;

    while (inlen > 0) {
        /* Only compress a full buffer once we know more input follows: the
         * final block must be compressed with the "last" flag instead. */
        if (ctx->buflen == BLAKE2B_BLOCK_BYTES) {
            add_to_count(ctx, BLAKE2B_BLOCK_BYTES);
            compress(ctx, 0);
            ctx->buflen = 0;
        }

        size_t n = BLAKE2B_BLOCK_BYTES - ctx->buflen;
        if (n > inlen) {
            n = inlen;
        }
        memcpy(ctx->buf + ctx->buflen, p, n);
        ctx->buflen += (uint8_t)n;
        p     += n;
        inlen -= n;
    }
}

void blake2b_final(blake2b_ctx *ctx, uint8_t *out)
{
    add_to_count(ctx, ctx->buflen);
    memset(ctx->buf + ctx->buflen, 0, BLAKE2B_BLOCK_BYTES - ctx->buflen);
    compress(ctx, 1);

    /* The digest is the state, written out least-significant byte first. */
    for (uint8_t i = 0; i < ctx->outlen; i++) {
        out[i] = (uint8_t)(ctx->h[i / 8] >> (8 * (i % 8)));
    }
}
