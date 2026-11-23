/*
 * sv2curve.c - see sv2curve.h.
 *
 * micro-ecc is compiled as part of this file (the #include of uECC.c below)
 * so that its build options are set in exactly one place and nothing else in
 * the project needs special compiler flags.
 *
 * Formulas: BIP324 (XSwiftEC, XElligatorSwift, v2_ecdh) and BIP340 (verify).
 */

/* ---- micro-ecc, configured for this project ---- */
#define uECC_ENABLE_VLI_API           1     /* expose the big-number functions */
#define uECC_SUPPORT_COMPRESSED_POINT 1     /* needed for the square root */
#define uECC_SUPPORTS_secp160r1       0
#define uECC_SUPPORTS_secp192r1       0
#define uECC_SUPPORTS_secp224r1       0
#define uECC_SUPPORTS_secp256r1       0
#define uECC_SUPPORTS_secp256k1       1
/* Our one hook inside the library: called once per step of a curve
 * multiplication (256 steps, about 13 ms each on the chip). */
void (*sv2curve_idle)(void);
#define uECC_YIELD() do { if (sv2curve_idle) sv2curve_idle(); } while (0)
#include "third_party/micro-ecc/uECC.c"
#include "third_party/micro-ecc/uECC_vli.h"

#include <string.h>

#include "sv2crypto.h"
#include "sv2curve.h"

/* A number modulo the field prime p (or a scalar), in micro-ecc's own
 * layout: an array of machine words, least significant first. One word is a
 * byte on the AVR and 8 bytes on the PC. */
#define NW (32 / uECC_WORD_SIZE)
typedef uECC_word_t fe[NW];

#define CURVE   uECC_secp256k1()
#define FIELD_P uECC_curve_p(CURVE)
#define ORDER_N uECC_curve_n(CURVE)

/* ---- field arithmetic (all results are below p) ---- */

static void fe_from_bytes(fe r, const uint8_t b[32])
{
    uECC_vli_bytesToNative(r, b, 32);
    if (uECC_vli_cmp(r, FIELD_P, NW) >= 0)          /* reduce modulo p */
        uECC_vli_sub(r, r, FIELD_P, NW);
}

static void fe_to_bytes(uint8_t b[32], const fe a)
{
    uECC_vli_nativeToBytes(b, 32, a);
}

static void fe_small(fe r, uECC_word_t value)
{
    uECC_vli_clear(r, NW);
    r[0] = value;
}

static uint8_t fe_is_zero(const fe a) { return uECC_vli_isZero(a, NW) != 0; }
static uint8_t fe_equal(const fe a, const fe b) { return uECC_vli_equal(a, b, NW) != 0; }
static uint8_t fe_is_odd(const fe a) { return a[0] & 1; }
static void fe_copy(fe r, const fe a) { uECC_vli_set(r, a, NW); }
static void fe_add(fe r, const fe a, const fe b) { uECC_vli_modAdd(r, a, b, FIELD_P, NW); }
static void fe_sub(fe r, const fe a, const fe b) { uECC_vli_modSub(r, a, b, FIELD_P, NW); }
static void fe_mul(fe r, const fe a, const fe b) { uECC_vli_modMult_fast(r, a, b, CURVE); }
static void fe_inv(fe r, const fe a) { uECC_vli_modInv(r, a, FIELD_P, NW); }

static void fe_neg(fe r, const fe a)
{
    fe zero;
    fe_small(zero, 0);
    fe_sub(r, zero, a);
}

/* r = a square root of a, if it has one. Returns 1 if so. r may be a. */
static uint8_t fe_sqrt(fe r, const fe a)
{
    fe root, check;

    fe_copy(root, a);
    uECC_vli_mod_sqrt(root, CURVE);         /* a candidate: a^((p+1)/4) */
    fe_mul(check, root, root);
    if (!fe_equal(check, a))
        return 0;
    fe_copy(r, root);
    return 1;
}

/* r = x^3 + 7, the right-hand side of the curve equation y^2 = x^3 + 7. */
static void fe_curve_rhs(fe r, const fe x)
{
    fe t, seven;

    fe_mul(t, x, x);
    fe_mul(t, t, x);
    fe_small(seven, 7);
    fe_add(r, t, seven);
}

/* The square root of -3 that BIP324 uses. */
static const uint8_t SQRT_MINUS_3[32] = {
    0x0a, 0x2d, 0x2b, 0xa9, 0x35, 0x07, 0xf1, 0xdf, 0x23, 0x37, 0x70, 0xc2,
    0xa7, 0x97, 0x96, 0x2c, 0xc6, 0x1f, 0x6d, 0x15, 0xda, 0x14, 0xec, 0xd4,
    0x7d, 0x8d, 0x27, 0xae, 0x1c, 0xd5, 0xf8, 0x52
};

