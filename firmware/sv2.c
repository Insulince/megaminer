/*
 * sv2.c - see sv2.h.
 *
 * Wire formats: the Stratum V2 specification (Noise handshake, framing,
 * common and mining messages), with XBT's changes to the job, prevhash and
 * share messages as the DATUM gateway defines them. Numbers on the wire are
 * little-endian.
 */

#include "sv2.h"
#include "version.h"

/* SetupConnection carries it; miner_sv2.c's message buffer has room for this. */
_Static_assert(sizeof(PROGRAM_NAME " " FIRMWARE_VERSION) <= 64, "version string too long");

#include <string.h>

#include "sv2crypto.h"
#include "sv2curve.h"

/* ======================================================================== */
/* Handshake (Noise NX)                                                     */
/* ======================================================================== */

static const char PROTOCOL_NAME[] = "Noise_NX_Secp256k1+EllSwift_ChaChaPoly_SHA256";

/* Layout of the server's 234-byte reply. */
#define ACT2_STATIC_AT  64      /* 64-byte key + 16-byte tag */
#define ACT2_CERT_AT    144     /* 74-byte certificate + 16-byte tag */
#define CERT_BYTES      74
#define CERT_LEEWAY     10      /* seconds of clock disagreement allowed */

/* The running state of the handshake: a chaining key that every shared
 * secret is folded into, and a hash of everything said so far. */
struct handshake {
    uint8_t ck[32];
    uint8_t h[32];
    uint8_t key[32];            /* current encryption key */
};

/* h = SHA-256(h || data) */
static void mix_hash(struct handshake *hs, const uint8_t *data, uint16_t len)
{
    sha256_ctx ctx;

    sha256_init(&ctx);
    sha256_update(&ctx, hs->h, 32);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, hs->h);
}

/* Fold a shared secret in; this also replaces the encryption key. */
static void mix_key(struct handshake *hs, const uint8_t secret[32])
{
    noise_hkdf2(hs->ck, secret, 32, hs->ck, hs->key);
}

/* Decrypt a handshake field (always the first message under its key, so the
 * counter is 0) and add the ciphertext to the running hash. */
static uint8_t decrypt_and_hash(struct handshake *hs, const uint8_t *sealed,
                                uint16_t len, uint8_t *out)
{
    if (!aead_open(hs->key, 0, hs->h, 32, sealed, len, out))
        return 0;
    mix_hash(hs, sealed, len + SV2_TAG_BYTES);
    return 1;
}

static uint32_t load32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint8_t sv2_handshake_start(sv2_session *s, const uint8_t priv[32],
                            const uint8_t seed[32], uint8_t act1[SV2_ACT1_BYTES])
{
    memset(s, 0, sizeof *s);
    if (!sv2_ellswift_create(priv, seed, s->ell))
        return 0;
    memcpy(s->priv, priv, 32);
    memcpy(act1, s->ell, SV2_ACT1_BYTES);
    return 1;
}

uint8_t sv2_handshake_finish(sv2_session *s, const uint8_t act2[SV2_ACT2_BYTES],
                             const uint8_t *authority, uint32_t now)
{
    struct handshake hs;
    sha256_ctx ctx;
    uint8_t secret[32];
    uint8_t server_static[64];
    uint8_t cert[CERT_BYTES];

    /* Start: ck = SHA-256(protocol name), h = SHA-256(ck). */
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)PROTOCOL_NAME, sizeof PROTOCOL_NAME - 1);
    sha256_final(&ctx, hs.ck);
    sha256_init(&ctx);
    sha256_update(&ctx, hs.ck, 32);
    sha256_final(&ctx, hs.h);

    /* Act 1 as both sides saw it: our key, then an empty payload. */
    mix_hash(&hs, s->ell, 64);
    mix_hash(&hs, NULL, 0);

    /* Act 2, part 1: the server's one-time key, in the clear. */
    mix_hash(&hs, act2, 64);
    if (!sv2_ecdh(s->priv, act2, s->ell, 1, secret))
        return SV2_ERR_KEY;
    mix_key(&hs, secret);

    /* Part 2: the server's long-term key, encrypted. */
    if (!decrypt_and_hash(&hs, act2 + ACT2_STATIC_AT, 64, server_static))
        return SV2_ERR_DECRYPT;
    if (!sv2_ecdh(s->priv, server_static, s->ell, 1, secret))
        return SV2_ERR_KEY;
    mix_key(&hs, secret);

    /* Part 3: the certificate, encrypted. */
    if (!decrypt_and_hash(&hs, act2 + ACT2_CERT_AT, CERT_BYTES, cert))
        return SV2_ERR_DECRYPT;

    if (authority) {
        /* Certificate: version (2), valid-from (4), not-valid-after (4),
         * signature (64). The signature is over
         * SHA-256(those first 10 bytes || the server's long-term key). */
        uint8_t static_x[32], digest[32];

        if (now) {
            uint32_t from = load32(cert + 2), until = load32(cert + 6);
            if ((from > CERT_LEEWAY && from - CERT_LEEWAY > now) ||
                (until < 0xffffffffUL - CERT_LEEWAY && until + CERT_LEEWAY < now))
                return SV2_ERR_WINDOW;
        }
        sv2_ellswift_decode(server_static, static_x);
        sha256_init(&ctx);
        sha256_update(&ctx, cert, 10);
        sha256_update(&ctx, static_x, 32);
        sha256_final(&ctx, digest);
        if (!sv2_schnorr_verify(authority, digest, cert + 10))
            return SV2_ERR_SIGNATURE;
    }

    /* Final keys: first for miner -> server, second for server -> miner. */
    noise_hkdf2(hs.ck, NULL, 0, s->send_key, s->recv_key);
    s->send_count = 0;
    s->recv_count = 0;
    memset(s->priv, 0, sizeof s->priv);     /* the one-time key is finished with */
    return SV2_OK;
}

