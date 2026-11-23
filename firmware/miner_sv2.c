/*
 * miner_sv2.c - the Stratum V2 side of the miner, in XBT's variant as the
 * DATUM gateway defines it. Everything else (start-up, console, lights,
 * hashing loop) is miner.c; miner_sv1.c is the same for SV1 pools.
 *
 * Build/flash:  make ota TARGET=miner_sv2   (the default TARGET)
 * PC test:      make sim   (this file and miner.c with a PC stand-in for
 *               the hardware, tests/sim/miner_pc.c)
 *
 * Life of a connection:
 *
 *   1. time      ask a time server what time it is (only if the certificate
 *                is checked; repeated every few hours)
 *   2. key       make a one-time key pair from fresh random numbers.
 *                4-12 seconds on the chip, so it is done BEFORE connecting:
 *                the server only waits 10 seconds for it.
 *   3. connect   TCP to the server
 *   4. handshake send our key (64 bytes), read the reply (234 bytes), work
 *                out the shared encryption keys and check the server's
 *                certificate. About 12 seconds on the chip.
 *   5. setup     SetupConnection, then OpenExtendedMiningChannel with our
 *                worker name; every message from here on is encrypted
 *   6. mine      the server sends jobs; we hash; a share is sent back
 *
 * If anything goes wrong, or the server closes the connection (a
 * pool-switching proxy does that on every rotation), it starts again from
 * step 1 or 2.
 *
 * Settings (type help): the wifi.* and pool.* ones, plus
 * sv2.authority_key=<key>  sv2.check_cert=0|1  time_server=a.b.c.d
 * (see settings.h).
 *
 * Console output:
 *   sv2: ...    progress and errors
 *   msg ...     a message received from the server
 *   job ...     a job's id (followed by miner.c's work / target lines)
 *
 * The console is also answered during the long calculations (making the
 * key, the handshake); a "save" typed then takes effect when the
 * calculation ends.
 */

#include <stdint.h>
#include <string.h>

#include "entropy.h"
#include "hashrate.h"
#include "miner.h"
#include "net.h"
#include "settings.h"
#include "sv2.h"
#include "sv2crypto.h"
#include "sv2curve.h"
#include "uart.h"
#include "uptime.h"

/* ---- sizes and limits ---- */

#define NOMINAL_HASHRATE  970.0f    /* what we tell the server to expect */

#define TIME_MAX_AGE      (6UL * 3600)  /* ask the time server again after this */
#define HANDSHAKE_TIMEOUT 30            /* seconds to wait for each reply while logging in */
#define SILENCE_TIMEOUT   600           /* seconds without any message while mining */
#define CONNECT_TIMEOUT   60            /* then restart the network chip and try again */

#define RX_MAX            320       /* largest sealed message we accept */
#define PAYLOAD_MAX       160       /* largest message we build */

/* ---- state ---- */

enum {
    NEED_KEY,       /* not connected; a one-time key must be made first */
    CONNECTING,     /* key ready; waiting for the TCP connection */
    WAIT_ACT2,      /* our key sent; waiting for the server's 234 bytes */
    RUNNING         /* encrypted messages flow */
};
static uint8_t  state = NEED_KEY;

static sv2_session session;
static sv2_work    sv2job;          /* the job as the server describes it */
static uint8_t  act1[SV2_ACT1_BYTES];

/* Receiving: gather `rx_need` bytes, then act on them. */
static uint8_t  rx[RX_MAX];
static uint16_t rx_have, rx_need;
static uint8_t  rx_is_payload;      /* 0 = gathering a header, 1 = a payload */
static uint8_t  rx_type;            /* the message type from the last header */
static uint16_t rx_payload_len;
static uint32_t last_heard;         /* when the server last sent something whole */

static uint32_t retry_at;           /* do not try again before this */
static uint32_t connecting_since;   /* when the current CONNECTING began */
static uint8_t  complained;         /* the "authority not set" hint was printed */
/* The certificate settings actually in use. They are copied from `settings`
 * at start-up and at "save", so a typed sv2.authority_key= or sv2.check_cert= changes
 * nothing until it is saved, like every other setting. */
