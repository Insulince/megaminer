/*
 * miner.c - the part of every megaminer program that is not the pool's
 * protocol: start-up, console, clock, lights, watchdog, /status and the
 * hashing loop. See miner.h for how a protocol program plugs in.
 *
 * Console output from here:
 *   miner: ...  start-up, settings being applied, the stuck rule
 *   work ...    the 80-byte block being hashed for the current job
 *   target ...  the share target, most significant byte first
 *   rate ...    speed while hashing | averages over the last 10 min, 1 h and
 *               1 day including time spent not hashing | best hash | uptime
 *               | hashes since start-up | jobs so far | supply voltage
 *               (see hashrate.h)
 * Every line is stamped [HH:MM:SSZ] (UTC) once the time server has
 * answered, [+Ns] (seconds since start-up) before that.
 *
 * Build with -DSTRATUM_SUBMIT_TEST (make ... SUBMIT_TEST=1) to submit, once
 * per job, the first digest that merely starts with a zero byte. The pool
 * should reject it; the point is to exercise the submit message.
 */

#include <stdint.h>
#include <string.h>

#include "blake2b.h"
#include "blake2b_work.h"
#include "board.h"
#include "bootshare.h"                 /* BOOT_CHOICE_* (the start-up button menu) */
#include "entropy.h"
#include "hashrate.h"
#include "miner.h"
#include "net.h"
#include "settings.h"
#include "uart.h"
#include "uptime.h"
#include "version.h"
#include "watchdog.h"

#define HASHES_PER_POLL   64            /* look at the network about every 65 ms */
#define STUCK_SECONDS     (15 * 60)     /* not hashing this long while running: restart */

/* ---- the hardware underneath (the chip here; the PC in tests/sim) ---- */

void platform_init(void);

#ifdef __AVR__

#include <avr/interrupt.h>
#include <avr/io.h>

void platform_init(void)
{
    DDRB |= _BV(DDB0);
    board_init();                           /* board.c: lights, button, voltage */
    uart_init();
    uart_echo = 1;                          /* show what is typed on the console */
    uptime_start();                         /* uptime.c: the clock (board.c has the heartbeat) */
    entropy_start();                        /* randomness, and the watchdog's tick */
    sei();
    watchdog_start();                       /* watchdog.c: from now on, a stop restarts the chip */
}

#endif /* __AVR__ */

/* ---- state ---- */

struct miner_job job;

static uint8_t  mining;
static uint8_t  failures;           /* failed attempts in a row since the last job */
static uint8_t  needs_you;          /* waiting on something only a person can fix */
static uint8_t  link_down;          /* refreshed by service(); see light() */
static uint16_t jobs_received;
static uint32_t hashed_at;          /* last time it was hashing (or rightly waiting) */
static uint8_t  busy;               /* inside a long calculation (miner_while_busy) */
static uint8_t  apply_pending;      /* "save" was typed during one */
static uint8_t  pending_changes;    /* settings_changed of every save not yet applied */
#ifdef STRATUM_SUBMIT_TEST
static uint8_t  test_share_sent;
#endif

/* ---- small helpers ---- */

static void say_P(const char *flash_text)
{
    uart_puts_P(PSTR("miner: "));
    uart_puts_P(flash_text);
    uart_puts_P(PSTR("\r\n"));
}

void put_hex_line(const char *label, const uint8_t *bytes, uint8_t len)
{
    uart_puts_P(label);
    uart_put_hex(bytes, len);
    uart_puts_P(PSTR("\r\n"));
}

void miner_hash(const uint8_t *in, uint8_t len, uint8_t *digest)
{
    blake2b_ctx ctx;

    blake2b_init(&ctx, MINER_DIGEST_BYTES);
    blake2b_update(&ctx, in, len);
    blake2b_final(&ctx, digest);
}

/* "4.87" from millivolts. */
static void put_volts(uint16_t mv)
{
    uart_put_u32(mv / 1000);
    uart_putc('.');
    uart_putc((char)('0' + mv / 100 % 10));
    uart_putc((char)('0' + mv / 10 % 10));
}

/* ---- the clock ---- */

static uint8_t  have_time;
static uint32_t time_unix;          /* seconds since 1970 ... */
static uint32_t time_taken_at;      /* ... at this many seconds after start-up */

uint8_t  clock_known(void)  { return have_time; }
uint32_t clock_age(void)    { return platform_seconds() - time_taken_at; }
uint32_t clock_now(void)    { return time_unix + (platform_seconds() - time_taken_at); }
void     clock_forget(void) { have_time = 0; }

