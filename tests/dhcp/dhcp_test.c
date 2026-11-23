/*
 * dhcp_test.c - PC test of dhcp.c (run by `make test`).
 *
 * Checks the messages we build byte by byte at the offsets RFC 2131 gives,
 * reads back replies made up here the way a home router sends them, and
 * then feeds the reader damaged and random messages, which it must refuse
 * without reading past the end.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dhcp.h"

static int failures;

static void check(const char *what, int ok)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static const uint8_t MAC[6] = { 0x02, 0x62, 0x72, 0x65, 0x61, 0x64 };
#define XID 0x12345678UL

/* Find an option in a message we built. Returns its value, or 0. */
static const uint8_t *find(const uint8_t *msg, uint8_t code, uint8_t *len)
{
    uint16_t at = 240;

    while (at < DHCP_MESSAGE_BYTES && msg[at] != 255) {
        if (msg[at] == 0) {
            at++;
            continue;
        }
        if (msg[at] == code) {
            *len = msg[at + 1];
            return msg + at + 2;
        }
        at += 2 + msg[at + 1];
    }
    return 0;
}

/* A server's reply, as a router would send it. Returns its length. */
static uint16_t reply(uint8_t *msg, uint8_t type, uint8_t with_extras)
{
    static const uint8_t head[] = { 2, 1, 6, 0, 0x12, 0x34, 0x56, 0x78 };
    uint8_t *p = msg + 240;

    memset(msg, 0, 400);
    memcpy(msg, head, sizeof head);
    if (type != DHCP_NAK) {
        msg[16] = 192; msg[17] = 168; msg[18] = 1; msg[19] = 57;
    }
    memcpy(msg + 28, MAC, 6);
    msg[236] = 0x63; msg[237] = 0x82; msg[238] = 0x53; msg[239] = 0x63;
    *p++ = 53; *p++ = 1; *p++ = type;
    *p++ = 54; *p++ = 4; *p++ = 192; *p++ = 168; *p++ = 1; *p++ = 1;
    if (type != DHCP_NAK) {
        *p++ = 51; *p++ = 4; *p++ = 0; *p++ = 1; *p++ = 0x51; *p++ = 0x80;      /* 86400 */
        *p++ = 0;                                                               /* padding */
        *p++ = 1; *p++ = 4; *p++ = 255; *p++ = 255; *p++ = 255; *p++ = 0;
        *p++ = 3; *p++ = 8; *p++ = 192; *p++ = 168; *p++ = 1; *p++ = 1;         /* two routers */
        *p++ = 192; *p++ = 168; *p++ = 1; *p++ = 2;
        if (with_extras) {
            *p++ = 6; *p++ = 4; *p++ = 8; *p++ = 8; *p++ = 8; *p++ = 8;         /* DNS: ignored */
            *p++ = 15; *p++ = 3; *p++ = 'l'; *p++ = 'a'; *p++ = 'n';            /* domain: ignored */
        }
    }
    *p++ = 255;
    return (uint16_t)(p - msg);
}

