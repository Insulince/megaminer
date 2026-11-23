/*
 * miner_net.c - XBT toy miner that takes its jobs over the network.
 *
 * Build/flash:  make flash TARGET=miner_net          (NET=w5500 by default)
 * PC side:      go run ./cmd/bridge -listen :4242 ...
 *
 * The chip connects to the bridge by TCP (addresses in net_config.h) and
 * then speaks exactly the same one-line-per-message protocol as
 * miner_uart.c:
 *
 *   PC -> chip    J<work><target>             a job (225 characters)
 *   chip -> PC    ready                       connected, waiting for a job
 *                 job ok                      job accepted, mining started
 *                 bad line: N chars           the line was not a valid job
 *                 share <nonce> <digest>      a digest at or below the target
 *                 rate <per second> <total>   every 5 seconds while mining
 *
 * If the connection drops, the chip stops mining and keeps trying to
 * reconnect.
 *
 * The serial port (250000 8N1) is now only a debug console: it shows what
 * the network is doing and a copy of every line sent. Nothing needs to be
 * connected to it.
 */

#ifndef F_CPU
#define F_CPU 20000000UL
#endif

#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdint.h>
#include <string.h>

#include "blake2b.h"
#include "net.h"
#include "settings.h"
#include "uart.h"

#if F_CPU != 20000000UL
#error "the one-second timer value below assumes a 20 MHz clock"
#endif

#define WORK_BYTES       80
#define DIGEST_BYTES     32     /* XBT proof-of-work uses a 32-byte digest */
#define NONCE_OFFSET     32     /* nNonce sits at bytes 32-35, low byte first */
#define REPORT_SECONDS   5
#define JOB_LINE_CHARS   (1 + 2 * WORK_BYTES + 2 * DIGEST_BYTES)   /* 225 */

/* Look at the network once per this many hashes (about every 90 ms). */
#define HASHES_PER_POLL  16

static uint8_t work[WORK_BYTES];
static uint8_t target[DIGEST_BYTES];

/* The line being received, and the line being built to send. */
#define LINE_MAX 250
static char     rx_line[LINE_MAX + 1];
static uint16_t rx_len;
static uint8_t  rx_overflow;
static char     tx_line[128];
static uint8_t  tx_len;

static uint8_t  mining;
static uint8_t  was_connected;
static uint32_t nonce;
static uint32_t attempts;                   /* for the current job */
static uint32_t attempts_at_report;
static uint8_t  last_report;                /* value of `seconds` at last report */

/* Seconds since reset, counted by the timer interrupt. One byte so the main
 * loop reads it in a single step. */
static volatile uint8_t seconds;

/* Runs once a second. */
ISR(TIMER1_COMPA_vect)
{
    seconds++;
    PINB = _BV(PINB0);                      /* heartbeat on the LED */
}

static void one_second_timer_init(void)
{
    /* 20 MHz / 1024 = 19,531.25 ticks per second. */
    OCR1A  = 19530;
    TCCR1A = 0;
    TCCR1B = _BV(WGM12) | _BV(CS12) | _BV(CS10);   /* reset-on-match, clock / 1024 */
    TIMSK1 = _BV(OCIE1A);
}

/* ---- building and sending a line ---------------------------------------- */

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
    static const char hex[] = "0123456789abcdef";

    while (len--) {
        out_char(hex[*bytes >> 4]);
        out_char(hex[*bytes & 0x0f]);
        bytes++;
    }
}

/* Send the line built so far to the bridge, and copy it to the console. */
static void out_send(void)
{
    tx_line[tx_len] = '\0';
    uart_puts_P(PSTR("> "));
    uart_puts(tx_line);
    uart_puts_P(PSTR("\r\n"));

    tx_line[tx_len++] = '\n';
    if (net_connected() && !net_send((const uint8_t *)tx_line, tx_len))
        uart_puts_P(PSTR("net: send failed\r\n"));
    tx_len = 0;
}

/* ---- jobs --------------------------------------------------------------- */