/* ---- ElligatorSwift ---- */

/* BIP324 XSwiftEC: from any pair (u, t) to the x coordinate of a point on
 * the curve. */
static void xswiftec(fe x, const fe u_in, const fe t_in)
{
    fe u, t, g, t2, X, Y, q, tmp, c, half;

    fe_copy(u, u_in);
    fe_copy(t, t_in);
    if (fe_is_zero(u)) fe_small(u, 1);
    if (fe_is_zero(t)) fe_small(t, 1);

    fe_curve_rhs(g, u);                     /* g = u^3 + 7 */
    fe_mul(t2, t, t);
    fe_add(tmp, g, t2);
    if (fe_is_zero(tmp)) {                  /* u^3 + t^2 + 7 == 0: use 2t */
        fe_add(t, t, t);
        fe_mul(t2, t, t);
    }

    fe_sub(X, g, t2);                       /* X = (u^3 + 7 - t^2) / (2t) */
    fe_add(tmp, t, t);
    fe_inv(tmp, tmp);
    fe_mul(X, X, tmp);

    fe_from_bytes(c, SQRT_MINUS_3);         /* Y = (X + t) / (c u) */
    fe_mul(tmp, c, u);
    fe_inv(tmp, tmp);
    fe_add(Y, X, t);
    fe_mul(Y, Y, tmp);

    /* Three candidates; at least one is on the curve. Try them in order. */
    fe_mul(tmp, Y, Y);                      /* x = u + 4 Y^2 */
    fe_add(tmp, tmp, tmp);
    fe_add(tmp, tmp, tmp);
    fe_add(x, u, tmp);
    fe_curve_rhs(tmp, x);
    if (fe_sqrt(tmp, tmp))
        return;

    fe_small(half, 2);
    fe_inv(half, half);
    fe_inv(q, Y);                           /* q = X / Y */
    fe_mul(q, q, X);

    fe_neg(tmp, q);                         /* x = (-X/Y - u) / 2 */
    fe_sub(tmp, tmp, u);
    fe_mul(x, tmp, half);
    fe_curve_rhs(tmp, x);
    if (fe_sqrt(tmp, tmp))
        return;

    fe_sub(tmp, q, u);                      /* x = (X/Y - u) / 2 */
    fe_mul(x, tmp, half);
}

/* BIP324 XSwiftECInv: given x and u, find a t so that xswiftec(u, t) == x.
 * `which` (0-7) selects one of up to eight answers. Returns 0 if that one
 * does not exist. */
static uint8_t xswiftec_inv(fe t, const fe x, const fe u, uint8_t which)
{
    fe s, v, w, r, g, tmp, tmp2, c, half;

    fe_curve_rhs(g, u);                     /* g = u^3 + 7 */

    if (!(which & 2)) {
        fe_neg(tmp, x);                     /* fail if -x-u is on the curve */
        fe_sub(tmp, tmp, u);
        fe_curve_rhs(tmp, tmp);
        if (fe_sqrt(tmp, tmp))
            return 0;
        fe_copy(v, x);
        fe_mul(tmp, u, u);                  /* s = -(u^3+7) / (u^2 + uv + v^2) */
        fe_mul(tmp2, u, v);
        fe_add(tmp, tmp, tmp2);
        fe_mul(tmp2, v, v);
        fe_add(tmp, tmp, tmp2);
        fe_inv(tmp, tmp);
        fe_mul(s, g, tmp);
        fe_neg(s, s);
    } else {
        fe_sub(s, x, u);
        if (fe_is_zero(s))
            return 0;
        fe_add(tmp, g, g);                  /* r = sqrt(-s (4(u^3+7) + 3 s u^2)) */
        fe_add(tmp, tmp, tmp);
        fe_mul(tmp2, u, u);
        fe_mul(tmp2, tmp2, s);
        fe_add(tmp, tmp, tmp2);
        fe_add(tmp, tmp, tmp2);
        fe_add(tmp, tmp, tmp2);
        fe_mul(tmp, tmp, s);
        fe_neg(tmp, tmp);
        if (!fe_sqrt(r, tmp))
            return 0;
        if ((which & 1) && fe_is_zero(r))
            return 0;
        fe_inv(tmp, s);                     /* v = (-u + r/s) / 2 */
        fe_mul(tmp, tmp, r);
        fe_sub(tmp, tmp, u);
        fe_small(half, 2);
        fe_inv(half, half);
        fe_mul(v, tmp, half);
    }

    if (!fe_sqrt(w, s))
        return 0;

    /* t = +-w (u (1 -+ c) / 2 + v), signs chosen by `which`. */
    fe_from_bytes(c, SQRT_MINUS_3);
    fe_small(tmp, 1);
    if (which & 1)
        fe_add(tmp, tmp, c);
    else
        fe_sub(tmp, tmp, c);
    fe_small(half, 2);
    fe_inv(half, half);
    fe_mul(tmp, tmp, half);
    fe_mul(tmp, tmp, u);
    fe_add(tmp, tmp, v);
    fe_mul(t, tmp, w);
    if ((which & 5) == 0 || (which & 5) == 5)
        fe_neg(t, t);
    return 1;
}