/* Ask the time server (SNTP, RFC 4330). The answer is NOT authenticated:
 * anyone on the path could lie about the time. Accepted on purpose: for SV2
 * a lie can only make an expired certificate look current. */
uint8_t clock_fetch(void)
{
    uint8_t packet[48] = {0};
    uint32_t since_1900;

    packet[0] = 0x23;                       /* no warning, version 4, "client" */
    if (net_udp_exchange(settings.ntp_ip, 123, packet, sizeof packet,
                         packet, sizeof packet) < 48)
        return 0;
    if ((packet[0] & 0x07) != 4 || packet[1] == 0)  /* not a server reply, or */
        return 0;                                   /* the server has no time */

    /* Bytes 40-43: the server's clock, whole seconds since 1900, high byte first. */
    since_1900 = (uint32_t)packet[40] << 24 | (uint32_t)packet[41] << 16 |
                 (uint32_t)packet[42] << 8 | packet[43];
    if (since_1900 < 3913056000UL)          /* before 2024: nonsense */
        return 0;
    time_unix = since_1900 - 2208988800UL;  /* 1900 -> 1970 */
    time_taken_at = platform_seconds();
    have_time = 1;
    return 1;
}

static void put_two(uint8_t n)
{
    uart_putc((char)('0' + n / 10));
    uart_putc((char)('0' + n % 10));
}

/* In front of every console line (uart_line_prefix). */
static void stamp_line(void)
{
    uart_putc('[');
    if (have_time) {
        uint32_t day_seconds = clock_now() % 86400UL;

        put_two((uint8_t)(day_seconds / 3600));
        uart_putc(':');
        put_two((uint8_t)(day_seconds / 60 % 60));
        uart_putc(':');
        put_two((uint8_t)(day_seconds % 60));
        uart_putc('Z');
    } else {
        uart_putc('+');
        uart_put_u32(platform_seconds());
        uart_putc('s');
    }
    uart_puts_P(PSTR("] "));
}

/* ---- what the protocol tells us ---- */

void miner_job(uint8_t new_job)
{
    if (!mining || new_job) {               /* a new job starts from nonce 0 */
        job.nonce = 0;
        jobs_received++;
#ifdef STRATUM_SUBMIT_TEST
        test_share_sent = 0;
#endif
    }
    mining = 1;
    failures = 0;
    needs_you = 0;
    bw_setup(job.work);                     /* the per-job part of the fast hashing */
    hashrate_interrupted();                 /* handling the job took time away from hashing */

    put_hex_line(PSTR("work "), job.work, MINER_WORK_BYTES);
    put_hex_line(PSTR("target "), job.target, MINER_DIGEST_BYTES);
    {
        /* What a pool puts here varies: the network's difficulty (most pools)
         * or its own share target (the DATUM gateway). hashrate.c copes. */
        uint8_t bits[4] = { (uint8_t)(job.nbits >> 24), (uint8_t)(job.nbits >> 16),
                            (uint8_t)(job.nbits >> 8), (uint8_t)job.nbits };
        put_hex_line(PSTR("nbits "), bits, 4);
    }
    hashrate_set_target(job.target);
    hashrate_set_nbits(job.nbits);
}

void miner_stop(void)
{
    mining = 0;
    hashrate_interrupted();
}

void miner_failed(void)
{
    if (failures < 255)
        failures++;
}

void miner_needs_you(uint8_t yes) { needs_you = yes; }
uint8_t miner_hashing(void) { return mining; }

/* The traffic light's colour. link_down is refreshed by service(); not
 * asked here, because status_json calls this from inside a web reply,
 * where the module must not be polled. */
static uint8_t light(void)
{
    if (mining)
        return BOARD_GREEN;
    if (needs_you || link_down || failures >= MINER_FAILURES_RED)
        return BOARD_RED;
    return BOARD_YELLOW;
}

/* ---- settings and the console ---- */

/* Start using the settings as they now are, restarting only what the saved
 * changes need (settings_changed, see settings.h):
 *   network settings -> restart the WiFi module (drops every connection,
 *                       including the network console and the web page)
 *   pool settings    -> close the pool connection and make a new one; the
 *                       console and web page stay connected
 *   anything else    -> nothing (e.g. the console password: just stored) */
