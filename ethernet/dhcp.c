/*
 * dhcp.c - see dhcp.h.
 *
 * A DHCP message is an old BOOTP message with a list of options on the end:
 *
 *   offset  bytes
 *     0       1   op: 1 = from a client, 2 = from a server
 *     1       1   hardware type: 1 = Ethernet
 *     2       1   hardware address length: 6
 *     4       4   xid
 *    10       2   flags: 0x8000 = "answer by broadcast, I have no address yet"
 *    12       4   ciaddr: the client's address, if it already has one
 *    16       4   yiaddr: "your address", filled in by the server
 *    28      16   client hardware address
 *   236       4   the marker 63 82 53 63 that says options follow
 *   240       -   options: one byte code, one byte length, then the value;
 *                 code 0 is padding (no length), code 255 ends the list
 */
#include "dhcp.h"

#include <string.h>

#define OPTIONS 240

#define OPT_SUBNET     1
#define OPT_ROUTER     3
#define OPT_HOST_NAME  12
#define OPT_REQUESTED  50
#define OPT_LEASE_TIME 51
#define OPT_TYPE       53
#define OPT_SERVER     54
#define OPT_WANTED     55
#define OPT_CLIENT_ID  61
#define OPT_END        255

static const uint8_t COOKIE[4] = { 0x63, 0x82, 0x53, 0x63 };

static uint8_t *option(uint8_t *p, uint8_t code, const uint8_t *value, uint8_t len)
{
    *p++ = code;
    *p++ = len;
    memcpy(p, value, len);
    return p + len;
}

void dhcp_build(uint8_t *buf, uint8_t type, uint32_t xid, const uint8_t mac[6],
                const char *name, const struct dhcp_lease *offer, const uint8_t *holding)
{
    static const uint8_t wanted[2] = { OPT_SUBNET, OPT_ROUTER };
    uint8_t id[7];
    uint8_t *p;

    memset(buf, 0, DHCP_MESSAGE_BYTES);
    buf[0] = 1;
    buf[1] = 1;
    buf[2] = 6;
    buf[4] = (uint8_t)(xid >> 24);
    buf[5] = (uint8_t)(xid >> 16);
    buf[6] = (uint8_t)(xid >> 8);
    buf[7] = (uint8_t)xid;
    if (holding)
        memcpy(buf + 12, holding, 4);       /* we can be answered directly */
    else
        buf[10] = 0x80;                     /* we cannot: answer by broadcast */
    memcpy(buf + 28, mac, 6);
    memcpy(buf + 236, COOKIE, 4);

    p = buf + OPTIONS;
    p = option(p, OPT_TYPE, &type, 1);
    id[0] = 1;                              /* "an Ethernet address follows" */
    memcpy(id + 1, mac, 6);
    p = option(p, OPT_CLIENT_ID, id, 7);
    if (offer) {
        p = option(p, OPT_REQUESTED, offer->ip, 4);
        p = option(p, OPT_SERVER, offer->server, 4);
    }
    if (name && *name) {
        uint8_t len = (uint8_t)strlen(name);

        if (len > 32)
            len = 32;
        p = option(p, OPT_HOST_NAME, (const uint8_t *)name, len);
    }
    p = option(p, OPT_WANTED, wanted, sizeof wanted);
    *p = OPT_END;
}

uint8_t dhcp_parse(const uint8_t *msg, uint16_t len, uint32_t xid, const uint8_t mac[6],
                   struct dhcp_lease *out)
{
    struct dhcp_lease lease;
    uint8_t type = 0, have_server = 0, have_subnet = 0;
    uint16_t at = OPTIONS;

    if (len < OPTIONS || msg[0] != 2 || msg[1] != 1 || msg[2] != 6)
        return 0;
    if (msg[4] != (uint8_t)(xid >> 24) || msg[5] != (uint8_t)(xid >> 16) ||
        msg[6] != (uint8_t)(xid >> 8) || msg[7] != (uint8_t)xid)
        return 0;
    if (memcmp(msg + 28, mac, 6) != 0 || memcmp(msg + 236, COOKIE, 4) != 0)
        return 0;

    memset(&lease, 0, sizeof lease);
    memcpy(lease.ip, msg + 16, 4);

    while (at < len) {
        uint8_t code = msg[at++];
        uint8_t size;
        const uint8_t *value;

        if (code == 0)
            continue;
        if (code == OPT_END || at >= len)
            break;
        size = msg[at++];
        if (size > len - at)
            break;                          /* runs off the end: stop here */
        value = msg + at;
        at += size;

        if (code == OPT_TYPE && size == 1) {
            type = value[0];
        } else if (code == OPT_SUBNET && size == 4) {
            memcpy(lease.subnet, value, 4);
            have_subnet = 1;
        } else if (code == OPT_ROUTER && size >= 4) {
            memcpy(lease.gateway, value, 4);        /* the first one listed */
        } else if (code == OPT_SERVER && size == 4) {
            memcpy(lease.server, value, 4);
            have_server = 1;
        } else if (code == OPT_LEASE_TIME && size == 4) {
            lease.seconds = (uint32_t)value[0] << 24 | (uint32_t)value[1] << 16 |
                            (uint32_t)value[2] << 8 | value[3];
        }
    }

    if (type == DHCP_NAK)
        return type;
    if (type != DHCP_OFFER && type != DHCP_ACK)
        return 0;
    if (!have_server || (lease.ip[0] | lease.ip[1] | lease.ip[2] | lease.ip[3]) == 0)
        return 0;
    if (!have_subnet) {
        lease.subnet[0] = lease.subnet[1] = lease.subnet[2] = 255;
        lease.subnet[3] = 0;
    }
    *out = lease;
    return type;
}