/* Load a secret key or scalar; returns 0 unless 0 < k < n. */
static uint8_t scalar_load(fe k, const uint8_t b[32])
{
    uECC_vli_bytesToNative(k, b, 32);
    return !uECC_vli_isZero(k, NW) && uECC_vli_cmp(k, ORDER_N, NW) < 0;
}

uint8_t sv2_ellswift_create(const uint8_t priv[32], const uint8_t seed[32],
                            uint8_t ell[ELLSWIFT_BYTES])
{
    fe d, x, u, t;
    uECC_word_t point[2 * NW];

    if (!scalar_load(d, priv))
        return 0;
    uECC_point_mult(point, uECC_curve_G(CURVE), d, CURVE);     /* public key */
    fe_copy(x, point);

    /* Try (u, which) pairs derived from the seed until one has an answer.
     * About one try in four succeeds. */
    for (uint8_t attempt = 0; ; attempt++) {
        uECC_YIELD();
        sha256_ctx ctx;
        uint8_t hash[32];

        sha256_init(&ctx);
        sha256_update(&ctx, seed, 32);
        sha256_update(&ctx, &attempt, 1);
        sha256_final(&ctx, hash);
        fe_from_bytes(u, hash);
        if (fe_is_zero(u))
            continue;

        /* A second hash picks which of the eight answers to try. */
        sha256_init(&ctx);
        sha256_update(&ctx, hash, 32);
        sha256_final(&ctx, hash);
        if (xswiftec_inv(t, x, u, hash[0] & 7)) {
            fe_to_bytes(ell, u);
            fe_to_bytes(ell + 32, t);
            return 1;
        }
    }
}

void sv2_ellswift_decode(const uint8_t ell[ELLSWIFT_BYTES], uint8_t x_out[32])
{
    fe u, t, x;

    fe_from_bytes(u, ell);
    fe_from_bytes(t, ell + 32);
    xswiftec(x, u, t);
    fe_to_bytes(x_out, x);
}

/* ---- hashing helper: BIP340-style "tagged hash" ---- */

static void tagged_hash_start(sha256_ctx *ctx, const char *tag)
{
    uint8_t tag_hash[32];

    sha256_init(ctx);
    sha256_update(ctx, (const uint8_t *)tag, strlen(tag));
    sha256_final(ctx, tag_hash);
    sha256_init(ctx);
    sha256_update(ctx, tag_hash, 32);
    sha256_update(ctx, tag_hash, 32);
}

/* ---- key agreement ---- */

uint8_t sv2_ecdh(const uint8_t priv[32],
                 const uint8_t theirs[ELLSWIFT_BYTES],
                 const uint8_t ours[ELLSWIFT_BYTES],
                 uint8_t initiating, uint8_t secret[32])
{
    fe d, u, t;
    uECC_word_t point[2 * NW];
    uint8_t x_bytes[32];
    sha256_ctx ctx;

    if (!scalar_load(d, priv))
        return 0;

    /* Their public key as a full point. Either of the two possible y values
     * gives the same shared x. */
    fe_from_bytes(u, theirs);
    fe_from_bytes(t, theirs + 32);
    xswiftec(point, u, t);
    fe_curve_rhs(point + NW, point);
    if (!fe_sqrt(point + NW, point + NW))
        return 0;                           /* cannot happen: x is on the curve */

    uECC_point_mult(point, point, d, CURVE);
    fe_to_bytes(x_bytes, point);

    /* The secret also commits to both encodings, initiator's first. */
    tagged_hash_start(&ctx, "bip324_ellswift_xonly_ecdh");
    sha256_update(&ctx, initiating ? ours : theirs, ELLSWIFT_BYTES);
    sha256_update(&ctx, initiating ? theirs : ours, ELLSWIFT_BYTES);
    sha256_update(&ctx, x_bytes, 32);
    sha256_final(&ctx, secret);
    return 1;
}

/* ---- Schnorr signature verification ---- */

