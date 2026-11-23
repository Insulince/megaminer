/*
 * miner_uart.c - XBT toy miner that takes its jobs over the serial port.
 *
 * Build/flash:  make flash TARGET=miner_uart
 * PC side:      250000 8N1. Every message is one line of text.
 *
 * PC -> chip
 *   J<work><target>    a job. <work> is the 80-byte work block as 160 hex
 *                      digits; <target> is 32 bytes as 64 hex digits.
 *                      225 characters in all. A new job replaces the current
 *                      one. Anything before the J is ignored, and senders
 *                      should put a few throwaway characters there (UUUU).
 *
 * chip -> PC
 *   ready                      after reset, waiting for a job
 *   job ok                     job accepted, mining started
 *   bad line: ...              the line was not a valid job, with details
 *   share <nonce> <digest>     a digest at or below the target
 *   rate <per second> <total>  every 5 seconds while mining
 *
 * Mining starts at the nonce already in the work block (bytes 32-35, low
 * byte first) and counts up. A digest counts as a share when, compared byte
 * by byte from the first byte, it is less than or equal to <target>. So the
 * target is in the same byte order as the digest: a target starting 0000ff...
 * asks for two leading zero bytes.
 *
 * Not handled here: the XOR mask applied to the block hash when the header's
 * m_xor_key is non-zero. The PC side must account for that.
 */

#ifndef F_CPU
#define F_CPU 20000000UL
#endif

#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdint.h>
#include <string.h>

#include "blake2b.h"
#include "uart.h"

#if F_CPU != 20000000UL
#error "the one-second timer value below assumes a 20 MHz clock"
#endif

#define WORK_BYTES       80
#define DIGEST_BYTES     32     /* XBT proof-of-work uses a 32-byte digest */
#define NONCE_OFFSET     32     /* nNonce sits at bytes 32-35, low byte first */
#define REPORT_SECONDS   5

static uint8_t work[WORK_BYTES];
static uint8_t target[DIGEST_BYTES];

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
        if (hi < 0 || lo < 0) {
            return 0;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

#define JOB_LINE_CHARS (1 + 2 * WORK_BYTES + 2 * DIGEST_BYTES)   /* 225 */

/* Say what was wrong with a line, so a bad paste can be diagnosed from the
 * terminal: its length, its first character, and where the first character
 * that is not a hex digit sits (counting the J as position 0). */
static void report_bad_line(const char *line)
{
    uint16_t len = strlen(line);

    uart_puts_P(PSTR("bad line: "));
    uart_put_u32(len);
    uart_puts_P(PSTR(" chars (want "));
    uart_put_u32(JOB_LINE_CHARS);
    uart_puts_P(PSTR("), first char code "));
    uart_put_u32((uint8_t)line[0]);
    for (uint16_t i = 1; i < len; i++) {
        if (hex_value(line[i]) < 0) {
            uart_puts_P(PSTR(", non-hex char code "));
            uart_put_u32((uint8_t)line[i]);
            uart_puts_P(PSTR(" at position "));
            uart_put_u32(i);
            break;
        }
    }
    uart_puts_P(PSTR(", receive errors so far "));
    uart_put_u32(uart_rx_errors());
    uart_puts_P(PSTR("\r\n"));
}

/* Try to load a job from a received line. Returns 1 if it was a valid job.
 * Parses into temporaries first so a bad line cannot damage the current job. */
static uint8_t load_job(const char *line)
{
    uint8_t new_work[WORK_BYTES];
    uint8_t new_target[DIGEST_BYTES];

    /* Skip anything before the last J. The sender puts a few throwaway
     * characters in front of each job, because the first characters of a
     * transmission can arrive damaged; J is not a hex digit, so the last J
     * in the line is unambiguous. */
    line = strrchr(line, 'J');
    if (!line || strlen(line) != JOB_LINE_CHARS) {
        return 0;
    }
    if (!parse_hex(line + 1, new_work, WORK_BYTES) ||
        !parse_hex(line + 1 + 2 * WORK_BYTES, new_target, DIGEST_BYTES)) {
        return 0;
    }
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

static void set_nonce(uint32_t nonce)
{
    work[NONCE_OFFSET + 0] = (uint8_t)(nonce);
    work[NONCE_OFFSET + 1] = (uint8_t)(nonce >> 8);
    work[NONCE_OFFSET + 2] = (uint8_t)(nonce >> 16);
    work[NONCE_OFFSET + 3] = (uint8_t)(nonce >> 24);
}

static void hash_work(uint8_t *digest)
{
    blake2b_ctx ctx;

    blake2b_init(&ctx, DIGEST_BYTES);
    blake2b_update(&ctx, work, WORK_BYTES);
    blake2b_final(&ctx, digest);
}

int main(void)
{
    uint8_t  digest[DIGEST_BYTES];
    uint8_t  mining = 0;
    uint32_t nonce = 0;
    uint32_t attempts = 0;                  /* for the current job */
    uint32_t attempts_at_report = 0;
    uint8_t  last_report = 0;               /* value of `seconds` at last report */

    DDRB |= _BV(DDB0);                      /* PB0 as output */
    uart_init();
    one_second_timer_init();
    sei();

    uart_puts_P(PSTR("\r\nready\r\n"));

    for (;;) {
        /* A line from the PC? */
        const char *line = uart_line();
        if (line) {
            if (load_job(line)) {
                mining = 1;
                nonce = get_nonce();
                attempts = 0;
                attempts_at_report = 0;
                last_report = seconds;
                uart_puts_P(PSTR("job ok\r\n"));
                if (uart_rx_errors()) {
                    uart_puts_P(PSTR("note: receive errors so far "));
                    uart_put_u32(uart_rx_errors());
                    uart_puts_P(PSTR("\r\n"));
                }
            } else {
                report_bad_line(line);
            }
            uart_line_done();
        }

        if (!mining) {
            continue;
        }

        /* One attempt. */
        set_nonce(nonce);
        hash_work(digest);
        if (memcmp(digest, target, DIGEST_BYTES) <= 0) {
            uart_puts_P(PSTR("share "));
            uart_put_u32(nonce);
            uart_putc(' ');
            uart_put_hex(digest, DIGEST_BYTES);
            uart_puts_P(PSTR("\r\n"));
        }
        nonce++;
        attempts++;

        /* Rate report. */
        uint8_t elapsed = (uint8_t)(seconds - last_report);
        if (elapsed >= REPORT_SECONDS) {
            uart_puts_P(PSTR("rate "));
            uart_put_u32((attempts - attempts_at_report) / elapsed);
            uart_putc(' ');
            uart_put_u32(attempts);
            uart_puts_P(PSTR("\r\n"));

            last_report += elapsed;
            attempts_at_report = attempts;
        }
    }
}
