/*
 * net_config.h - the miner's built-in default settings. Edit to match the
 * LAN. These are only the defaults: settings typed on the serial console
 * and saved (see settings.h) take their place until the next flash.
 */

#ifndef NET_CONFIG_H
#define NET_CONFIG_H

/* The chip's own fixed address (no DHCP yet). */
#define NET_IP        { 192, 168, 1, 22 }
#define NET_SUBNET    { 255, 255, 255, 0 }
#define NET_GATEWAY   { 192, 168, 1, 1 }

/* Hardware (MAC) address. The W5500 has none built in. The 02 in the first
 * byte marks it as "locally administered": made up by us, not a
 * manufacturer's. It only has to be unique on this LAN. */
#define NET_MAC       { 0x02, 0x62, 0x72, 0x65, 0x61, 0x64 }

/* The machine the chip connects to, and the TCP port.
 *   4242   cmd/bridge -listen :4242        (miner_net)
 *   23339  mainnet solo DATUM gateway      (miner_sv1; set by the Makefile)
 *   23334  regtest DATUM gateway
 * Override the port when building:  make flash TARGET=... PORT=23334 */
#define NET_SERVER_IP   { 192, 168, 1, 165 }
#ifndef NET_SERVER_PORT
#define NET_SERVER_PORT 4242
#endif

/* Time server for the Stratum V2 certificate check (plain SNTP, UDP port
 * 123). There is no name lookup on the chip, so it has to be an address:
 * this is one of time.google.com's fixed addresses. */
#define NET_NTP_IP      { 216, 239, 35, 0 }

/* Stratum worker name. Override when building:  make flash ... WORKER=name */
#ifndef NET_WORKER
#define NET_WORKER "megaminer"
#endif

#endif
