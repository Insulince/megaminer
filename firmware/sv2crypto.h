/*
 * sv2crypto.h - the symmetric cryptography Stratum V2's encrypted transport
 * needs: SHA-256, HMAC-SHA256, the two-output HKDF that Noise uses, and the
 * ChaCha20-Poly1305 authenticated cipher.
 *
 * Portable C with no hardware in it, like blake2b.c: it is tested on the PC
 * (make test-sv2) before it ever runs on the chip.
 *
 * The elliptic-curve half of the handshake is NOT here.
 */

#ifndef SV2CRYPTO_H
#define SV2CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_BYTES   32
#define AEAD_KEY_BYTES 32
#define AEAD_TAG_BYTES 16

/* ---- SHA-256 (FIPS 180-4) ---- */

typedef struct {
    uint32_t h[8];
    uint8_t  block[64];
    uint8_t  used;          /* bytes waiting in block */
    uint32_t total;         /* bytes hashed so far (inputs here are tiny) */
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t out[SHA256_BYTES]);

/* ---- HMAC-SHA256 (RFC 2104). The key must be 64 bytes or shorter. ---- */

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t out[SHA256_BYTES]);

/* ---- Noise's HKDF with two outputs ----
 * RFC 5869 with the chaining key as salt, `material` as input key material
 * and empty info; out1 and out2 are the first two 32-byte output blocks.
 * out1 may be the same array as chaining_key. */

void noise_hkdf2(const uint8_t chaining_key[SHA256_BYTES],
                 const uint8_t *material, size_t material_len,
                 uint8_t out1[SHA256_BYTES], uint8_t out2[SHA256_BYTES]);

/* ---- ChaCha20-Poly1305 (RFC 8439), with Noise's nonce layout ----
 * The 12-byte nonce is four zero bytes followed by `counter`, low byte first.
 *
 * seal: writes len bytes of ciphertext followed by the 16-byte tag to out
 *       (len + 16 bytes in all). out may be the same buffer as plain.
 * open: `len` is the ciphertext length WITHOUT the tag; the tag is the 16
 *       bytes after it. Returns 1 and writes len bytes of plaintext to out if
 *       the tag is right; returns 0 and writes nothing if it is wrong.
 *       out may be the same buffer as cipher. */

void aead_seal(const uint8_t key[AEAD_KEY_BYTES], uint64_t counter,
               const uint8_t *ad, size_t ad_len,
               const uint8_t *plain, size_t len, uint8_t *out);

uint8_t aead_open(const uint8_t key[AEAD_KEY_BYTES], uint64_t counter,
                  const uint8_t *ad, size_t ad_len,
                  const uint8_t *cipher, size_t len, uint8_t *out);

#endif
