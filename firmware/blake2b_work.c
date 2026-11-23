/*
 * blake2b_work.c - see blake2b_work.h.
 */
#include "blake2b_work.h"

#include <string.h>

#include "blake2b.h"

/* Shared with the assembly routine, which addresses them by name. */
uint8_t bw_m[128];                  /* the work block, padded with zeros to one full block */
uint8_t bw_init[128];               /* the 16 state words every attempt starts from */
uint8_t bw_v[128];                  /* scratch */
uint8_t bw_out[8];
const uint8_t bw_h0[8] = {          /* IV0 ^ 0x01010020 (32-byte digest, no key), low byte first */
    0x28, 0xc9, 0xbd, 0xf2, 0x67, 0xe6, 0x09, 0x6a
};

void bw_asm(void);                  /* blake2b_work_avr.S: bw_m, bw_init -> bw_out */

#ifdef __AVR__
static uint8_t fast = 1;
#else
static uint8_t fast = 0;            /* no assembly on the PC */
#endif

uint8_t bw_is_fast(void) { return fast; }

/* ---- the same arithmetic as blake2b.c, for the per-job part ---- */

static const uint64_t IV[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

static uint64_t rotr(uint64_t x, uint8_t n) { return (x >> n) | (x << (64 - n)); }

static void mix(uint64_t *v, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint64_t x, uint64_t y)
{
    v[a] = v[a] + v[b] + x;  v[d] = rotr(v[d] ^ v[a], 32);
    v[c] = v[c] + v[d];      v[b] = rotr(v[b] ^ v[c], 24);
    v[a] = v[a] + v[b] + y;  v[d] = rotr(v[d] ^ v[a], 16);
    v[c] = v[c] + v[d];      v[b] = rotr(v[b] ^ v[c], 63);
}

static uint64_t load64(const uint8_t *p)
{
    uint64_t x = 0;

    for (int8_t i = 7; i >= 0; i--)
        x = (x << 8) | p[i];
    return x;
}

static void store64(uint8_t *p, uint64_t x)
{
    for (uint8_t i = 0; i < 8; i++, x >>= 8)
        p[i] = (uint8_t)x;
}

void bw_setup(const uint8_t work[80])
{
    uint64_t v[16];

    memset(bw_m, 0, sizeof bw_m);
    memcpy(bw_m, work, 80);
    memset(bw_m + 32, 0, 8);                /* the nonce field is bw_hash()'s */

    for (uint8_t i = 0; i < 8; i++)
        v[i] = v[8 + i] = IV[i];
    v[0] ^= 0x01010020;                     /* 32-byte digest, no key */
    v[12] ^= 80;                            /* bytes hashed */
    v[14] = ~v[14];                         /* this block is the last */

    /* Round 0 mixes four columns, each with two message words. The nonce is
     * in word 4, so only the third column depends on it; the other three
     * come out the same for every attempt and are done here, once. */
    mix(v, 0, 4, 8, 12, load64(bw_m + 0), load64(bw_m + 8));
    mix(v, 1, 5, 9, 13, load64(bw_m + 16), load64(bw_m + 24));
    mix(v, 3, 7, 11, 15, load64(bw_m + 48), load64(bw_m + 56));

    for (uint8_t i = 0; i < 16; i++)
        store64(bw_init + 8 * i, v[i]);
}

/* ---- a C model of the assembly's route (the PC's bw_hash, and a test of
 *      bw_setup: it starts from bw_init like the assembly does) ---- */
#ifndef __AVR__
static const uint8_t SIGMA[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}
};

static void model(void)
{
    uint64_t v[16], m[16];

    for (uint8_t i = 0; i < 16; i++) {
        v[i] = load64(bw_init + 8 * i);
        m[i] = load64(bw_m + 8 * i);
    }
    for (uint8_t r = 0; r < 12; r++) {
        const uint8_t *s = SIGMA[r % 10];

        if (r != 0) {                       /* round 0: done by bw_setup() */
            mix(v, 0, 4, 8, 12, m[s[0]], m[s[1]]);
            mix(v, 1, 5, 9, 13, m[s[2]], m[s[3]]);
            mix(v, 3, 7, 11, 15, m[s[6]], m[s[7]]);
        }
        mix(v, 2, 6, 10, 14, m[s[4]], m[s[5]]);
        mix(v, 0, 5, 10, 15, m[s[8]], m[s[9]]);
        mix(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        mix(v, 2, 7, 8, 13, m[s[12]], m[s[13]]);
        mix(v, 3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    store64(bw_out, load64(bw_h0) ^ v[0] ^ v[8]);
}
#endif

/* ---- the general algorithm, for checking and as the fallback ---- */

static void plain(uint8_t first8[8])
{
    blake2b_ctx ctx;
    uint8_t digest[32];

    blake2b_init(&ctx, 32);
    blake2b_update(&ctx, bw_m, 80);
    blake2b_final(&ctx, digest);
    memcpy(first8, digest, 8);
}

void bw_hash(uint32_t nonce)
{
    bw_m[32] = (uint8_t)nonce;
    bw_m[33] = (uint8_t)(nonce >> 8);
    bw_m[34] = (uint8_t)(nonce >> 16);
    bw_m[35] = (uint8_t)(nonce >> 24);
#ifdef __AVR__
    if (fast)
        bw_asm();
    else
        plain(bw_out);
#else
    model();
#endif
}

uint8_t bw_selftest(void)
{
    static const uint32_t nonces[] = { 0, 1, 255, 256, 0x12345678UL, 0x80000000UL, 0xffffffffUL };
    uint8_t work[80], want[8];

    for (uint8_t pass = 0; pass < 3; pass++) {
        for (uint8_t i = 0; i < 80; i++)
            work[i] = (uint8_t)(pass == 0 ? 0 : (i * 37 + pass * 101 + 11));
        if (pass == 2)
            memset(work + 32, 0, 16);       /* nonce and time zero, as the live pools send */
        bw_setup(work);
        for (uint8_t n = 0; n < 24; n++) {
            uint32_t nonce = n < 7 ? nonces[n] : 0x00c0ffeeUL * n + pass;

            bw_hash(nonce);
            plain(want);                    /* bw_hash left the nonce in bw_m */
            if (memcmp(bw_out, want, 8) != 0) {
                fast = 0;
                return 0;
            }
        }
    }
    return 1;
}