int main(void)
{
    uint8_t msg[400], len;
    const uint8_t *v;
    struct dhcp_lease lease, offer;
    uint16_t n;

    /* ---- DISCOVER ---- */
    dhcp_build(msg, DHCP_DISCOVER, XID, MAC, "megaminer", 0, 0);
    check("discover op/htype/hlen", msg[0] == 1 && msg[1] == 1 && msg[2] == 6);
    check("discover xid", msg[4] == 0x12 && msg[5] == 0x34 && msg[6] == 0x56 && msg[7] == 0x78);
    check("discover broadcast flag", msg[10] == 0x80 && msg[11] == 0);
    check("discover no ciaddr", (msg[12] | msg[13] | msg[14] | msg[15]) == 0);
    check("discover chaddr", memcmp(msg + 28, MAC, 6) == 0);
    check("discover cookie", msg[236] == 0x63 && msg[237] == 0x82 && msg[238] == 0x53 && msg[239] == 0x63);
    v = find(msg, 53, &len);
    check("discover type", v && len == 1 && v[0] == 1);
    v = find(msg, 61, &len);
    check("discover client id", v && len == 7 && v[0] == 1 && memcmp(v + 1, MAC, 6) == 0);
    v = find(msg, 12, &len);
    check("discover host name", v && len == 9 && memcmp(v, "megaminer", 9) == 0);
    check("discover asks for nothing yet", !find(msg, 50, &len) && !find(msg, 54, &len));
    v = find(msg, 55, &len);
    check("discover wanted list", v && len == 2 && v[0] == 1 && v[1] == 3);

    /* ---- OFFER ---- */
    n = reply(msg, DHCP_OFFER, 1);
    check("offer read", dhcp_parse(msg, n, XID, MAC, &offer) == DHCP_OFFER);
    check("offer ip", offer.ip[0] == 192 && offer.ip[1] == 168 && offer.ip[2] == 1 && offer.ip[3] == 57);
    check("offer mask", offer.subnet[0] == 255 && offer.subnet[2] == 255 && offer.subnet[3] == 0);
    check("offer first router", offer.gateway[0] == 192 && offer.gateway[3] == 1);
    check("offer server", offer.server[0] == 192 && offer.server[3] == 1);
    check("offer lease", offer.seconds == 86400);

    /* ---- REQUEST answering it ---- */
    dhcp_build(msg, DHCP_REQUEST, XID, MAC, 0, &offer, 0);
    v = find(msg, 53, &len);
    check("request type", v && v[0] == 3);
    v = find(msg, 50, &len);
    check("request names the address", v && len == 4 && memcmp(v, offer.ip, 4) == 0);
    v = find(msg, 54, &len);
    check("request names the server", v && len == 4 && memcmp(v, offer.server, 4) == 0);
    check("request still broadcast, no ciaddr", msg[10] == 0x80 && (msg[12] | msg[15]) == 0);
    check("request no host name when none given", !find(msg, 12, &len));

    /* ---- REQUEST renewing ---- */
    dhcp_build(msg, DHCP_REQUEST, XID, MAC, 0, 0, offer.ip);
    check("renew ciaddr", memcmp(msg + 12, offer.ip, 4) == 0);
    check("renew not broadcast-flagged", msg[10] == 0);
    check("renew names neither", !find(msg, 50, &len) && !find(msg, 54, &len));

    /* ---- ACK, NAK ---- */
    n = reply(msg, DHCP_ACK, 0);
    check("ack read", dhcp_parse(msg, n, XID, MAC, &lease) == DHCP_ACK && lease.seconds == 86400);
    n = reply(msg, DHCP_NAK, 0);
    check("nak read", dhcp_parse(msg, n, XID, MAC, &lease) == DHCP_NAK);

    /* ---- things that must be refused ---- */
    n = reply(msg, DHCP_ACK, 1);
    check("someone else's xid", dhcp_parse(msg, n, XID + 1, MAC, &lease) == 0);
    msg[28] ^= 1;
    check("someone else's mac", dhcp_parse(msg, n, XID, MAC, &lease) == 0);
    msg[28] ^= 1;
    msg[0] = 1;
    check("a client's message", dhcp_parse(msg, n, XID, MAC, &lease) == 0);
    msg[0] = 2;
    msg[237] = 0;
    check("bad marker", dhcp_parse(msg, n, XID, MAC, &lease) == 0);
    msg[237] = 0x82;
    check("too short", dhcp_parse(msg, 239, XID, MAC, &lease) == 0);
    check("intact again", dhcp_parse(msg, n, XID, MAC, &lease) == DHCP_ACK);
    msg[16] = msg[17] = msg[18] = msg[19] = 0;
    check("ack of no address", dhcp_parse(msg, n, XID, MAC, &lease) == 0);

    /* Cut short at every possible length: must never crash, and whatever it
     * accepts must be a complete lease. */
    n = reply(msg, DHCP_ACK, 1);
    for (uint16_t cut = 0; cut < n; cut++) {
        uint8_t *copy = malloc(cut ? cut : 1);      /* exact size: a tool like ASan would catch overreads */
        uint8_t t;

        memcpy(copy, msg, cut);
        t = dhcp_parse(copy, cut, XID, MAC, &lease);
        check("cut message", t == 0 || (t == DHCP_ACK && lease.ip[3] == 57 && lease.server[3] == 1));
        free(copy);
    }

    /* An option whose length runs off the end. */
    n = reply(msg, DHCP_ACK, 0);
    msg[241] = 200;
    check("overlong option", dhcp_parse(msg, n, XID, MAC, &lease) == 0);

    /* Random option bytes after a valid header. */
    srand(20261116);
    for (int round = 0; round < 200000; round++) {
        n = reply(msg, DHCP_ACK, 0);
        for (int i = 240; i < 400; i++)
            msg[i] = (uint8_t)rand();
        (void)dhcp_parse(msg, (uint16_t)(240 + rand() % 161), XID, MAC, &lease);
    }

    printf(failures ? "dhcp: FAILED\n" : "dhcp: all tests passed\n");
    return failures != 0;
}