/* One hex digit to its value 0-15, or -1 if it is not a hex digit. */
static int8_t hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Convert 2*n hex digits to n bytes. Returns 1 on success, 0 on a bad digit. */
static uint8_t parse_hex(const char *hex, uint8_t *out, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        int8_t hi = hex_value(hex[2 * i]);
        int8_t lo = hex_value(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

/* Try to load a job from a received line. Returns 1 if it was a valid job.
 * Parses into temporaries first so a bad line cannot damage the current job.
 * Anything before the last J is ignored, as in miner_uart.c. */
static uint8_t load_job(const char *line)
{
    uint8_t new_work[WORK_BYTES];
    uint8_t new_target[DIGEST_BYTES];

    line = strrchr(line, 'J');
    if (!line || strlen(line) != JOB_LINE_CHARS)
        return 0;
    if (!parse_hex(line + 1, new_work, WORK_BYTES) ||
        !parse_hex(line + 1 + 2 * WORK_BYTES, new_target, DIGEST_BYTES))
        return 0;
    memcpy(work, new_work, WORK_BYTES);
    memcpy(target, new_target, DIGEST_BYTES);
    return 1;
}

static uint32_t get_nonce(void)
{
    return (uint32_t)work[NONCE_OFFSET]
         | (uint32_t)work[NONCE_OFFSET + 1] << 8
         | (uint32_t)work[NONCE_OFFSET + 2] << 16
         | (uint32_t)work[NONCE_OFFSET + 3] << 24;
}

static void set_nonce(uint32_t n)
{
    work[NONCE_OFFSET + 0] = (uint8_t)(n);
    work[NONCE_OFFSET + 1] = (uint8_t)(n >> 8);
    work[NONCE_OFFSET + 2] = (uint8_t)(n >> 16);
    work[NONCE_OFFSET + 3] = (uint8_t)(n >> 24);
}

static void hash_work(uint8_t *digest)
{
    blake2b_ctx ctx;

    blake2b_init(&ctx, DIGEST_BYTES);
    blake2b_update(&ctx, work, WORK_BYTES);
    blake2b_final(&ctx, digest);
}

/* A complete line has arrived from the bridge. */
static void handle_line(void)
{
    rx_line[rx_len] = '\0';
    if (!rx_overflow && load_job(rx_line)) {
        mining = 1;
        nonce = get_nonce();
        attempts = 0;
        attempts_at_report = 0;
        last_report = seconds;
        out_str("job ok");
    } else {
        out_str("bad line: ");
        out_u32(rx_len);
        out_str(rx_overflow ? "+ chars (want " : " chars (want ");
        out_u32(JOB_LINE_CHARS);
        out_char(')');
    }
    out_send();
}

/* ---- network ------------------------------------------------------------ */

/* Keep the connection going and collect whatever has arrived. */
static void service_network(void)
{
    uint8_t chunk[32];
    uint16_t got;

    /* A settings command typed on the console (see settings.h)? */
    net_service();                          /* network console and web page */
    const char *typed = uart_line();
    if (typed) {
        if (settings_command(typed) == SETTINGS_APPLY) {
            mining = 0;
            was_connected = 0;
            net_init();
        }
        uart_line_done();
    }

    net_poll();

    if (net_connected() && !was_connected) {
        was_connected = 1;
        uart_puts_P(PSTR("net: connected\r\n"));
        rx_len = 0;
        rx_overflow = 0;
        out_str("ready");
        out_send();
    } else if (!net_connected() && was_connected) {
        was_connected = 0;
        mining = 0;                         /* nobody to report shares to */
        uart_puts_P(PSTR("net: connection lost, retrying\r\n"));
    }
    if (!net_connected())
        return;

    while ((got = net_recv(chunk, sizeof chunk)) != 0) {
        for (uint16_t i = 0; i < got; i++) {
            char c = (char)chunk[i];
            if (c == '\n' || c == '\r') {
                if (rx_len > 0)
                    handle_line();
                rx_len = 0;
                rx_overflow = 0;
            } else if (rx_len < LINE_MAX) {
                rx_line[rx_len++] = c;
            } else {
                rx_overflow = 1;            /* too long: reject at the newline */
            }
        }
    }
}

int main(void)
{
    uint8_t digest[DIGEST_BYTES];
    uint8_t hashes_since_poll = 0;

    DDRB |= _BV(DDB0);                      /* PB0 as output */
    uart_init();
    uart_echo = 1;                          /* show what is typed on the console */
    one_second_timer_init();
    sei();

    uart_puts_P(PSTR("\r\nminer_net starting (type help for settings commands)\r\n"));
    settings_load();
    settings_show();
    if (!net_init()) {
        uart_puts_P(PSTR("net: the network chip does not answer; stopping\r\n"));
        for (;;)
            ;
    }
    uart_puts(net_link_up() ? "net: link up\r\n" : "net: no link yet (cable?)\r\n");

    for (;;) {
        /* While idle, watch the network constantly; while mining, only
         * every few hashes so it costs almost nothing. */
        if (!mining || ++hashes_since_poll >= HASHES_PER_POLL) {
            hashes_since_poll = 0;
            service_network();
        }
        if (!mining)
            continue;

        /* One attempt. */
        set_nonce(nonce);
        hash_work(digest);
        if (memcmp(digest, target, DIGEST_BYTES) <= 0) {
            out_str("share ");
            out_u32(nonce);
            out_char(' ');
            out_hex(digest, DIGEST_BYTES);
            out_send();
        }
        nonce++;
        attempts++;

        /* Rate report. */
        uint8_t elapsed = (uint8_t)(seconds - last_report);
        if (elapsed >= REPORT_SECONDS) {
            out_str("rate ");
            out_u32((attempts - attempts_at_report) / elapsed);
            out_char(' ');
            out_u32(attempts);
            out_send();

            last_report += elapsed;
            attempts_at_report = attempts;
        }
    }
}