static void apply_settings(void)
{
    uint8_t changed = pending_changes;

    pending_changes = 0;
    apply_pending = 0;
    if (changed & SETTINGS_CHANGED_NET) {
        say_P(PSTR("restarting the network with the new settings"));
        if (!net_init())
            say_P(PSTR("the network chip does not answer"));
        pool_reset(0);                  /* net_init dropped any connection */
    } else if (changed & SETTINGS_CHANGED_POOL) {
        say_P(PSTR("reconnecting to the pool with the new settings"));
        pool_reset(1);
    } else {
        say_P(PSTR("nothing to restart"));
        return;
    }
    clock_forget();                     /* the time server may have changed */
}

static void service_console(void)
{
    const char *line;

    net_service();                      /* network console and web page */
    line = uart_line();

    if (!line)
        return;
    if (strcmp_P(line, PSTR("resetrestarts")) == 0) {
        watchdog_forget();
    } else if (pool_command(line) || hashrate_command(line)) {
        /* dealt with */
    } else if (settings_command(line) == SETTINGS_APPLY) {
        /* Collected, because a second save during a long calculation compares
         * against the first one's stored values, and must not hide them. */
        pending_changes |= settings_changed;
        if (busy) {
            /* Restarting the network underneath a half-done handshake would
             * leave it working on a connection that no longer exists. */
            apply_pending = 1;
            say_P(PSTR("saved; will take effect when the current calculation finishes"));
        } else {
            apply_settings();
        }
    }
    uart_line_done();
}

void miner_while_busy(void)
{
    busy = 1;
    board_tick();
    service_console();
    busy = 0;
}

/* Everything between hashes, about every 65 ms (constantly when not hashing). */
static void service(void)
{
    uint32_t now = platform_seconds();

    hashrate_tick();
    service_console();
    if (apply_pending)
        apply_settings();
    board_tick();
    link_down = !mining && !net_link_up();
    board_traffic(light());

    /* The stuck rule: the program is alive, so the watchdog is fed, yet
     * nothing has been hashed for STUCK_SECONDS. A restart gets everything
     * afresh, the WiFi module included. Not while waiting for something only
     * a person can give: a restart cannot fix that. */
    if (mining || needs_you) {
        hashed_at = now;
    } else if (now - hashed_at >= STUCK_SECONDS) {
        say_P(PSTR("not hashing for 15 minutes although running; restarting the chip (stuck rule)"));
        watchdog_restart(WATCHDOG_STUCK);
    }

    pool_service();
}

/* ---- the web page's /status (net_winc.c writes the braces, rssi, worker) ---- */

void status_json(void)
{
    uart_puts_P(PSTR(",\"state\":\""));
    {
        uint8_t colour = light();
        uart_puts_P(colour == BOARD_GREEN ? PSTR("mining")
                    : colour == BOARD_RED ? PSTR("needs you")
                    : failures ? PSTR("retrying") : PSTR("connecting"));
    }
    uart_puts_P(PSTR("\",\"pool\":\""));
    for (uint8_t i = 0; i < 4; i++) {
        if (i)
            uart_putc('.');
        uart_put_u32(settings.server_ip[i]);
    }
    uart_putc(':');
    uart_put_u32(settings.server_port);
    uart_puts_P(PSTR("\",\"jobs\":"));
    uart_put_u32(jobs_received);
    uart_puts_P(PSTR(",\"vcc\":"));
    put_volts(board_vcc_mv());
    watchdog_json();
    hashrate_json();
}

/* ---- start-up ---- */

/* The network module, until it answers. Before, a module that did not
 * answer stopped the program for good (and the watchdog then restarted it
 * over and over); now it is retried, with the heartbeat kept going. */
static void start_network(void)
{
    while (!net_init()) {
        uint32_t until;

        board_traffic(BOARD_RED);
        say_P(PSTR("the network chip does not answer; trying again in 10 s"));
        until = platform_seconds() + MINER_RETRY_SECONDS;
        while ((int32_t)(platform_seconds() - until) < 0)
            board_tick();               /* the heartbeat, which also feeds the watchdog */
        board_traffic(BOARD_YELLOW);
    }
}

/* Check the fast hashing routine against the plain one, then time it. */
static void check_fast_hashing(void)
{
    uint32_t began;
    uint16_t i;

    if (bw_selftest())
        uart_puts_P(PSTR("hashing: fast routine agrees with blake2b.c\r\n"));
    else
        uart_puts_P(PSTR("hashing: FAST ROUTINE GAVE A WRONG DIGEST; using plain blake2b.c instead\r\n"));
    began = platform_millis();
    for (i = 0; i < 256; i++)
        bw_hash(i);
    uart_puts_P(PSTR("hashing: "));
    uart_put_u32((platform_millis() - began) * 1000UL / 256);
    uart_puts_P(PSTR(" microseconds per hash\r\n"));
}

