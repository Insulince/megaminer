/*
 * net.h - the miner's view of "the network": one TCP connection to the
 * bridge program on the PC.
 *
 * This header is the whole contract. Each network chip gets its own file
 * that implements every function here (net_w5500.c now, net_winc.c later),
 * and the Makefile compiles exactly one of them (make NET=w5500). The mining
 * code only ever includes this header, so swapping the chip never touches it.
 *
 * Nothing here waits for the network. The main loop calls net_poll() every
 * so often between hashes, and asks net_connected() / net_recv() what, if
 * anything, has happened.
 *
 * Addresses (ours and the server's) come from the `settings` structure in
 * settings.h, which must be loaded before net_init() is called.
 */

#ifndef NET_H
#define NET_H

#include <stdint.h>

/* Bring up the network chip and give it its addresses. Returns 1 on
 * success, 0 if the chip does not answer. Call after reset, and again to
 * restart the network after the settings have changed. */
uint8_t net_init(void);

/* WiFi setup mode (button menu choice 2; call BEFORE net_init): the WiFi
 * module becomes an open access point with its own setup page, someone
 * picks a network and types its password on a phone, and the details are
 * TRIED before they are kept: only a successful join (with an address) saves
 * them (settings save, which also updates the bootloader's copy). A failed
 * try puts the old details back and offers the page again. Returns when
 * saved (1) or if the module does not start (0); a reset leaves it without
 * changing anything. Returns 0 at once where there is no WiFi. */
uint8_t net_wifi_setup(void);

/* 1 if the physical link is up (cable in, or radio joined). */
uint8_t net_link_up(void);

/* Keep the connection alive: open it, reconnect after a drop, tidy up when
 * the other side closes. Call regularly; returns quickly. */
void net_poll(void);

/* Background chores that must go on even while the program is busy or not
 * connected: on WiFi, the network console (TCP port 2323) and the web page
 * (port 80). Never opens the mining connection, so it is safe to call from
 * anywhere the serial console is serviced. Does nothing on the W5500. */
void net_service(void);

/* 1 while the TCP connection to the server is open (as of the last
 * net_poll). */
uint8_t net_connected(void);

/* Copy up to `max` received bytes into buf. Returns how many were copied;
 * 0 means nothing is waiting. */
uint16_t net_recv(uint8_t *buf, uint16_t max);

/* Send len bytes. Returns 1 once the other side has acknowledged them, 0 if
 * the connection failed. May take a few milliseconds. */
uint8_t net_send(const uint8_t *buf, uint16_t len);

/* Close the TCP connection from our side (for example after the server sent
 * something unacceptable). net_poll() will open a new one. */
void net_close(void);

/* Print one line about the network chip's state on the console, for
 * finding out why a connection does not come up: link, the address the
 * chip is actually using, and the connection's state. */
void net_report(void);

/* One question, one answer, over UDP: send req to ip:port and wait up to
 * about two seconds for a reply datagram. Returns the reply's length (cut to
 * resp_max), or 0 if none came. Unlike everything above, this one waits.
 * Used to ask a time server what time it is. */
uint16_t net_udp_exchange(const uint8_t ip[4], uint16_t port,
                          const uint8_t *req, uint16_t req_len,
                          uint8_t *resp, uint16_t resp_max);

#endif
