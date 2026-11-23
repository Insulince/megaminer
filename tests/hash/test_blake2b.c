/*
 * test_blake2b.c - desktop test for blake2b.c. Not for the chip.
 *
 * Run:  make test
 *
 * Checks our BLAKE2b against:
 *   1. the "abc" example printed in RFC 7693, Appendix A;
 *   2. digests produced by Python's hashlib (blake2b_vectors.h), each one
 *      hashed three ways: all at once, one byte at a time, and in 7-byte
 *      pieces, to exercise the block-buffering logic.
 */

#include <stdio.h>
#include <string.h>

#include "blake2b.h"
#include "blake2b_vectors.h"

static void to_hex(const uint8_t *bytes, unsigned n, char *hex)
{
    for (unsigned i = 0; i < n; i++) {
        sprintf(hex + 2 * i, "%02x", bytes[i]);
    }
}

/* Hash `in` feeding at most `chunk` bytes per update call; return hex. */
static void hash_hex(const uint8_t *in, unsigned inlen, unsigned outlen,
                     unsigned chunk, char *hex)
{
    blake2b_ctx ctx;
    uint8_t digest[BLAKE2B_MAX_OUT];

    blake2b_init(&ctx, (uint8_t)outlen);
    for (unsigned off = 0; off < inlen; off += chunk) {
        unsigned n = inlen - off < chunk ? inlen - off : chunk;
        blake2b_update(&ctx, in + off, n);
    }
    blake2b_final(&ctx, digest);
    to_hex(digest, outlen, hex);
}

static int check(const char *name, const char *got, const char *want)
{
    if (strcmp(got, want) == 0) {
        printf("ok    %s\n", name);
        return 0;
    }
    printf("FAIL  %s\n  got  %s\n  want %s\n", name, got, want);
    return 1;
}

int main(void)
{
    static uint8_t input[1000];
    char hex[2 * BLAKE2B_MAX_OUT + 1];
    char name[64];
    int failures = 0;

    /* RFC 7693, Appendix A. */
    hash_hex((const uint8_t *)"abc", 3, 64, 3, hex);
    failures += check("RFC 7693 \"abc\"", hex,
        "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
        "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");

    /* XBT proof-of-work vector: the 80-byte "asic_input" and block hash of
     * profile_0_time_offset from Bitcoin Knots' src/test/data/
     * block_header_v2.json, as copied into datum_gateway's
     * datum_pow_tests.c. The block hash is the 32-byte BLAKE2b digest of
     * the work block with its bytes reversed (XOR key is all zeros here). */
    {
        static const char work_hex[] =
            "000000000000943aff74219e1f45899abfdf536373c0f2fc92e6fe58335cd0ad"
            "0df0ad0b4433221158020000efcdab89"
            "7e6326906eaa52fe59e03a14f1dfb8dd5d6e78497e56a8a6e4f4fb4d385e43db";
        static const char hash_reversed_hex[] =
            "04d78755b174467ec8537c230912ddd9bc4f28229b795b78490ad705cf5d494b";
        uint8_t work[80];
        char digest_hex[65], reversed_hex[65];

        for (unsigned i = 0; i < sizeof work; i++) {
            unsigned byte;
            sscanf(work_hex + 2 * i, "%2x", &byte);
            work[i] = (uint8_t)byte;
        }
        hash_hex(work, sizeof work, 32, sizeof work, digest_hex);
        for (unsigned i = 0; i < 32; i++) {
            memcpy(reversed_hex + 2 * i, digest_hex + 2 * (31 - i), 2);
        }
        reversed_hex[64] = '\0';
        failures += check("XBT work block (Knots vector)", reversed_hex,
                          hash_reversed_hex);
    }

    for (unsigned i = 0; i < sizeof input; i++) {
        input[i] = (uint8_t)(i % 251);
    }

    static const unsigned chunks[] = { 1000, 1, 7 };
    for (unsigned v = 0; v < sizeof VECTORS / sizeof VECTORS[0]; v++) {
        for (unsigned c = 0; c < 3; c++) {
            hash_hex(input, VECTORS[v].inlen, VECTORS[v].outlen, chunks[c], hex);
            sprintf(name, "in=%u out=%u chunk=%u",
                    VECTORS[v].inlen, VECTORS[v].outlen, chunks[c]);
            failures += check(name, hex, VECTORS[v].hex);
        }
    }

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