/* ======================================================================== */
/* Frames                                                                   */
/* ======================================================================== */

uint16_t sv2_seal_frame(sv2_session *s, uint8_t channel_msg, uint8_t msg_type,
                        const uint8_t *payload, uint16_t len, uint8_t *out)
{
    uint8_t header[6];

    /* extension type (2 bytes; the top bit marks a channel message),
     * message type (1), payload length (3). */
    header[0] = 0;
    header[1] = channel_msg ? 0x80 : 0x00;
    header[2] = msg_type;
    header[3] = (uint8_t)len;
    header[4] = (uint8_t)(len >> 8);
    header[5] = 0;

    aead_seal(s->send_key, s->send_count++, NULL, 0, header, 6, out);
    if (len == 0)
        return SV2_HEADER_SEALED;
    aead_seal(s->send_key, s->send_count++, NULL, 0, payload, len,
              out + SV2_HEADER_SEALED);
    return SV2_HEADER_SEALED + len + SV2_TAG_BYTES;
}

uint8_t sv2_open_header(sv2_session *s, const uint8_t sealed[SV2_HEADER_SEALED],
                        uint8_t *msg_type, uint32_t *payload_len)
{
    uint8_t header[6];

    if (!aead_open(s->recv_key, s->recv_count, NULL, 0, sealed, 6, header))
        return 0;
    s->recv_count++;
    *msg_type = header[2];
    *payload_len = (uint32_t)header[3] | (uint32_t)header[4] << 8 |
                   (uint32_t)header[5] << 16;
    return 1;
}

uint8_t sv2_open_payload(sv2_session *s, uint8_t *buf, uint16_t len)
{
    if (!aead_open(s->recv_key, s->recv_count, NULL, 0, buf, len, buf))
        return 0;
    s->recv_count++;
    return 1;
}

/* ======================================================================== */
/* Building messages                                                        */
/* ======================================================================== */

static uint8_t *put16(uint8_t *p, uint16_t v)
{
    *p++ = (uint8_t)v;
    *p++ = (uint8_t)(v >> 8);
    return p;
}

static uint8_t *put32(uint8_t *p, uint32_t v)
{
    p = put16(p, (uint16_t)v);
    return put16(p, (uint16_t)(v >> 16));
}

/* A string: one length byte, then the characters. */
static uint8_t *put_str(uint8_t *p, const char *text)
{
    uint8_t len = (uint8_t)strlen(text);

    *p++ = len;
    memcpy(p, text, len);
    return p + len;
}

uint16_t sv2_build_setup_connection(uint8_t *out)
{
    uint8_t *p = out;

    *p++ = 0;                       /* protocol 0 = mining */
    p = put16(p, 2);                /* lowest version we speak */
    p = put16(p, 2);                /* highest version we speak */
    p = put32(p, 0);                /* no optional features */
    p = put_str(p, "");             /* host and port we dialled: not used */
    p = put16(p, 0);
    p = put_str(p, "megaminer");   /* vendor */
    p = put_str(p, "ATmega1284P");  /* hardware */
    p = put_str(p, PROGRAM_NAME " " FIRMWARE_VERSION);   /* firmware */
    p = put_str(p, "");             /* device id */
    return (uint16_t)(p - out);
}