static struct {
    uint8_t check_cert;
    uint8_t authority_set;
    uint8_t authority[32];
} in_use;

/* Mining. */
static const uint8_t our_extranonce[SV2_EXTRANONCE_MAX] = "megamine";
static uint8_t  have_job;
static uint32_t mining_job;         /* sv2job.job_id of the job being hashed */
static uint32_t share_sequence;

/* ---- small helpers ---- */

/* Progress and errors, text in flash: say_P(PSTR("...")). */
static void say_P(const char *flash_text)
{
    uart_puts_P(PSTR("sv2: "));
    uart_puts_P(flash_text);
    uart_puts_P(PSTR("\r\n"));
}

/* before and after are in flash: say_number(PSTR("a "), n, PSTR(" b")). */
static void say_number(const char *before, uint32_t n, const char *after)
{
    uart_puts_P(PSTR("sv2: "));
    uart_puts_P(before);
    uart_put_u32(n);
    uart_puts_P(after);
    uart_puts_P(PSTR("\r\n"));
}

/* Give up on this connection and start over. `hang_up` closes our side
 * (not wanted when the server has already closed it); `pause` waits a few
 * seconds before the next attempt and counts as a failure. */
#define HANG_UP 1
#define PAUSE   2
static void start_over(uint8_t how)
{
    uint8_t pause = how & PAUSE;

    if (how & HANG_UP)
        net_close();
    if (pause)
        miner_failed();                 /* a failure, not an ordinary close */
    miner_stop();
    have_job = 0;
    state = NEED_KEY;
    retry_at = platform_seconds() + (pause ? MINER_RETRY_SECONDS : 0);
}

static void send_message(uint8_t channel_msg, uint8_t type,
                         const uint8_t *payload, uint16_t len)
{
    uint8_t sealed[SV2_HEADER_SEALED + PAYLOAD_MAX + SV2_TAG_BYTES];
    uint16_t n = sv2_seal_frame(&session, channel_msg, type, payload, len, sealed);

    if (!net_send(sealed, n))
        say_P(PSTR("send failed"));
}

/* ---- the authority key setting ---- */

