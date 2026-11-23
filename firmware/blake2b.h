/*
 * blake2b.h - BLAKE2b hash (RFC 7693), unkeyed, written to run unchanged on
 * a desktop and on the ATmega1284P: fixed-size state, no malloc.
 *
 * Usage:
 *     blake2b_ctx ctx;
 *     uint8_t digest[64];
 *     blake2b_init(&ctx, 64);
 *     blake2b_update(&ctx, data, len);     (may be called repeatedly)
 *     blake2b_final(&ctx, digest);
 */

#ifndef BLAKE2B_H
#define BLAKE2B_H

#include <stddef.h>
#include <stdint.h>

#define BLAKE2B_BLOCK_BYTES 128
#define BLAKE2B_MAX_OUT     64

typedef struct {
    uint64_t h[8];                      /* the running hash state */
    uint64_t t[2];                      /* bytes hashed so far (128-bit count) */
    uint8_t  buf[BLAKE2B_BLOCK_BYTES];  /* input waiting to fill a block */
    uint8_t  buflen;                    /* bytes currently in buf */
    uint8_t  outlen;                    /* digest length in bytes, 1..64 */
} blake2b_ctx;

void blake2b_init(blake2b_ctx *ctx, uint8_t outlen);
void blake2b_update(blake2b_ctx *ctx, const void *in, size_t inlen);
void blake2b_final(blake2b_ctx *ctx, uint8_t *out);

#endif