/* Leading zero bits of the first 8 digest bytes (0-64). Nearly every hash
 * has a non-zero first byte, so this is a few shifts. */
static uint8_t leading_zero_bits(const uint8_t *first8)
{
    uint8_t bits = 0, i = 0, b;

    while (i < 8 && first8[i] == 0) {
        bits += 8;
        i++;
    }
    if (i == 8)
        return 64;
    for (b = first8[i]; !(b & 0x80); b <<= 1)
        bits++;
    return bits;
}

/* The nonce goes into the work block low byte first. */
static void put_nonce(void)
{
    job.work[MINER_NONCE_OFFSET + 0] = (uint8_t)(job.nonce);
    job.work[MINER_NONCE_OFFSET + 1] = (uint8_t)(job.nonce >> 8);
    job.work[MINER_NONCE_OFFSET + 2] = (uint8_t)(job.nonce >> 16);
    job.work[MINER_NONCE_OFFSET + 3] = (uint8_t)(job.nonce >> 24);
}

#ifndef __AVR__
#define main miner_main             /* on the PC, the stand-in (tests/sim) owns main() */
int miner_main(void);
#endif

int main(void)
{
    uint8_t digest[MINER_DIGEST_BYTES];
    uint8_t hashes_since_poll = 0;

    platform_init();
    entropy_idle = miner_while_busy;
    uart_line_prefix = stamp_line;

    uart_puts_P(PSTR("\r\nmegaminer " FIRMWARE_VERSION " (" PROGRAM_NAME ") starting (type help for settings commands)\r\n"));
    watchdog_report();                  /* why the last reset happened (and the counts) */
    settings_load();
    board_lamp_test();                  /* every light once: shows the wiring is right */
    if (board_recovery() == BOOT_CHOICE_WIFI_SETUP)  /* button menu (1 is done inside) */
        net_wifi_setup();               /* stays until new WiFi details work (or a reset) */
    hashrate_start();
    pool_start();
    settings_show();
    board_traffic(BOARD_YELLOW);
    start_network();
    check_fast_hashing();

    for (;;) {
        if (!mining || ++hashes_since_poll >= HASHES_PER_POLL) {
            hashes_since_poll = 0;
            service();
        }
        if (!mining)
            continue;

        /* One attempt. The fast routine gives the first 8 digest bytes,
         * which is enough to dismiss nearly every nonce. */
        bw_hash(job.nonce);
        board_beat();                       /* the heartbeat: a byte test per hash */

        if (!(bw_out[0] & 0x80)) {          /* at least one zero bit: half of all hashes */
            uint8_t bits = leading_zero_bits(bw_out);
            hashrate_count_bits(bits);      /* the luck chart */
            if (bits >= 8)
                board_luck(bits);           /* the luck lights */
        }
        if (bw_out[0] == 0 && hashrate_is_better(bw_out)) {
            /* A new best for this run: get the whole digest to keep. */
            put_nonce();
            miner_hash(job.work, MINER_WORK_BYTES, digest);
            hashrate_consider(digest);
        }
        if (bw_out[0] <= job.target[0] && memcmp(bw_out, job.target, 8) <= 0) {
            /* A possible share. Whether it is one is decided on the whole
             * digest from the plain BLAKE2b, never on the fast routine. */
            put_nonce();
            miner_hash(job.work, MINER_WORK_BYTES, digest);
            if (memcmp(digest, job.target, MINER_DIGEST_BYTES) <= 0) {
                put_hex_line(PSTR("SHARE FOUND "), digest, MINER_DIGEST_BYTES);
                pool_submit();
            }
        }
#ifdef STRATUM_SUBMIT_TEST
        else if (bw_out[0] == 0 && !test_share_sent) {
            test_share_sent = 1;
            put_nonce();
            miner_hash(job.work, MINER_WORK_BYTES, digest);
            put_hex_line(PSTR("submit test: sending a digest the pool should reject: "),
                         digest, MINER_DIGEST_BYTES);
            pool_submit();
        }
#endif
        job.nonce++;

        /* Rate report, console only. */
        if (hashrate_hashed()) {
            hashrate_print();
            uart_puts_P(PSTR(" | jobs "));
            uart_put_u32(jobs_received);
            uart_puts_P(PSTR(" | vcc "));
            put_volts(board_vcc_mv());
            uart_puts_P(PSTR(" V\r\n"));
        }
    }
}
