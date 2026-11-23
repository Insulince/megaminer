/*
 * miner_sv1.c - the Stratum V1 side of the miner, for pools that speak SV1
 * (such as a DATUM gateway's SV1 port). Everything else (start-up,
 * console, lights, hashing loop) is miner.c, shared with miner_sv2.c.
 *
 * Build/flash:  make ota TARGET=miner_sv1
 *
 * What happens on the wire (one JSON message per line, both directions):
 *
 *   chip -> pool   mining.subscribe          "hello"
 *   pool -> chip   reply with extranonce1    bytes that identify this
 *                                            connection
 *   chip -> pool   mining.authorize          our user (worker) name
 *   pool -> chip   mining.set_difficulty     the share difficulty, for the
 *                                            jobs that follow
 *   pool -> chip   mining.notify             a job; sent again whenever
 *                                            the work changes
 *   chip -> pool   mining.submit             only if a share is found
 *
 * Turning a job into the 80 bytes we hash (as a DATUM gateway's SV1 port
 * hands it out):
 *
 *   root = BLAKE2b-256( 0x00 || coinb1(39) || extranonce1 || extranonce2 )
 *   work = prevhash(32) || nonce(8) || ntime(8) || root(32)
 *
 * prevhash, coinb1 and ntime come from the job and are used unchanged.
 * extranonce2 is ours to choose. A digest is a share when its first 4 bytes
 * are zero and it is at or below the share target: the one set_difficulty
 * gives (see "share difficulty" below), or, from a pool that never sends
 * one, the job's nbits. (The DATUM gateway puts the share target in nbits
 * too; a proxy may pass on the network's there instead, which only a block
 * would meet, so set_difficulty comes first.)
 *
 * This is not a JSON parser. The gateway's messages have a fixed shape, so
 * the code looks for known markers and takes the quoted strings that follow
 * in order. A message that does not fit is logged and ignored.
 *
 * Console output:
 *   < ...     a line received from the pool
 *   > ...     a line sent to the pool
 *   sv1: ...  progress and errors
 *   (and miner.c's work / target lines: tools/check_sv1_work.py re-derives
 *   the work block on the PC from the "<" line before it)
 */

#include <stdint.h>
#include <string.h>

#include "hashrate.h"
#include "miner.h"
#include "net.h"
#include "settings.h"
#include "uart.h"
#include "uptime.h"
#include "version.h"

#define USER_AGENT       "megaminer/" FIRMWARE_VERSION

#define COINB1_BYTES     39
#define JOB_ID_MAX       40
#define CONNECT_TIMEOUT  60             /* then restart the network chip and try again */
#define TIME_MAX_AGE     (6UL * 3600)   /* ask the time server again after this (log stamps) */

/* The server gives us extranonce1 and tells us how many bytes of extranonce2
 * we may choose. A gateway gives 4 + 8; a proxy that shares one gateway
 * session between several miners may give 5 + 7. Either way the two together
 * must be 12 bytes, because the hashed "leaf" has a fixed size. */
#define EXTRANONCE_BYTES 12
static const uint8_t extranonce2_source[8] = { 'm', 'e', 'g', 'a', 'm', 'i', 'n', 'e' };

static uint8_t  extranonce1[EXTRANONCE_BYTES];
static uint8_t  extranonce1_len;
static uint8_t  extranonce2_len;
static uint8_t  have_extranonce;            /* 1 once both are known and add up to 12 */

/* The current job (its work block and target are miner.c's `job`). */
static char     job_id[JOB_ID_MAX + 1];
static char     job_ntime[17];              /* as sent; echoed in a submit */
static uint8_t  submit_id = 10;             /* ids 1 and 2 are the login messages */

/* Line buffers. A job message is about 270 characters. */
#define RX_MAX 400
static char     rx_line[RX_MAX + 1];
static uint16_t rx_len;
static uint8_t  rx_overflow;
static char     tx_line[256];
static uint16_t tx_len;

static uint8_t  was_connected;
static uint8_t  had_job;                    /* a job arrived on this connection */
static uint32_t waiting_since;              /* when we began waiting for a connection */
static uint8_t  waiting;
static uint32_t next_time_fetch;

/* ---- building and sending a line ---------------------------------------- */

static const char hex_digits[] = "0123456789abcdef";

