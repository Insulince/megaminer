/*
 * settings.h - the miner's changeable settings: WiFi, the console password,
 * the pool, SV2's certificate check, and (Ethernet build only) addresses.
 *
 * They live in the chip's EEPROM (storage that survives power-off) and are
 * changed by typing commands on a console (telnet, the web page, or the
 * serial port at 250000 8N1). Spaces are dropped from what is typed, except
 * in the two wifi.* lines (network names and passwords may contain them).
 *
 *   show                      print the settings (passwords never)
 *   wifi.ssid=My Network      WiFi network name
 *   wifi.password=my secret   WiFi password; empty means an open network
 *   console.password=secret   password for typing commands over the network
 *                             (telnet, web page); empty removes it. The
 *                             serial console never asks for it, so a
 *                             forgotten one is cleared there (or with the
 *                             start-up button menu).
 *   pool.address=a.b.c.d      the pool's IP address (no DNS on the chip)
 *   pool.port=23415           and its TCP port
 *   pool.worker=megaminer     worker name (up to 32 characters)
 *   time_server=a.b.c.d       SNTP server: the console's time stamps, and
 *                             (SV2) the certificate's validity window
 *   sv2.authority_key=<key>   the key the pool's certificate must be signed
 *                             by (handled by miner_sv2: 64 hex digits, or
 *                             the base58check text SV2 pools print)
 *   sv2.check_cert=1          1 = verify the pool's certificate,
 *                             0 = accept any server (no time needed either)
 *   ethernet.ip=, ethernet.netmask=, ethernet.gateway=, ethernet.mac=,
 *   ethernet.dhcp=0|1         the W5500 build only (NET=w5500): its own
 *                             addresses, or DHCP with those as the fallback.
 *                             The WiFi module does DHCP itself.
 *   reboot                    restart the chip (and the WiFi module)
 *   update                    restart into the bootloader for new firmware
 *   mem                       how close the stack has come to the data
 *   net                       the network's state
 *   resetbest                 forget the best hash ever found (see hashrate.h)
 *   resetstats                restart the hash rate averages from now
 *   resetrestarts             zero the watchdog's restart counts
 *   defaults                  go back to the values built into the firmware
 *                             (except wifi.* and console.password, so that
 *                             doing it over the network does not cut the
 *                             miner off the network)
 *   save                      store the settings and use them
 *   help
 *
 * A change only takes effect, and is only stored, at "save".
 *
 * When the EEPROM holds nothing valid, the built-in defaults from
 * net_config.h are used. Flashing new firmware erases the EEPROM unless the
 * chip's EESAVE fuse is set (high fuse bit 3 programmed; see the README).
 * With it set, settings survive a "make flash", except when the new
 * firmware stores them in a different layout (the marker below changes).
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>

#define WORKER_MAX 32
#define SSID_MAX   32
#define PASS_MAX   64
#define CONSOLE_PASS_MAX 16

struct settings {
    uint8_t  ip[4];
    uint8_t  subnet[4];
    uint8_t  gateway[4];
    uint8_t  mac[6];
    uint8_t  server_ip[4];
    uint16_t server_port;
    char     worker[WORKER_MAX + 1];
    uint8_t  ntp_ip[4];         /* time server (time stamps; SV2's certificate check) */
    /* Stratum V2 only (miner_sv2); the other programs ignore these. */
    uint8_t  check_cert;        /* 1 = verify the server's certificate (default) */
    uint8_t  authority_set;     /* 1 once an authority key has been given */
    uint8_t  authority[32];     /* the key the server's certificate must be signed by */
    uint8_t  dhcp;              /* 1 = get ip, subnet and gateway from a DHCP server */
    /* WiFi (added 2026-11-18; settings stored by older firmware load with
     * these empty). */
    char     ssid[SSID_MAX + 1];  /* network name; empty = not set */
    char     pass[PASS_MAX + 1];  /* WPA2 passphrase (8-63) or 64 hex digits; empty = open network */
    /* Network console and web page (added 2026-11-18). */
    char     console_pass[CONSOLE_PASS_MAX + 1];  /* empty = commands need no password */
};

/* The settings in use. Filled by settings_load(). */
extern struct settings settings;

/* Read the settings from EEPROM, or use the defaults if nothing valid is
 * stored. Call once at start-up, before net_init(). */
void settings_load(void);

/* The closest the stack has come to the program's data since start-up, in
 * bytes (see settings.c; also the "mem" command). */
uint16_t stack_least_free(void);

/* Print the settings on the serial console. */
void settings_show(void);

/* Handle one line typed on the console. */
#define SETTINGS_HANDLED  0     /* dealt with (or rejected with a message) */
#define SETTINGS_APPLY    1     /* "save": stored; restart the network to use them */
uint8_t settings_command(const char *line);

/* 1 if this network name and password would be accepted by ssid= and pass=
 * (printable, the right lengths). */
uint8_t settings_wifi_ok(const char *ssid, const char *pass);

/* For "update" (also from the web page): 1 = a bootloader is installed and
 * switched on, and the request for it is set; the caller restarts the chip. */
uint8_t settings_request_update(void);

/* After SETTINGS_APPLY: which groups of settings that save changed, compared
 * with what was stored before it (and so with what the program started or
 * last restarted with). A program may restart only what is needed; the
 * simple ones ignore this and restart the network on every save. When the
 * EEPROM held nothing valid in the current layout, everything counts as
 * changed. */
#define SETTINGS_CHANGED_NET   0x01  /* wifi.*, ethernet.* */
#define SETTINGS_CHANGED_POOL  0x02  /* pool.*, time_server, sv2.* */
extern uint8_t settings_changed;

#endif
