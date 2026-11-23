/*
 * sv2curve.h - the elliptic-curve half of Stratum V2's handshake, on the
 * secp256k1 curve:
 *
 *   - ElligatorSwift key encoding (BIP324): a public key sent as 64 bytes
 *     that look random
 *   - v2_ecdh (BIP324): two sides each combine their own secret key with the
 *     other's public key and arrive at the same 32-byte shared secret
 *   - Schnorr signature verification (BIP340): checks the server's certificate
 *
 * The big-number and curve arithmetic underneath is the micro-ecc library
 * (third_party/micro-ecc, BSD licence). Portable C; tested on the PC with
 * make test-sv2.
 *
 * All numbers are 32 bytes, most significant byte first, as in the BIPs.
 * Every function returns 1 on success and 0 on failure.
 */

#ifndef SV2CURVE_H
#define SV2CURVE_H

#include <stdint.h>

#define ELLSWIFT_BYTES 64

/* Make the 64-byte ElligatorSwift encoding of the public key that belongs to
 * secret key `priv`. `seed` is 32 more random bytes used to pick one of the
 * many valid encodings. Fails only if priv is zero or not below the curve
 * order (try again with new random bytes). */
uint8_t sv2_ellswift_create(const uint8_t priv[32], const uint8_t seed[32],
                            uint8_t ell[ELLSWIFT_BYTES]);

/* Recover the public key's x coordinate from an encoding. Every 64-byte
 * string is a valid encoding, so this cannot fail. */
void sv2_ellswift_decode(const uint8_t ell[ELLSWIFT_BYTES], uint8_t x[32]);

/* BIP324 v2_ecdh. `theirs` and `ours` are the two sides' encodings;
 * `initiating` is 1 for the side that opened the connection (the miner). */
uint8_t sv2_ecdh(const uint8_t priv[32],
                 const uint8_t theirs[ELLSWIFT_BYTES],
                 const uint8_t ours[ELLSWIFT_BYTES],
                 uint8_t initiating, uint8_t secret[32]);

/* BIP340: is `sig` a valid signature of the 32-byte message `msg` by the key
 * whose x coordinate is `pubkey`? */
uint8_t sv2_schnorr_verify(const uint8_t pubkey[32], const uint8_t msg[32],
                           const uint8_t sig[64]);

/* Some of these take seconds on the chip. If this is set, it is called
 * every few milliseconds to few hundred milliseconds while they run, so the
 * program can stay responsive. It must not call anything in this file. */
extern void (*sv2curve_idle)(void);

#endif
