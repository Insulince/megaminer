/*
 * test_blake2b_work.c - PC test of blake2b_work.c (run by `make test`).
 *
 * Checks the per-job setup and the shortcut route (in its C model form)
 * against the general blake2b.c for many pseudo-random work blocks and
 * nonces. The chip's assembly version of the same route is tested by
 * gen_blake2b_avr.py and, on the chip, by bw_selftest() at start-up.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "blake2b.h"
#include "blake2b_work.h"

static uint32_t seed = 20261116;

static uint32_t next(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

int main(void)
{
    uint8_t work[80], digest[32];
    blake2b_ctx ctx;
    long checked = 0;

    if (!bw_selftest()) {
        printf("blake2b_work: FAILED its own self-test\n");
        return 1;
    }
    for (int block = 0; block < 500; block++) {
        for (int i = 0; i < 80; i++)
            work[i] = (uint8_t)next();
        memset(work + 32, 0, 8);                    /* the nonce field */
        if (block % 3 == 0)
            memset(work + 40, 0, 8);                /* zero time, as the live pools send */
        bw_setup(work);
        for (int n = 0; n < 40; n++) {
            uint32_t nonce = n == 0 ? 0 : n == 1 ? 0xffffffffUL : next();

            work[32] = (uint8_t)nonce;
            work[33] = (uint8_t)(nonce >> 8);
            work[34] = (uint8_t)(nonce >> 16);
            work[35] = (uint8_t)(nonce >> 24);
            blake2b_init(&ctx, 32);
            blake2b_update(&ctx, work, 80);
            blake2b_final(&ctx, digest);
            bw_hash(nonce);
            if (memcmp(bw_out, digest, 8) != 0) {
                printf("blake2b_work: MISMATCH block %d nonce %08lx\n", block, (unsigned long)nonce);
                return 1;
            }
            checked++;
        }
    }
    printf("blake2b_work: all tests passed (self-test, %ld hashes against blake2b.c)\n", checked);
    return 0;
}