static void out_char(char c)
{
    if (tx_len < sizeof tx_line - 1)        /* keep room for the newline */
        tx_line[tx_len++] = c;
}

static void out_str(const char *s)
{
    while (*s)
        out_char(*s++);
}

static void out_u32(uint32_t n)
{
    char digits[10];
    uint8_t count = 0;

    do {
        digits[count++] = '0' + n % 10;
        n /= 10;
    } while (n);
    while (count)
        out_char(digits[--count]);
}

static void out_hex(const uint8_t *bytes, uint8_t len)
{
    while (len--) {
        out_char(hex_digits[*bytes >> 4]);
        out_char(hex_digits[*bytes & 0x0f]);
        bytes++;
    }
}

/* Send the line built so far to the pool, and copy it to the console. */
static void out_send(void)
{
    tx_line[tx_len] = '\0';
    uart_puts_P(PSTR("> "));
    uart_puts(tx_line);
    uart_puts_P(PSTR("\r\n"));

    tx_line[tx_len++] = '\n';
    if (net_connected() && !net_send((const uint8_t *)tx_line, tx_len))
        uart_puts_P(PSTR("sv1: send failed\r\n"));
    tx_len = 0;
}

/* ---- picking fields out of a message ------------------------------------ */

static int8_t hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Convert exactly 2*n hex digits to n bytes. Returns 1 on success. */
static uint8_t parse_hex(const char *hex, uint16_t hex_len, uint8_t *out, uint8_t n)
{
    if (hex_len != (uint16_t)n * 2)
        return 0;
    for (uint8_t i = 0; i < n; i++) {
        int8_t hi = hex_value(hex[2 * i]);
        int8_t lo = hex_value(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

/* Find the next "quoted string" at or after *p. On success, sets *start and
 * *len to its contents (without the quotes), moves *p past it and returns 1. */
static uint8_t next_string(const char **p, const char **start, uint16_t *len)
{
    const char *open = strchr(*p, '"');
    if (!open)
        return 0;
    const char *close = strchr(open + 1, '"');
    if (!close)
        return 0;
    *start = open + 1;
    *len = (uint16_t)(close - open - 1);
    *p = close + 1;
    return 1;
}

/* ---- jobs --------------------------------------------------------------- */

/* The DATUM gateway's floor: a share's first 4 digest bytes must be zero.
 * An easier target is lowered to that. */
static void apply_floor(uint8_t target[MINER_DIGEST_BYTES])
{
    if (target[0] || target[1] || target[2] || target[3]) {
        memset(target, 0x00, 4);
        memset(target + 4, 0xff, MINER_DIGEST_BYTES - 4);
    }
}

/* Expand a compact "nbits" value into a 32-byte target, most significant
 * byte first (the same order as the digest). Used only when the pool has
 * sent no mining.set_difficulty (see below). */
static void target_from_nbits(const uint8_t nbits[4], uint8_t target[MINER_DIGEST_BYTES])
{
    uint8_t size = nbits[0];                /* how many bytes long the number is */

    memset(target, 0, MINER_DIGEST_BYTES);
    for (uint8_t i = 0; i < 3; i++) {       /* its top three bytes */
        int16_t pos = MINER_DIGEST_BYTES - (int16_t)size + i;
        if (pos >= 0 && pos < MINER_DIGEST_BYTES)
            target[pos] = nbits[1 + i];
    }
    if (size > MINER_DIGEST_BYTES)
        target[0] = 0xff;                   /* far too easy: the floor below takes over */
    apply_floor(target);
}

/* ---- share difficulty ----------------------------------------------------
 *
 * mining.set_difficulty gives the share difficulty D as a JSON number
 * ("1024", "4095.9375", "0.5", "1e-3"), and the share target is the
 * standard
 *
 *     target = diff1 / D,   diff1 = 0xffff * 2^208
 *
 * (The DATUM gateway's values look odd, 4095.9375 for its 4096, because it
 * converts its own power-of-two difficulty into this scale; the result is
 * exactly its 2^224 / 4096.) No floating point: D is read as an integer M
 * and a power of ten, D = M * 10^e, and the division is done on a 40-byte
 * number. Dividing in steps rounds exactly like one big division, because
 * floor(floor(a / b) / c) = floor(a / (b * c)) for positive integers.
 *
 * A difficulty applies from the next job on (as SV1 pools expect). */
#define BIG_BYTES 40                        /* 320 bits: room for diff1 * 10^19 */

static uint8_t  diff_target[MINER_DIGEST_BYTES];
static uint8_t  have_diff;                  /* 1 once this connection sent one */

/* n = n * m, most significant byte first. Returns the carry out (0 = fits). */
static uint8_t big_mul_small(uint8_t *n, uint8_t m)
{
    uint16_t carry = 0;

    for (int8_t i = BIG_BYTES - 1; i >= 0; i--) {
        carry += (uint16_t)n[i] * m;
        n[i] = (uint8_t)carry;
        carry >>= 8;
    }
    return (uint8_t)carry;
}

/* n = floor(n / d), most significant byte first. d must not be 0. */
static void big_div(uint8_t *n, uint32_t d)
{
    uint64_t rest = 0;                      /* always < d, so rest * 256 + 255 fits */

    for (uint8_t i = 0; i < BIG_BYTES; i++) {
        rest = rest << 8 | n[i];
        n[i] = (uint8_t)(rest / d);
        rest %= d;
    }
}

/* Parse the number after "params":[ and turn it into diff_target.
 * Returns 0 (diff_target unchanged) if it is not a usable number. */
static uint8_t load_difficulty(const char *p)
{
    uint32_t mant = 0;                      /* D = mant * 10^exp10 */
    int16_t  exp10 = 0;
    uint8_t  digits = 0, after_point = 0;
    uint8_t  n[BIG_BYTES];

    for (; ; p++) {
        if (*p >= '0' && *p <= '9') {
            if (mant < 429496729UL) {       /* keep what fits in 32 bits (9-10 digits); */
                                            /* a dropped digit moves the target by < 1e-8 */
                mant = mant * 10 + (uint8_t)(*p - '0');
                if (after_point)
                    exp10--;
            } else if (!after_point) {
                exp10++;                    /* a digit we cannot keep: still a power of ten */
            }
            digits++;
        } else if (*p == '.' && !after_point) {
            after_point = 1;
        } else {
            break;
        }
    }
    if (!digits || !mant)
        return 0;                           /* not a number, or zero */
    if (*p == 'e' || *p == 'E') {
        int8_t sign = 1;
        int16_t e = 0;

        p++;
        if (*p == '+' || *p == '-')
            sign = (*p++ == '-') ? -1 : 1;
        if (*p < '0' || *p > '9')
            return 0;
        while (*p >= '0' && *p <= '9' && e < 1000)
            e = (int16_t)(e * 10 + (*p++ - '0'));
        exp10 = (int16_t)(exp10 + sign * e);
    }

    /* n = diff1 = 0xffff * 2^208: bytes 0xff 0xff at 26 bytes from the end. */
    memset(n, 0, sizeof n);
    n[BIG_BYTES - 28] = 0xff;
    n[BIG_BYTES - 27] = 0xff;
    for (; exp10 < 0; exp10++)              /* D below 1: the target grows */
        if (big_mul_small(n, 10)) {
            n[0] = 0xff;                    /* absurdly easy: the floor below takes over */
            break;
        }
    for (; exp10 > 0 && exp10 < 80; exp10--)
        big_div(n, 10);
    if (exp10 >= 80)
        memset(n, 0, sizeof n);             /* absurdly hard: no hash meets it */
    big_div(n, mant);

    for (uint8_t i = 0; i < BIG_BYTES - MINER_DIGEST_BYTES; i++)
        if (n[i])
            n[BIG_BYTES - MINER_DIGEST_BYTES] = 0xff;   /* bigger than 256 bits: floor */
    memcpy(diff_target, n + BIG_BYTES - MINER_DIGEST_BYTES, MINER_DIGEST_BYTES);
    apply_floor(diff_target);
    return 1;
}

/* Handle a mining.notify. `params` points just after "params":[ .
 * Strings in order: job id, prevhash, coinb1, coinb2 (empty), version,
 * nbits, ntime. Everything is parsed into temporaries first, so a malformed
 * message cannot damage the job being mined. */
static uint8_t load_job(const char *params)
{
    const char *s;
    uint16_t len;
    char     new_id[JOB_ID_MAX + 1];
    uint8_t  prevhash[32], leaf[1 + COINB1_BYTES + 12], nbits[4], ntime[8];
    uint8_t  dummy[4];

    if (!have_extranonce)
        return 0;

    if (!next_string(&params, &s, &len) || len == 0 || len > JOB_ID_MAX)
        return 0;
    memcpy(new_id, s, len);
    new_id[len] = '\0';

    if (!next_string(&params, &s, &len) || !parse_hex(s, len, prevhash, 32))
        return 0;

    leaf[0] = 0x00;
    if (!next_string(&params, &s, &len) || !parse_hex(s, len, leaf + 1, COINB1_BYTES))
        return 0;
    memcpy(leaf + 1 + COINB1_BYTES, extranonce1, extranonce1_len);
    memcpy(leaf + 1 + COINB1_BYTES + extranonce1_len, extranonce2_source, extranonce2_len);

    if (!next_string(&params, &s, &len) || len != 0)            /* coinb2: empty */
        return 0;
    if (!next_string(&params, &s, &len) || !parse_hex(s, len, dummy, 4))    /* version */
        return 0;
    if (!next_string(&params, &s, &len) || !parse_hex(s, len, nbits, 4))
        return 0;
    if (!next_string(&params, &s, &len) || !parse_hex(s, len, ntime, 8))
        return 0;
    memcpy(job_ntime, s, 16);
    job_ntime[16] = '\0';

    /* All good: build the work block. */
    strcpy(job_id, new_id);
    memcpy(job.work, prevhash, 32);
    memset(job.work + MINER_NONCE_OFFSET, 0, 8);
    memcpy(job.work + MINER_NTIME_OFFSET, ntime, 8);
    miner_hash(leaf, sizeof leaf, job.work + MINER_ROOT_OFFSET);
    if (have_diff)
        memcpy(job.target, diff_target, MINER_DIGEST_BYTES);
    else
        target_from_nbits(nbits, job.target);
    job.nbits = (uint32_t)nbits[0] << 24 | (uint32_t)nbits[1] << 16 |
                (uint32_t)nbits[2] << 8 | nbits[3];
    return 1;
}

void pool_submit(void)
{
    out_str("{\"id\":");
    out_u32(submit_id++);
    out_str(",\"method\":\"mining.submit\",\"params\":[\"");
    out_str(settings.worker);
    out_str("\",\"");
    out_str(job_id);
    out_str("\",\"");
    out_hex(extranonce2_source, extranonce2_len);
    out_str("\",\"");
    out_str(job_ntime);
    out_str("\",\"");
    out_hex(job.work + MINER_NONCE_OFFSET, 8);  /* the nonce bytes exactly as hashed */
    out_str("\"]}");
    out_send();
}

/* ---- messages from the pool --------------------------------------------- */

static void handle_line(void)
{
    const char *p;

    rx_line[rx_len] = '\0';
    uart_puts_P(PSTR("< "));
    uart_puts(rx_line);
    uart_puts_P(PSTR("\r\n"));

    if ((p = strstr(rx_line, "\"method\":\"mining.notify\"")) != 0 &&
        (p = strstr(p, "\"params\":[")) != 0) {
        if (!load_job(p + 10)) {
            uart_puts_P(PSTR("sv1: could not use that job\r\n"));
            return;
        }
        had_job = 1;
        miner_job(1);                       /* every notify is a fresh job */
        return;
    }

    if ((p = strstr(rx_line, "\"method\":\"mining.set_difficulty\"")) != 0 &&
        (p = strstr(p, "\"params\":[")) != 0) {
        if (load_difficulty(p + 10))
            have_diff = 1;
        else
            uart_puts_P(PSTR("sv1: could not use that difficulty\r\n"));
        return;
    }

    /* Subscribe reply: {"error":null,"id":1,"result":[[[...],[...]],"<extranonce1>",8]} */
    if ((p = strstr(rx_line, "\"result\":[[")) != 0 && (p = strstr(p, "]],")) != 0) {
        const char *s;
        uint16_t len;
        uint8_t size = 0;

        /* "<extranonce1 hex>",<extranonce2 size>] */
        have_extranonce = 0;
        if (!next_string(&p, &s, &len) || len == 0 || (len & 1) || len > 2 * EXTRANONCE_BYTES ||
            !parse_hex(s, len, extranonce1, (uint8_t)(len / 2)) || *p != ',') {
            uart_puts_P(PSTR("sv1: could not read the subscribe reply\r\n"));
            return;
        }
        extranonce1_len = (uint8_t)(len / 2);
        for (p++; *p >= '0' && *p <= '9' && size < 100; p++)
            size = (uint8_t)(size * 10 + (*p - '0'));
        extranonce2_len = size;

        if (extranonce1_len + extranonce2_len == EXTRANONCE_BYTES &&
            extranonce2_len <= sizeof extranonce2_source) {
            have_extranonce = 1;
            uart_puts_P(PSTR("sv1: subscribed\r\n"));
        } else {
            /* e.g. a proxy's first "introduce yourself" session, closed as
             * soon as it has our worker name. No jobs to mine here. */
            uart_puts_P(PSTR("sv1: subscribed, but these extranonce sizes cannot be mined; waiting\r\n"));
        }
        return;
    }

    /* Everything else (replies to authorize and submit) has
     * already been shown on the console; nothing to do with it. */
}

/* ---- the hooks miner.c calls -------------------------------------------- */

void pool_start(void) { }

uint8_t pool_command(const char *line)
{
    (void)line;
    return 0;                               /* SV1 has no settings of its own */
}

void pool_reset(uint8_t hang_up)
{
    if (hang_up)
        net_close();
    was_connected = 0;
    have_extranonce = 0;
    waiting = 0;
    miner_stop();
}

/* The time is only for the console's time stamps here (SV1 has no
 * certificate to check), so a failure just means trying again later. */
static void keep_time(uint32_t now)
{
    if ((int32_t)(now - next_time_fetch) < 0 || !net_link_up())
        return;
    if (clock_known() && clock_age() < TIME_MAX_AGE)
        return;
    if (clock_fetch()) {
        uart_puts_P(PSTR("sv1: time server says "));
        uart_put_u32(clock_now());
        uart_puts_P(PSTR(" seconds since 1970\r\n"));
    } else {
        next_time_fetch = now + 60;
    }
}

void pool_service(void)
{
    uint8_t chunk[32];
    uint16_t got;
    uint32_t now = platform_seconds();

    keep_time(now);
    net_poll();

    if (net_connected() && !was_connected) {
        was_connected = 1;
        waiting = 0;
        had_job = 0;
        uart_puts_P(PSTR("sv1: connected to the pool\r\n"));
        rx_len = 0;
        rx_overflow = 0;
        have_extranonce = 0;
        have_diff = 0;                      /* each connection sends its own */
        out_str("{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"" USER_AGENT "\"]}");
        out_send();
        out_str("{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"");
        out_str(settings.worker);
        out_str("\",\"x\"]}");
        out_send();
    } else if (!net_connected() && was_connected) {
        was_connected = 0;
        miner_stop();
        if (!had_job)
            miner_failed();                 /* lost before it ever gave us work */
        uart_puts_P(PSTR("sv1: connection lost, retrying\r\n"));
    }

    if (!net_connected()) {
        /* The network chip keeps retrying by itself, so this would
         * otherwise wait silently forever. After a minute, say what the
         * chip looks like and start it afresh. */
        if (!waiting) {
            waiting = 1;
            waiting_since = now;
        } else if (now - waiting_since > CONNECT_TIMEOUT) {
            uart_puts_P(PSTR("sv1: could not connect for a minute; restarting the network chip\r\n"));
            net_report();
            if (!net_init())
                uart_puts_P(PSTR("sv1: the network chip does not answer\r\n"));
            miner_failed();
            waiting_since = now;
        }
        return;
    }

    while ((got = net_recv(chunk, sizeof chunk)) != 0) {
        for (uint16_t i = 0; i < got; i++) {
            char c = (char)chunk[i];
            if (c == '\n' || c == '\r') {
                if (rx_overflow)
                    uart_puts_P(PSTR("sv1: line too long, ignored\r\n"));
                else if (rx_len > 0)
                    handle_line();
                rx_len = 0;
                rx_overflow = 0;
            } else if (rx_len < RX_MAX) {
                /* The line is shown on the console and the web page: no
                 * control characters from the server (escape sequences). */
                rx_line[rx_len++] = (c >= ' ' && c <= '~') ? c : '?';
            } else {
                rx_overflow = 1;
            }
        }
    }
}