uint16_t sv2_build_open_channel(uint8_t *out, const char *user, float hash_rate)
{
    uint8_t *p = out;

    p = put32(p, 1);                /* request id, echoed in the reply */
    p = put_str(p, user);
    memcpy(p, &hash_rate, 4);       /* 32-bit float; both our machines store
                                     * it low byte first, as the wire wants */
    p += 4;
    memset(p, 0xff, 32);            /* easiest target we would accept: any */
    p += 32;
    p = put16(p, 0);                /* smallest extranonce we need */
    return (uint16_t)(p - out);
}

uint16_t sv2_build_submit(uint8_t *out, uint32_t channel_id, uint32_t sequence,
                          uint32_t job_id, const uint8_t nonce[8],
                          const uint8_t ntime[8],
                          const uint8_t *extranonce, uint8_t extranonce_len)
{
    uint8_t *p = out;

    p = put32(p, channel_id);
    p = put32(p, sequence);
    p = put32(p, job_id);
    memcpy(p, nonce, 8);            /* the 8 work-block bytes, as they are */
    p += 8;
    memcpy(p, ntime, 8);
    p += 8;
    *p++ = extranonce_len;
    memcpy(p, extranonce, extranonce_len);
    p += extranonce_len;
    return (uint16_t)(p - out);
}

/* ======================================================================== */
/* Reading messages                                                         */
/* ======================================================================== */

/* Targets travel least significant byte first; the digest comparison wants
 * the most significant first. */
static void store_target(sv2_work *w, const uint8_t *wire)
{
    for (uint8_t i = 0; i < 32; i++)
        w->target[i] = wire[31 - i];
}

uint8_t sv2_handle_message(sv2_work *w, uint8_t msg_type,
                           const uint8_t *p, uint16_t len)
{
    switch (msg_type) {
    case SV2_MSG_OPEN_EXTENDED_SUCCESS: {
        /* request id (4), channel id (4), target (32), extranonce size (2),
         * prefix (1 length byte + bytes), group channel id (4) */
        uint8_t prefix_len;

        if (len < 43)
            return 0;
        prefix_len = p[42];
        if (prefix_len > SV2_EXTRANONCE_MAX || len < 43u + prefix_len ||
            p[41] != 0 || p[40] > SV2_EXTRANONCE_MAX)
            return 0;
        w->channel_id = load32(p + 4);
        store_target(w, p + 8);
        w->extranonce_len = p[40];
        w->prefix_len = prefix_len;
        memcpy(w->prefix, p + 43, prefix_len);
        return 1;
    }

    case SV2_MSG_NEW_EXTENDED_MINING_JOB: {
        /* channel id (4), job id (4), a flag byte that is 1 if a start time
         * (8) follows, then the 32-byte commitment. With the flag at 0 the
         * job waits for a SetNewPrevHash naming it. */
        uint8_t has_time;
        const uint8_t *root;

        if (len < 9)
            return 0;
        has_time = p[8];
        if (len != (has_time ? 49u : 41u))
            return 0;
        root = p + (has_time ? 17 : 9);
        if (has_time && w->active) {
            /* Usable straight away with the previous hash we already have. */
            w->job_id = load32(p + 4);
            memcpy(w->root, root, 32);
            memcpy(w->ntime, p + 9, 8);
            return 2;
        }
        w->pending_job = load32(p + 4);
        memcpy(w->pending_root, root, 32);
        w->have_pending = 1;
        return 1;
    }

    case SV2_MSG_SET_NEW_PREV_HASH:
        /* channel id (4), job id (4), previous hash (32), time (8), bits (4) */
        if (len != 52)
            return 0;
        if (!w->have_pending || load32(p + 4) != w->pending_job)
            return 1;                       /* names a job we were never given */
        w->job_id = w->pending_job;
        memcpy(w->root, w->pending_root, 32);
        memcpy(w->prevhash, p + 8, 32);
        memcpy(w->ntime, p + 40, 8);
        w->nbits = load32(p + 48);
        w->have_pending = 0;
        w->active = 1;
        return 2;

    case SV2_MSG_SET_TARGET: {
        /* channel id (4), target (32). Sent with every job, usually
         * unchanged; only a real change is worth restarting for. */
        uint8_t old[32];

        if (len != 36)
            return 0;
        memcpy(old, w->target, 32);
        store_target(w, p + 4);
        return (w->active && memcmp(old, w->target, 32) != 0) ? 2 : 1;
    }

    default:
        return 0;
    }
}

uint8_t sv2_root_input(const sv2_work *w, const uint8_t *extranonce, uint8_t *out)
{
    memset(out, 0, 4);
    memcpy(out + 4, w->root, 32);
    memset(out + 36, 0, 4);
    memcpy(out + 40, w->prefix, w->prefix_len);
    memcpy(out + 40 + w->prefix_len, extranonce, w->extranonce_len);
    return (uint8_t)(40 + w->prefix_len + w->extranonce_len);
}