static int8_t hex_value(char c)
{
    if (c >= '0' && c <= '9') return (int8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (int8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int8_t)(c - 'A' + 10);
    return -1;
}

/* The key in the base58check form SV2 pools print: text that decodes to 38
 * bytes = 0x01 0x00, the 32-byte key, and 4 checksum bytes (the start of
 * SHA-256 of SHA-256 of the first 34). Base58 is a number written in base
 * 58, so decoding is "multiply what we have by 58 and add the next digit",
 * done on a 38-byte number. */
static uint8_t parse_base58_key(const char *text, uint8_t key[32])
{
    static const char alphabet[] =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    uint8_t number[38] = {0};
    uint8_t check[32];
    sha256_ctx ctx;

    for (; *text; text++) {
        const char *at = strchr(alphabet, *text);
        uint16_t carry;

        if (!at)
            return 0;
        carry = (uint16_t)(at - alphabet);
        for (int8_t i = sizeof number - 1; i >= 0; i--) {
            carry += (uint16_t)number[i] * 58;
            number[i] = (uint8_t)carry;
            carry >>= 8;
        }
        if (carry)
            return 0;                       /* too long */
    }
    if (number[0] != 0x01 || number[1] != 0x00)
        return 0;

    sha256_init(&ctx);
    sha256_update(&ctx, number, 34);
    sha256_final(&ctx, check);
    sha256_init(&ctx);
    sha256_update(&ctx, check, 32);
    sha256_final(&ctx, check);
    if (memcmp(check, number + 34, 4) != 0)
        return 0;

    memcpy(key, number + 2, 32);
    return 1;
}

/* "sv2.authority_key=<64 hex digits>" or "...=<base58 text>". */
static void set_authority(const char *text)
{
    uint8_t key[32];
    uint8_t ok = 0;

    if (strlen(text) == 64) {
        ok = 1;
        for (uint8_t i = 0; i < 32 && ok; i++) {
            int8_t hi = hex_value(text[2 * i]), lo = hex_value(text[2 * i + 1]);
            if (hi < 0 || lo < 0)
                ok = 0;
            else
                key[i] = (uint8_t)((hi << 4) | lo);
        }
    }
    if (!ok)
        ok = parse_base58_key(text, key);

    if (!ok) {
        say_P(PSTR("sv2.authority_key: not 64 hex digits and not a valid base58 key; nothing changed"));
        return;
    }
    memcpy(settings.authority, key, 32);
    settings.authority_set = 1;
    put_hex_line(PSTR("sv2: authority key is now (type save to store and use it) "), key, 32);
}

static void use_certificate_settings(void)
{
    in_use.check_cert = settings.check_cert;
    in_use.authority_set = settings.authority_set;
    memcpy(in_use.authority, settings.authority, 32);
}

/* ---- the hooks miner.c calls ---- */

void pool_start(void)
{
    sv2curve_idle = miner_while_busy;   /* the console keeps answering during the curve maths */
    use_certificate_settings();
}

uint8_t pool_command(const char *line)
{
    if (strncmp_P(line, PSTR("sv2.authority_key="), 18) != 0)
        return 0;
    set_authority(line + 18);
    return 1;
}

void pool_reset(uint8_t hang_up)
{
    use_certificate_settings();
    start_over(hang_up ? HANG_UP : 0);
    retry_at = 0;
    complained = 0;
}

void pool_submit(void)
{
    uint8_t payload[PAYLOAD_MAX];

    send_message(1, SV2_MSG_SUBMIT_SHARES_EXTENDED, payload,
                 sv2_build_submit(payload, sv2job.channel_id, share_sequence++, sv2job.job_id,
                                  job.work + MINER_NONCE_OFFSET, job.work + MINER_NTIME_OFFSET,
                                  our_extranonce, sv2job.extranonce_len));
}

/* ---- jobs ---- */

static void load_work(void)
{
    uint8_t leaf[40 + 2 * SV2_EXTRANONCE_MAX];
    uint8_t new_job = !have_job || sv2job.job_id != mining_job;

    miner_hash(leaf, sv2_root_input(&sv2job, our_extranonce, leaf), job.work + MINER_ROOT_OFFSET);
    memcpy(job.work, sv2job.prevhash, 32);
    memset(job.work + MINER_NONCE_OFFSET, 0, 8);
    memcpy(job.work + MINER_NTIME_OFFSET, sv2job.ntime, 8);
    memcpy(job.target, sv2job.target, MINER_DIGEST_BYTES);
    job.nbits = sv2job.nbits;
    have_job = 1;
    mining_job = sv2job.job_id;

    uart_puts_P(PSTR("job "));
    uart_put_u32(sv2job.job_id);
    uart_puts_P(PSTR("\r\n"));
    miner_job(new_job);
}

/* ---- messages from the server ---- */

static void handle_message(uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t out[PAYLOAD_MAX];
    uint8_t result;

    uart_puts_P(PSTR("msg 0x"));
    uart_put_hex(&type, 1);
    uart_putc(' ');
    uart_put_u32(len);
    uart_puts_P(PSTR(" bytes\r\n"));

    switch (type) {
    case SV2_MSG_SETUP_CONNECTION_SUCCESS:
        say_P(PSTR("setup accepted; opening a channel"));
        send_message(0, SV2_MSG_OPEN_EXTENDED_CHANNEL, out,
                     sv2_build_open_channel(out, settings.worker, NOMINAL_HASHRATE));
        return;

    case SV2_MSG_SETUP_CONNECTION_ERROR:
        say_P(PSTR("the server refused the connection setup"));
        start_over(HANG_UP | PAUSE);
        return;

    case SV2_MSG_OPEN_CHANNEL_ERROR:
        say_P(PSTR("the server refused the channel"));
        start_over(HANG_UP | PAUSE);
        return;

    case SV2_MSG_SUBMIT_SHARES_SUCCESS:
        say_P(PSTR("share ACCEPTED"));
        return;

    case SV2_MSG_SUBMIT_SHARES_ERROR:
        /* channel (4), sequence (4), then the reason as length + text */
        uart_puts_P(PSTR("sv2: share REJECTED: "));
        if (len >= 9 && 9u + payload[8] <= len) {
            for (uint8_t i = 0; i < payload[8]; i++) {
                char c = (char)payload[9 + i];
                uart_putc(c >= ' ' && c <= '~' ? c : '?');  /* server text: no control characters */
            }
        }
        uart_puts_P(PSTR("\r\n"));
        return;
    }

    result = sv2_handle_message(&sv2job, type, payload, len);
    if (result == 0) {
        say_P(PSTR("(that message was not understood; ignored)"));
    } else if (type == SV2_MSG_OPEN_EXTENDED_SUCCESS) {
        if (sv2job.extranonce_len > sizeof our_extranonce) {
            say_P(PSTR("the server wants a longer extranonce than we have"));
            start_over(HANG_UP | PAUSE);
            return;
        }
        say_number(PSTR("channel "), sv2job.channel_id, PSTR(" open; waiting for a job"));
    }
    if (result == 2 && sv2job.active)
        load_work();
}

/* Take what has arrived and act on each complete piece. */
static void receive(void)
{
    for (;;) {
        uint16_t got = net_recv(rx + rx_have, rx_need - rx_have);

        if (got == 0)
            return;
        rx_have += got;
        if (rx_have < rx_need)
            continue;
        rx_have = 0;
        last_heard = platform_seconds();

        if (state == WAIT_ACT2) {
            uint32_t began = platform_seconds();
            uint8_t result;

            say_P(PSTR("reply received; working out the keys (about 12 s on the chip)"));
            result = sv2_handshake_finish(&session, rx,
                                          in_use.check_cert ? in_use.authority : 0,
                                          in_use.check_cert ? clock_now() : 0);
            if (result != SV2_OK) {
                say_P(result == SV2_ERR_WINDOW
                      ? PSTR("handshake FAILED: the certificate is not valid at the current time")
                      : result == SV2_ERR_DECRYPT
                      ? PSTR("handshake FAILED: the reply did not decrypt (is this an SV2 port?)")
                      : result == SV2_ERR_SIGNATURE
                      ? PSTR("handshake FAILED: the certificate is not signed by our authority key")
                      : PSTR("handshake FAILED: our own key was unusable"));
                if (result == SV2_ERR_WINDOW)
                    clock_forget();         /* perhaps our clock is the wrong one */
                start_over(HANG_UP | PAUSE);
                return;
            }
            say_number(in_use.check_cert ? PSTR("handshake ok, certificate verified (")
                                           : PSTR("handshake ok, certificate NOT CHECKED ("),
                       platform_seconds() - began, PSTR(" s)"));

            state = RUNNING;
            rx_is_payload = 0;
            rx_need = SV2_HEADER_SEALED;
            last_heard = platform_seconds();
            {
                uint8_t out[PAYLOAD_MAX];
                send_message(0, SV2_MSG_SETUP_CONNECTION, out, sv2_build_setup_connection(out));
            }
            continue;
        }

        /* RUNNING: a header, then (if it announces one) a payload. */
        if (!rx_is_payload) {
            uint32_t payload_len;

            if (!sv2_open_header(&session, rx, &rx_type, &payload_len)) {
                say_P(PSTR("a message header did not decrypt; reconnecting"));
                start_over(HANG_UP | PAUSE);
                return;
            }
            if (payload_len + SV2_TAG_BYTES > RX_MAX) {
                say_number(PSTR("a message of "), payload_len, PSTR(" bytes is too large for us; reconnecting"));
                start_over(HANG_UP | PAUSE);
                return;
            }
            rx_payload_len = (uint16_t)payload_len;
            if (payload_len == 0) {
                handle_message(rx_type, rx, 0);
            } else {
                rx_is_payload = 1;
                rx_need = rx_payload_len + SV2_TAG_BYTES;
            }
        } else {
            if (!sv2_open_payload(&session, rx, rx_payload_len)) {
                say_P(PSTR("a message did not decrypt; reconnecting"));
                start_over(HANG_UP | PAUSE);
                return;
            }
            rx_is_payload = 0;
            rx_need = SV2_HEADER_SEALED;
            handle_message(rx_type, rx, rx_payload_len);
        }
        if (state != RUNNING)
            return;                         /* handle_message gave up on the connection */
    }
}

/* ---- the connection, step by step ---- */

static void make_key(void)
{
    uint8_t priv[32], seed[32];
    uint32_t began = platform_seconds();

    say_P(PSTR("making a one-time key (4-12 s on the chip)"));
    do {
        entropy_bytes(priv);
        entropy_bytes(seed);
    } while (!sv2_handshake_start(&session, priv, seed, act1));
    memset(priv, 0, sizeof priv);
    say_number(PSTR("key ready ("), platform_seconds() - began, PSTR(" s); connecting"));
}

void pool_service(void)
{
    uint32_t now = platform_seconds();

    switch (state) {
    case NEED_KEY:
        /* net_poll() is deliberately not called here: it is what opens the
         * connection, and the key has to exist first. */
        if ((int32_t)(now - retry_at) < 0)
            return;
        if (in_use.check_cert) {
            if (!in_use.authority_set) {
                miner_needs_you(1);
                if (!complained) {
                    complained = 1;
                    say_P(PSTR("no authority key set. Type sv2.authority_key=<key> then save, "
                        "or sv2.check_cert=0 then save to accept any server."));
                }
                return;
            }
            if (!clock_known() || clock_age() > TIME_MAX_AGE) {
                if (!net_link_up()) {
                    /* (net_link_up also asks the module to rejoin when due.) */
                    say_P(PSTR("no network link; waiting"));
                    retry_at = now + MINER_RETRY_SECONDS;
                    return;
                }
                if (clock_fetch()) {
                    say_number(PSTR("time server says "), clock_now(), PSTR(" seconds since 1970"));
                } else if (!clock_known()) {
                    say_P(PSTR("no answer from the time server (time_server setting); retrying"));
                    miner_failed();
                    retry_at = now + MINER_RETRY_SECONDS;
                    return;
                }   /* else: keep running on the older reading */
            }
        }
        miner_needs_you(0);
        make_key();
        state = CONNECTING;
        connecting_since = platform_seconds();
        return;

    case CONNECTING:
        net_poll();
        if (!net_connected()) {
            /* The network chip keeps retrying by itself, so this would
             * otherwise wait silently forever. After a minute, say what the
             * chip looks like and start it afresh (reset, addresses, DHCP). */
            if (platform_seconds() - connecting_since > CONNECT_TIMEOUT) {
                say_P(PSTR("could not connect for a minute; restarting the network chip"));
                net_report();
                if (!net_init())
                    say_P(PSTR("the network chip does not answer"));
                net_report();
                start_over(PAUSE);
            }
            return;
        }
        say_P(PSTR("connected; sending our key"));
        if (!net_send(act1, sizeof act1)) {
            /* Connected, yet the server never acknowledged our 64 bytes. */
            say_P(PSTR("could not send our key (no acknowledgement); starting over"));
            net_report();
            start_over(HANG_UP | PAUSE);
            return;
        }
        state = WAIT_ACT2;
        rx_have = 0;
        rx_need = SV2_ACT2_BYTES;
        last_heard = now;
        memset(&sv2job, 0, sizeof sv2job);
        return;

    case WAIT_ACT2:
    case RUNNING:
        net_poll();
        if (!net_connected()) {
            say_P(PSTR("connection lost"));
            start_over(0);          /* the server hung up; nothing to close */
            return;
        }
        receive();
        if (state == NEED_KEY)
            return;
        if (platform_seconds() - last_heard > (miner_hashing() ? SILENCE_TIMEOUT : HANDSHAKE_TIMEOUT)) {
            say_P(PSTR("the server has gone quiet; reconnecting"));
            start_over(HANG_UP | PAUSE);
        }
        return;
    }
}
