/* uart.c - see uart.h. */

#include "uart.h"

#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdlib.h>
#include <string.h>

/* Optional: a copy of every byte printed (net_winc.c keeps recent output for
 * the network console and the web page). Weak: absent, nothing is copied. */
void log_tap(char c) __attribute__((weak));

#define BAUD 250000UL

/* The UART's bit clock is F_CPU / 16 / (UBRR0 + 1). At 20 MHz: 5 - 1 = 4. */
#define UBRR_VALUE (F_CPU / 16 / BAUD - 1)

static char    rx_line[UART_LINE_MAX + 1];  /* the line being received */
static uint8_t rx_len;                      /* characters in it so far */
static uint8_t rx_too_long;                 /* it went past UART_LINE_MAX */
static volatile uint8_t rx_ready;           /* 1 = complete line waiting */
static volatile uint8_t rx_errors;          /* bytes the hardware flagged as damaged */

void uart_init(void)
{
    UBRR0  = UBRR_VALUE;                    /* baud rate divider */
    UCSR0B = _BV(TXEN0)                     /* transmitter on */
           | _BV(RXEN0)                     /* receiver on */
           | _BV(RXCIE0);                   /* interrupt when a byte arrives */
    UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);     /* 8 data bits, no parity, 1 stop */
    /* Pull-up on the receive pin (PD0): with the USB-serial adapter
     * unplugged nothing drives it, and a floating input picks up noise as
     * random characters, which would half-fill the input line and make
     * every command from the network console or web page answer "busy".
     * Idle serial is high, so the pull-up looks exactly like "connected,
     * nobody typing". With the adapter plugged in, its output overrides it. */
    PORTD |= _BV(PD0);
}

uint8_t uart_echo;

/* Send one character from inside the receive interrupt. Waits at most one
 * character time (40 us) if the main program is in the middle of sending. */
static void echo(char c)
{
    while (!(UCSR0A & _BV(UDRE0))) {
    }
    UDR0 = c;
}

/* A WiFi network name or password may contain spaces, so in the two
 * "wifi." lines (wifi.ssid=, wifi.password=) they are kept. */
static uint8_t keeps_spaces(void)
{
    return rx_len >= 5 && memcmp_P(rx_line, PSTR("wifi."), 5) == 0;
}

/* Passwords echo as stars. */
static uint8_t is_secret(void)
{
    return (rx_len > 14 && memcmp_P(rx_line, PSTR("wifi.password="), 14) == 0)
        || (rx_len > 17 && memcmp_P(rx_line, PSTR("console.password="), 17) == 0);
}

/* Runs each time a byte has been received. */
ISR(USART0_RX_vect)
{
    uint8_t status = UCSR0A;                /* must be read before UDR0 */
    char c = UDR0;                          /* take the byte (clears the interrupt) */

    /* FE0: the stop bit was missing (framing error). DOR0: a byte was lost
     * because the previous one had not been taken yet (overrun). */
    if ((status & (_BV(FE0) | _BV(DOR0))) && rx_errors < 255) {
        rx_errors++;
    }

    if (rx_ready) {
        return;                             /* previous line not handled yet: drop */
    }
    if (c == '\r' || c == '\n') {
        if (rx_too_long) {
            /* Cut short, a command could do something else than meant (a
             * shorter password saved, say), so it is dropped instead. */
            rx_too_long = 0;
            rx_len = 0;
            if (uart_echo)
                for (const char *p = PSTR("\r\n(line too long; ignored)\r\n"); pgm_read_byte(p); p++)
                    echo((char)pgm_read_byte(p));
        } else if (rx_len > 0) {            /* ignore empty lines */
            rx_line[rx_len] = '\0';
            rx_ready = 1;
            if (uart_echo) {
                echo('\r');
                echo('\n');
            }
        }
        return;
    }
    if (c == '\b' || c == 0x7f) {           /* backspace (PuTTY sends 0x7f) */
        if (rx_len > 0) {
            rx_len--;
            if (uart_echo) {
                echo('\b');
                echo(' ');
                echo('\b');
            }
        }
        return;
    }
    if ((c == ' ' || c == '\t') && !keeps_spaces()) {
        return;                             /* ignore stray spaces from copy-paste */
    }
    if (rx_len < UART_LINE_MAX) {
        rx_line[rx_len++] = c;
        if (uart_echo)
            echo(is_secret() ? '*' : c);
    } else {
        rx_too_long = 1;                    /* dropped at the end of the line */
    }
}

const char *uart_line(void)
{
    return rx_ready ? rx_line : 0;
}

uint8_t uart_rx_errors(void)
{
    return rx_errors;
}

void uart_line_done(void)
{
    rx_len   = 0;
    rx_ready = 0;                           /* cleared last: the interrupt
                                               leaves the buffer alone until then */
}

void (*uart_line_prefix)(void);
void (*uart_redirect)(char c);

static void put_raw(char c);

void uart_putc(char c)
{
    static uint8_t mid_line, in_prefix;

    if (uart_redirect) {                /* e.g. the web page's /status reply */
        uart_redirect(c);
        return;
    }
    /* Line endings themselves get no stamp, so "\r\n" and blank lines
     * pass through untouched. */
    if (!mid_line && !in_prefix && uart_line_prefix && c != '\r' && c != '\n') {
        in_prefix = 1;
        uart_line_prefix();
        in_prefix = 0;
    }
    if (!in_prefix)
        mid_line = (c != '\n');
    put_raw(c);
}

static void put_raw(char c)
{
    /* Wait until the transmit buffer is empty, then fill it, with
     * interrupts off between the check and the write: otherwise an echo
     * from the receive interrupt could take the slot in between, and this
     * character would be silently dropped by the hardware. */
    for (;;) {
        uint8_t sreg = SREG;
        cli();
        if (UCSR0A & _BV(UDRE0)) {
            UDR0 = c;
            SREG = sreg;
            break;
        }
        SREG = sreg;
    }
    if (log_tap)
        log_tap(c);
}

uint8_t uart_inject_line(const char *s)
{
    uint8_t ok = 0;
    uint8_t sreg = SREG;

    cli();
    if (!rx_ready && rx_len == 0) {     /* nothing typed on the serial line meanwhile */
        strncpy(rx_line, s, UART_LINE_MAX);
        rx_line[UART_LINE_MAX] = '\0';
        rx_ready = 1;
        ok = 1;
    }
    SREG = sreg;
    return ok;
}

void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}

void uart_puts_P(const char *flash_text)
{
    char c;

    while ((c = (char)pgm_read_byte(flash_text++)) != '\0') {
        uart_putc(c);
    }
}

void uart_put_u32(uint32_t n)
{
    char digits[11];                        /* "4294967295" plus terminator */

    ultoa(n, digits, 10);
    uart_puts(digits);
}

void uart_put_hex(const uint8_t *bytes, uint8_t len)
{
    static const char HEX[] = "0123456789abcdef";

    for (uint8_t i = 0; i < len; i++) {
        uart_putc(HEX[bytes[i] >> 4]);      /* high four bits */
        uart_putc(HEX[bytes[i] & 0x0f]);    /* low four bits */
    }
}
