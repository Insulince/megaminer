/*
 * winc.h - the ATWINC1500 WiFi module, lowest two layers:
 *
 *   1. the SPI bus protocol (read/write the module's registers and memory),
 *      and starting the module's own firmware;
 *   2. the "host interface" (HIF): sending a request message to the
 *      firmware and picking up the messages it sends back.
 *
 * Everything above that (joining a network, sockets) is just message
 * formats, built by the caller. Written from Microchip's host driver as
 * shipped in the Arduino WiFi101 library (nmspi.c, nmasic.c, nmdrv.c,
 * m2m_hif.c), which serves as the reference; no code is linked from it.
 *
 * No interrupts are used: the module pulls its IRQ line low when it has a
 * message, and winc_poll() looks at that pin.
 *
 * Wiring: PB4 -> CS, PB5 -> MOSI, PB7 -> SCK (220 ohm),
 * PB6 <- MISO (220 ohm), PB3 -> RST, PB1 -> EN, PB2 <- IRQ.
 */

#ifndef WINC_H
#define WINC_H

#include <stdint.h>

/* Message groups (m2m_types.h tenuM2mReqGroup). */
#define WINC_GROUP_WIFI  1
#define WINC_GROUP_IP    2

/* Set in a request's opcode when it carries a separate data part. */
#define WINC_DATA_PACKET 0x80

/* Power the module up, boot its firmware and get it ready for messages.
 * Takes about half a second. Returns 1 on success; on failure winc_error
 * says what went wrong. Safe to call again to start over. */
uint8_t winc_start(void);

extern const char *winc_error;      /* last failure, in flash: uart_puts_P(winc_error) */
extern uint32_t winc_chip_id;       /* e.g. 0x001003a0 */
extern uint32_t winc_version;       /* low half: firmware, high half: minimum driver */
extern uint8_t  winc_mac[6];        /* the module's own MAC address */
extern uint16_t winc_bus_errors;    /* SPI exchanges that had to be retried */
extern uint16_t winc_send_no_room;  /* requests refused: the module had no buffer free */
extern uint32_t winc_last_value;    /* on a start-up timeout: the last value read */
extern uint8_t  winc_seen[12];      /* reply bytes of the last failed SPI exchange */
extern uint8_t  winc_seen_n;

/* Send one request: a control part and optionally a data part placed
 * data_offset bytes after the start of the control part. Returns 1 if the
 * module took it. */
uint8_t winc_send(uint8_t group, uint8_t op, const void *ctrl, uint16_t ctrl_len,
                  const void *data, uint16_t data_len, uint16_t data_offset);

/* If the module has a message for us, fetch its header and call
 * handler(group, op, len, addr). The handler reads the body with
 * winc_receive() (at addr, addr + offset, ...). Whatever it does not read is
 * released after it returns. Returns 1 if a message was handled. */
typedef void (*winc_handler_t)(uint8_t group, uint8_t op, uint16_t len, uint32_t addr);
uint8_t winc_poll(winc_handler_t handler);

/* Read part of the current message's body into buf. With done = 1 the
 * message is released afterwards (then nothing more may be read from it). */
uint8_t winc_receive(uint32_t addr, void *buf, uint16_t n, uint8_t done);

/* Called from a handler: keep the current message open after the handler
 * returns, so its body can be read later with winc_receive() (this is how
 * received TCP data is handed out a piece at a time, straight from the
 * module's memory, as the Arduino port does). While a message is held the
 * module sends no other messages and winc_poll() does nothing; sending
 * requests still works. winc_release() (or winc_receive with done = 1)
 * ends it. */
void winc_hold(void);
void winc_release(void);
uint8_t winc_holding(void);

/* Little-endian helpers for message fields. */
static inline uint16_t winc_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t winc_u32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#endif
