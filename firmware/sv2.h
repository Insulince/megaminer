/*
 * sv2.h - Stratum V2 for an XBT miner (XBT's variant, as the DATUM gateway
 * defines it):
 * the encrypted handshake, the encrypted frames, and the handful of mining
 * messages a miner needs.
 *
 * Nothing in here touches a network or a clock. The caller hands in bytes
 * that arrived and gets back bytes to send, so the same code runs in the PC
 * test client (sv2_pc_client.c) and on the chip.
 *
 * Order of use:
 *   1. sv2_handshake_start()  -> 64 bytes to send
 *   2. read 234 bytes         -> sv2_handshake_finish()
 *   3. after that, every message in either direction is a "frame":
 *        sending:   sv2_seal_frame()
 *        receiving: read 22 bytes -> sv2_open_header() tells the payload
 *                   length; read that many + 16 -> sv2_open_payload()
 */

#ifndef SV2_H
#define SV2_H

#include <stdint.h>

#define SV2_ACT1_BYTES      64
#define SV2_ACT2_BYTES      234
#define SV2_HEADER_SEALED   22      /* 6-byte frame header + 16-byte tag */
#define SV2_TAG_BYTES       16
#define SV2_EXTRANONCE_MAX  32

/* Results of sv2_handshake_finish. */
#define SV2_OK              0
#define SV2_ERR_KEY         1       /* our own secret key was unusable */
#define SV2_ERR_DECRYPT     2       /* the reply did not decrypt: not an SV2 server, or damaged */
#define SV2_ERR_SIGNATURE   3       /* certificate not signed by the expected authority key */
#define SV2_ERR_WINDOW      4       /* certificate not valid at the current time */

/* Message types. */
#define SV2_MSG_SETUP_CONNECTION          0x00
#define SV2_MSG_SETUP_CONNECTION_SUCCESS  0x01
#define SV2_MSG_SETUP_CONNECTION_ERROR    0x02
#define SV2_MSG_OPEN_CHANNEL_ERROR        0x12
#define SV2_MSG_OPEN_EXTENDED_CHANNEL     0x13
#define SV2_MSG_OPEN_EXTENDED_SUCCESS     0x14
#define SV2_MSG_SUBMIT_SHARES_EXTENDED    0x1b
#define SV2_MSG_SUBMIT_SHARES_SUCCESS     0x1c
#define SV2_MSG_SUBMIT_SHARES_ERROR       0x1d
#define SV2_MSG_NEW_EXTENDED_MINING_JOB   0x1f
#define SV2_MSG_SET_NEW_PREV_HASH         0x20
#define SV2_MSG_SET_TARGET                0x21

typedef struct {
    /* During the handshake. */
    uint8_t  priv[32];              /* our one-time secret key */
    uint8_t  ell[64];               /* its public encoding (what act 1 carries) */
    /* After the handshake: one key and one message counter per direction. */
    uint8_t  send_key[32];
    uint8_t  recv_key[32];
    uint64_t send_count;
    uint64_t recv_count;
} sv2_session;

/* ---- handshake ---- */

/* `priv` and `seed` are 32 fresh random bytes each. Writes the 64 bytes to
 * send. Returns 0 if priv is unusable (try again with new random bytes). */
uint8_t sv2_handshake_start(sv2_session *s, const uint8_t priv[32],
                            const uint8_t seed[32], uint8_t act1[SV2_ACT1_BYTES]);

/* Process the server's 234-byte reply.
 *   authority: the 32-byte public key the certificate must be signed with,
 *              or NULL to skip the certificate check entirely.
 *   now:       current time in seconds since 1970, or 0 to skip only the
 *              validity-window part of the check.
 * Returns SV2_OK or an SV2_ERR_ code. */
uint8_t sv2_handshake_finish(sv2_session *s, const uint8_t act2[SV2_ACT2_BYTES],
                             const uint8_t *authority, uint32_t now);

/* ---- frames ---- */

/* Encrypt one message into `out`; returns the number of bytes to send
 * (22, plus len + 16 if there is a payload). channel_msg is 1 for messages
 * that belong to a channel (submits), 0 otherwise. */
uint16_t sv2_seal_frame(sv2_session *s, uint8_t channel_msg, uint8_t msg_type,
                        const uint8_t *payload, uint16_t len, uint8_t *out);

/* Decrypt a 22-byte header. Returns 1 and the message type and payload
 * length on success, 0 if it does not decrypt (the connection is then dead). */
uint8_t sv2_open_header(sv2_session *s, const uint8_t sealed[SV2_HEADER_SEALED],
                        uint8_t *msg_type, uint32_t *payload_len);

/* Decrypt a payload in place: buf holds len + 16 bytes on entry and len
 * bytes of message on return. Returns 0 if it does not decrypt. */
uint8_t sv2_open_payload(sv2_session *s, uint8_t *buf, uint16_t len);

/* ---- messages we send (each returns the payload length written) ---- */

uint16_t sv2_build_setup_connection(uint8_t *out);
uint16_t sv2_build_open_channel(uint8_t *out, const char *user, float hash_rate);
uint16_t sv2_build_submit(uint8_t *out, uint32_t channel_id, uint32_t sequence,
                          uint32_t job_id, const uint8_t nonce[8],
                          const uint8_t ntime[8],
                          const uint8_t *extranonce, uint8_t extranonce_len);

/* ---- messages we receive ---- */

/* Everything needed to mine, gathered from several messages. */
typedef struct {
    uint32_t channel_id;
    uint8_t  prefix[SV2_EXTRANONCE_MAX];    /* the server's part of the extranonce */
    uint8_t  prefix_len;
    uint8_t  extranonce_len;                /* how many bytes are ours to choose */
    uint8_t  target[32];        /* most significant byte first, like the digest */

    uint32_t pending_job;       /* a job announced but not yet switched on */
    uint8_t  pending_root[32];
    uint8_t  have_pending;

    uint32_t job_id;            /* the job to mine now */
    uint8_t  root[32];          /* its 32-byte commitment */
    uint8_t  prevhash[32];
    uint8_t  ntime[8];          /* as it goes in the work block and the submit */
    uint32_t nbits;             /* as sent with the job; not used for mining */
    uint8_t  active;            /* 1 once there is a complete job */
} sv2_work;

/* Apply one received message to `w`. Returns:
 *   0  not a message this handles, or malformed (w unchanged)
 *   1  handled, nothing to restart
 *   2  handled, and the work to mine has changed (new job or new target) */
uint8_t sv2_handle_message(sv2_work *w, uint8_t msg_type,
                           const uint8_t *payload, uint16_t len);

/* The 52 bytes whose BLAKE2b-256 is the "root" in the 80-byte work block:
 * 0x00000000 || commitment || 0x00000000 || prefix || extranonce.
 * Returns the length (always 40 + prefix_len + extranonce_len). */
uint8_t sv2_root_input(const sv2_work *w, const uint8_t *extranonce, uint8_t *out);

#endif