uint8_t sv2_schnorr_verify(const uint8_t pubkey[32], const uint8_t msg[32],
                           const uint8_t sig[64])
{
    fe r, s, e, z, rx, ry, tx, ty, tz;
    uECC_word_t P[2 * NW], GQ[2 * NW];
    const uECC_word_t *points[4];
    const uECC_word_t *point;
    bitcount_t bits, i;
    uint8_t hash[32];
    sha256_ctx ctx;

    /* P = the public key as a point with even y. Its x must be below p and
     * on the curve. */
    uECC_vli_bytesToNative(P, pubkey, 32);
    if (uECC_vli_cmp(P, FIELD_P, NW) >= 0)
        return 0;
    fe_curve_rhs(P + NW, P);
    if (!fe_sqrt(P + NW, P + NW))
        return 0;
    if (fe_is_odd(P + NW))
        fe_neg(P + NW, P + NW);

    /* The signature is (r, s): r below p, s below the curve order n. */
    uECC_vli_bytesToNative(r, sig, 32);
    if (uECC_vli_cmp(r, FIELD_P, NW) >= 0)
        return 0;
    uECC_vli_bytesToNative(s, sig + 32, 32);
    if (uECC_vli_cmp(s, ORDER_N, NW) >= 0)
        return 0;

    /* e = hash(r || pubkey || msg) modulo n. */
    tagged_hash_start(&ctx, "BIP0340/challenge");
    sha256_update(&ctx, sig, 32);
    sha256_update(&ctx, pubkey, 32);
    sha256_update(&ctx, msg, 32);
    sha256_final(&ctx, hash);
    uECC_vli_bytesToNative(e, hash, 32);
    if (uECC_vli_cmp(e, ORDER_N, NW) >= 0)
        uECC_vli_sub(e, e, ORDER_N, NW);

    /* R = s*G - e*P = s*G + e*Q with Q = -P, worked out in ONE pass over
     * the bits of s and e together ("Shamir's trick"; the same steps
     * micro-ecc uses for its own signature check). Going from the top bit
     * down: double the running point, then add G, Q or G+Q according to
     * whether this bit is set in s, in e, or in both. Two separate
     * multiplications would each do their own 256 doublings; this shares
     * them.
     *
     * The running point is kept as (rx, ry) over a common divisor z, which
     * is divided out once at the end. Every way the sums can go wrong (two
     * equal points, a point and its mirror image, "no point") shows up as
     * z = 0, which no honest signature produces, and is refused. */
    if (uECC_vli_isZero(s, NW) && uECC_vli_isZero(e, NW))
        return 0;                           /* R would be "no point" */
    fe_neg(P + NW, P + NW);                 /* P now holds Q = -P */

    uECC_vli_set(GQ, P, NW);                /* GQ = G + Q */
    uECC_vli_set(GQ + NW, P + NW, NW);
    uECC_vli_set(tx, uECC_curve_G(CURVE), NW);
    uECC_vli_set(ty, uECC_curve_G(CURVE) + NW, NW);
    fe_sub(z, GQ, tx);
    if (fe_is_zero(z))
        return 0;                           /* the key is G or -G: nobody's real key */
    XYcZ_add(tx, ty, GQ, GQ + NW, tx, CURVE);
    fe_inv(z, z);
    apply_z(GQ, GQ + NW, z, CURVE);

    points[0] = 0;
    points[1] = uECC_curve_G(CURVE);
    points[2] = P;
    points[3] = GQ;
    bits = smax(uECC_vli_numBits(s, NW), uECC_vli_numBits(e, NW));

    point = points[(!!uECC_vli_testBit(s, bits - 1)) | ((!!uECC_vli_testBit(e, bits - 1)) << 1)];
    uECC_vli_set(rx, point, NW);
    uECC_vli_set(ry, point + NW, NW);
    fe_small(z, 1);

    for (i = bits - 2; i >= 0; --i) {
        uECC_YIELD();
        CURVE->double_jacobian(rx, ry, z, CURVE);
        point = points[(!!uECC_vli_testBit(s, i)) | ((!!uECC_vli_testBit(e, i)) << 1)];
        if (point) {
            uECC_vli_set(tx, point, NW);
            uECC_vli_set(ty, point + NW, NW);
            apply_z(tx, ty, z, CURVE);
            fe_sub(tz, rx, tx);
            XYcZ_add(tx, ty, rx, ry, tx, CURVE);
            fe_mul(z, z, tz);
        }
    }
    if (fe_is_zero(z))
        return 0;
    fe_inv(z, z);
    apply_z(rx, ry, z, CURVE);

    /* Valid when R has even y and its x equals r. */
    return !fe_is_odd(ry) && fe_equal(rx, r);
}
