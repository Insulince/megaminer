/*
 * test_sv2curve.c - PC test of sv2curve.c against results computed by btcec
 * (sv2curve_vectors.h, made by cmd/sv2curvevectors), plus the official
 * BIP340 test vectors that matter for verification.
 *
 * Run with:  make test-sv2
 */

#include <stdio.h>
#include <string.h>

#include "sv2curve.h"
#include "sv2curve_vectors.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static int failures;

static void check(const char *what, unsigned index, int ok)
{
    if (!ok) {
        printf("FAIL  %s #%u\n", what, index);
        failures++;
    }
}

int main(void)
{
    uint8_t out[32], ell[64], ell2[64];
    unsigned i;

    for (i = 0; i < COUNT(decode_vectors); i++) {
        sv2_ellswift_decode(decode_vectors[i].ell, out);
        check("ellswift decode", i, memcmp(out, decode_vectors[i].x, 32) == 0);
    }

    /* Our own encodings must decode back to the right public key, and
     * different seeds must give different encodings of the same key. */
    for (i = 0; i < COUNT(key_vectors); i++) {
        uint8_t seed[32];

        for (uint8_t round = 0; round < 6; round++) {
            memset(seed, (uint8_t)(i * 16 + round), sizeof seed);
            check("ellswift create", i,
                  sv2_ellswift_create(key_vectors[i].priv, seed, ell));
            sv2_ellswift_decode(ell, out);
            check("ellswift create round trip", i,
                  memcmp(out, key_vectors[i].x, 32) == 0);
            if (round == 0)
                memcpy(ell2, ell, 64);
            else
                check("ellswift encodings differ", i, memcmp(ell, ell2, 64) != 0);
        }
    }
    {
        static const uint8_t zero[32];
        uint8_t all_ff[32];
        memset(all_ff, 0xff, sizeof all_ff);
        check("create refuses zero key", 0, !sv2_ellswift_create(zero, zero, ell));
        check("create refuses big key", 0, !sv2_ellswift_create(all_ff, zero, ell));
    }

    for (i = 0; i < COUNT(ecdh_vectors); i++) {
        const struct ecdh_vector *v = &ecdh_vectors[i];
        check("ecdh", i,
              sv2_ecdh(v->priv, v->theirs, v->ours, v->initiating, out) &&
              memcmp(out, v->secret, 32) == 0);
    }

    /* Two sides, each with its own key, must reach the same secret. */
    {
        const uint8_t *a = key_vectors[0].priv, *b = key_vectors[1].priv;
        uint8_t seed[32] = {1}, ell_a[64], ell_b[64], sa[32], sb[32];

        sv2_ellswift_create(a, seed, ell_a);
        seed[0] = 2;
        sv2_ellswift_create(b, seed, ell_b);
        check("ecdh agreement", 0,
              sv2_ecdh(a, ell_b, ell_a, 1, sa) && sv2_ecdh(b, ell_a, ell_b, 0, sb) &&
              memcmp(sa, sb, 32) == 0);
    }

    for (i = 0; i < COUNT(schnorr_vectors); i++) {
        const struct schnorr_vector *v = &schnorr_vectors[i];
        uint8_t bad[64];

        check("schnorr valid", i, sv2_schnorr_verify(v->pubkey, v->msg, v->sig));

        /* One flipped bit anywhere must be refused. */
        memcpy(bad, v->sig, 64);
        bad[3] ^= 0x10;
        check("schnorr bad r", i, !sv2_schnorr_verify(v->pubkey, v->msg, bad));
        memcpy(bad, v->sig, 64);
        bad[60] ^= 0x01;
        check("schnorr bad s", i, !sv2_schnorr_verify(v->pubkey, v->msg, bad));
        memcpy(bad, v->msg, 32);
        bad[0] ^= 0x80;
        check("schnorr bad msg", i, !sv2_schnorr_verify(v->pubkey, bad, v->sig));
        check("schnorr wrong key", i,
              !sv2_schnorr_verify(schnorr_vectors[(i + 1) % COUNT(schnorr_vectors)].pubkey,
                                  v->msg, v->sig));

        /* Out-of-range and zero values must be refused, not crash. */
        memcpy(bad, v->sig, 64);
        memset(bad + 32, 0xff, 32);
        check("schnorr s too big", i, !sv2_schnorr_verify(v->pubkey, v->msg, bad));
        memset(bad + 32, 0x00, 32);
        check("schnorr s zero", i, !sv2_schnorr_verify(v->pubkey, v->msg, bad));
        memcpy(bad, v->sig, 64);
        memset(bad, 0xff, 32);
        check("schnorr r too big", i, !sv2_schnorr_verify(v->pubkey, v->msg, bad));
        memset(bad, 0xff, 32);
        check("schnorr key too big", i, !sv2_schnorr_verify(bad, v->msg, v->sig));
    }

    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("sv2curve: all tests passed (%u decode, %u keys, %u ecdh, %u schnorr)\n",
           (unsigned)COUNT(decode_vectors), (unsigned)COUNT(key_vectors),
           (unsigned)COUNT(ecdh_vectors), (unsigned)COUNT(schnorr_vectors));
    return 0;
}
