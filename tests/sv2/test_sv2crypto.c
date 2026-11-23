/*
 * test_sv2crypto.c - PC test of sv2crypto.c against results computed by Go
 * (sv2crypto_vectors.h, made by cmd/sv2vectors).
 *
 * Run with:  make test-sv2
 */

#include <stdio.h>
#include <string.h>

#include "sv2crypto.h"
#include "sv2crypto_vectors.h"

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
    uint8_t out[32], out2[32];
    uint8_t buf[216];

    for (unsigned i = 0; i < COUNT(sha_vectors); i++) {
        const struct sha_vector *v = &sha_vectors[i];
        sha256_ctx ctx;

        /* All at once, then one byte at a time. */
        sha256_init(&ctx);
        sha256_update(&ctx, v->data, v->len);
        sha256_final(&ctx, out);
        check("sha256", i, memcmp(out, v->want, 32) == 0);

        sha256_init(&ctx);
        for (unsigned j = 0; j < v->len; j++)
            sha256_update(&ctx, v->data + j, 1);
        sha256_final(&ctx, out);
        check("sha256 bytewise", i, memcmp(out, v->want, 32) == 0);
    }

    for (unsigned i = 0; i < COUNT(hmac_vectors); i++) {
        const struct hmac_vector *v = &hmac_vectors[i];
        hmac_sha256(v->key, v->key_len, v->data, v->len, out);
        check("hmac", i, memcmp(out, v->want, 32) == 0);
    }

    for (unsigned i = 0; i < COUNT(hkdf_vectors); i++) {
        const struct hkdf_vector *v = &hkdf_vectors[i];
        noise_hkdf2(v->ck, v->material, v->len, out, out2);
        check("hkdf out1", i, memcmp(out, v->want1, 32) == 0);
        check("hkdf out2", i, memcmp(out2, v->want2, 32) == 0);

        /* out1 is allowed to overwrite the chaining key. */
        memcpy(out, v->ck, 32);
        noise_hkdf2(out, v->material, v->len, out, out2);
        check("hkdf in place", i, memcmp(out, v->want1, 32) == 0 &&
                                  memcmp(out2, v->want2, 32) == 0);
    }

    for (unsigned i = 0; i < COUNT(aead_vectors); i++) {
        const struct aead_vector *v = &aead_vectors[i];

        aead_seal(v->key, v->counter, v->ad, v->ad_len, v->plain, v->len, buf);
        check("aead seal", i, memcmp(buf, v->sealed, v->len + 16u) == 0);

        /* Open in place. */
        memcpy(buf, v->sealed, v->len + 16u);
        check("aead open", i,
              aead_open(v->key, v->counter, v->ad, v->ad_len, buf, v->len, buf) &&
              memcmp(buf, v->plain, v->len) == 0);

        /* Any single damaged bit must be refused: in the tag, in the
         * ciphertext, in the associated data, and with the wrong counter. */
        memcpy(buf, v->sealed, v->len + 16u);
        buf[v->len + 5] ^= 0x01;
        check("aead bad tag", i,
              !aead_open(v->key, v->counter, v->ad, v->ad_len, buf, v->len, buf));
        if (v->len) {
            memcpy(buf, v->sealed, v->len + 16u);
            buf[v->len / 2] ^= 0x80;
            check("aead bad ciphertext", i,
                  !aead_open(v->key, v->counter, v->ad, v->ad_len, buf, v->len, buf));
        }
        if (v->ad_len) {
            uint8_t ad[32];
            memcpy(ad, v->ad, sizeof ad);
            ad[0] ^= 0x01;
            memcpy(buf, v->sealed, v->len + 16u);
            check("aead bad ad", i,
                  !aead_open(v->key, v->counter, ad, v->ad_len, buf, v->len, buf));
        }
        memcpy(buf, v->sealed, v->len + 16u);
        check("aead wrong counter", i,
              !aead_open(v->key, v->counter + 1, v->ad, v->ad_len, buf, v->len, buf));
    }

    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("sv2crypto: all tests passed (%u sha256, %u hmac, %u hkdf, %u aead)\n",
           (unsigned)COUNT(sha_vectors), (unsigned)COUNT(hmac_vectors),
           (unsigned)COUNT(hkdf_vectors), (unsigned)COUNT(aead_vectors));
    return 0;
}
