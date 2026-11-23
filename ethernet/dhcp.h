/*
 * dhcp.h - the message half of DHCP: asking the LAN's router for an address
 * instead of having one typed in.
 *
 * The conversation is four broadcast UDP messages (client port 68, server
 * port 67):
 *
 *   we:     DISCOVER   "is there a DHCP server? I am MAC so-and-so"
 *   server: OFFER      "you could have 192.168.1.57"
 *   we:     REQUEST    "yes please, 192.168.1.57 from you"
 *   server: ACK        "yours for 86400 seconds; mask ..., router ..."
 *
 * The address is a lease. Before it runs out (we ask at half time) we send
 * another REQUEST naming the address we hold, and the server ACKs again.
 *
 * This file only builds and reads the messages. It does no input or
 * output, so it is the same code on the chip and in the PC test
 * (dhcp_test.c). Sending and receiving is net_w5500.c's job. Formats are
 * from RFC 2131 and RFC 2132.
 */
#ifndef DHCP_H
#define DHCP_H

#include <stdint.h>

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

#define DHCP_MESSAGE_BYTES 300      /* every message we send is this long */

struct dhcp_lease {
    uint8_t  ip[4];
    uint8_t  subnet[4];
    uint8_t  gateway[4];        /* 0.0.0.0 if the server named none */
    uint8_t  server[4];         /* the server's identifier */
    uint32_t seconds;           /* how long the address is ours; 0 = not said */
};

/* Build a DISCOVER or REQUEST in buf (DHCP_MESSAGE_BYTES long).
 *   xid       a number of our choosing; the reply carries it back
 *   mac       our hardware address
 *   name      host name to announce (may be 0)
 *   offer     REQUEST answering an OFFER: the lease offered. Otherwise 0.
 *   holding   REQUEST renewing: the address we hold. Otherwise 0.
 * A DISCOVER passes 0 for both. */
void dhcp_build(uint8_t *buf, uint8_t type, uint32_t xid, const uint8_t mac[6],
                const char *name, const struct dhcp_lease *offer, const uint8_t *holding);

/* Read a received message. Returns its type (DHCP_OFFER, DHCP_ACK,
 * DHCP_NAK) if it is a well-formed reply to us (same xid, same MAC), and
 * fills *out for an OFFER or ACK. Returns 0 for anything else. Safe on any
 * bytes at all, including a message cut short. */
uint8_t dhcp_parse(const uint8_t *msg, uint16_t len, uint32_t xid, const uint8_t mac[6],
                   struct dhcp_lease *out);

#endif
