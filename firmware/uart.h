/*
 * uart.h - minimal serial port on USART0.
 *   transmit: pin 15 (PD1 / TXD0)      receive: pin 14 (PD0 / RXD0)
 * 250000 baud, 8 data bits, no parity, 1 stop bit.
 *
 * Receiving is done by an interrupt, so the program must call sei() before
 * any input will be seen. Programs that only print can ignore that.
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>

/* Longest input line accepted, not counting the line ending. */
#define UART_LINE_MAX 250

void uart_init(void);

/* Output. */
void uart_putc(char c);
void uart_puts(const char *s);

/* The same for text kept in flash: uart_puts_P(PSTR("text")). On this chip a
 * plain "text" is copied into RAM at start-up and stays there; PSTR() keeps
 * it in flash (moving the messages to flash freed ~7 KB of the 16 KB of
 * RAM). On the PC (tests, simulator) there is no difference, so the two
 * are the same there, and so are avr-libc's *_P string functions. */
#ifdef __AVR__
#include <avr/pgmspace.h>
void uart_puts_P(const char *flash_text);
#else
#include <string.h>
#ifndef PSTR
#define PSTR(s) (s)
#endif
#define uart_puts_P(s) uart_puts(s)
#ifndef strcmp_P
#define strcmp_P strcmp
#define strncmp_P strncmp
#endif
#endif
void uart_put_u32(uint32_t n);                          /* decimal */
void uart_put_hex(const uint8_t *bytes, uint8_t len);   /* two hex digits per byte */

/* Optional: called before the first character of every output line (for a
 * time stamp). It may print; its own output does not trigger it again. */
extern void (*uart_line_prefix)(void);

/* Optional: while set, everything printed goes to this function instead of
 * the serial line and the log (no time stamps either). Used to build a web
 * reply with the ordinary printing functions. */
extern void (*uart_redirect)(char c);

/* Set to 1 to echo typed characters back to the terminal (passwords after
 * "pass=" as stars; backspace works). Off by default: the Go bridge's serial
 * job protocol (miner_uart) must not get its own lines sent back. */
extern uint8_t uart_echo;

/* Input, one line at a time. Spaces and tabs are dropped, except in lines
 * starting "ssid=" or "pass=" (WiFi names and passwords may contain them). uart_line() returns the waiting line (without
 * its line ending), or 0 if no complete line has arrived. Call
 * uart_line_done() when finished with it; input arriving before then is
 * discarded. */
const char *uart_line(void);
void uart_line_done(void);

/* Hand the program a line as if it had been typed (the network console and
 * the web page use this, so their commands go through exactly the same
 * handling as typed ones). Returns 0 if a line is already waiting or being
 * typed; try again shortly. */
uint8_t uart_inject_line(const char *s);

/* How many received bytes the hardware has flagged as damaged (missing stop
 * bit) or lost (overrun) since reset. Stops counting at 255. */
uint8_t uart_rx_errors(void);

#endif
