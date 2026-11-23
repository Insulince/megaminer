/*
 * settings.c - see settings.h.
 */

#include <avr/boot.h>
#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <stddef.h>
#include <string.h>

#include "bootshare.h"
#include "net_config.h"
#include "settings.h"
#include "version.h"
#include "uart.h"
#include "watchdog.h"

/* watchdog.c is in miner_sv2 only: weak, so the other programs link without it. */
void watchdog_restart(uint8_t why) __attribute__((weak, noreturn));

struct settings settings;

/* The "net" command prints this when the program has network code (net.h). */
void net_report(void) __attribute__((weak));

/* After a watchdog reset (the "reboot" command) the watchdog stays switched
 * on, and would reset the chip again 15 ms later, forever, before main()
 * could do anything about it. This runs first thing after reset, before
 * main() and before memory is set up ("init3"), and switches it off. Every
 * program that has the reboot command links this file. */
void early_watchdog_off(void) __attribute__((naked, used, section(".init3")));
void early_watchdog_off(void)
{
    MCUSR = 0;                  /* the reset flag must be cleared first */
    wdt_disable();
}

/* What is stored in EEPROM: a marker, the settings, and a checksum. A blank
 * (erased) EEPROM reads as all 0xff, which fails the marker test. */
#define STORED_MAGIC 0xB110

struct stored {
    uint16_t        magic;
    struct settings settings;
    uint16_t        checksum;
};

#define EEPROM_ADDRESS ((void *)0)

static uint16_t checksum_bytes(const void *s, uint16_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    uint16_t sum = 0x1284;

    for (uint16_t i = 0; i < n; i++)
        sum = (uint16_t)(sum * 31 + p[i]);
    return sum;
}

static uint16_t checksum(const struct settings *s)
{
    return checksum_bytes(s, sizeof *s);
}

/* The bootloader's copy of wifi.ssid, wifi.password and console.password, in its own fixed
 * format and place (bootshare.h: the bootloader never
 * changes, so it must not depend on this file's layout, which does). Only
 * bytes that differ are written, so calling this at every start costs
 * nothing once it is in step. */
static void boot_creds_sync(void)
{
    struct boot_creds c;

    memset(&c, 0, sizeof c);
    c.magic = BOOT_CREDS_MAGIC;
    strncpy(c.ssid, settings.ssid, sizeof c.ssid - 1);
    strncpy(c.pass, settings.pass, sizeof c.pass - 1);
    strncpy(c.console_pass, settings.console_pass, sizeof c.console_pass - 1);
    c.sum = BOOT_CREDS_SUM(&c);
    eeprom_update_block(&c, BOOT_CREDS_EE, sizeof c);
}

/* "update": hand over to the bootloader for a firmware update over WiFi.
 * Returns 0 (and says why) if there is no working bootloader; otherwise
 * sets the request, and the caller restarts the chip. */
uint8_t settings_request_update(void)
{
    if (pgm_read_word_far(BOOT_START) == 0xffff) {
        uart_puts_P(PSTR("update: no bootloader on this chip (flash it with the USBasp: make flash)\r\n"));
        return 0;
    }
    if (boot_lock_fuse_bits_get(GET_HIGH_FUSE_BITS) & 0x01) {
        uart_puts_P(PSTR("update: the bootloader is there but not switched on "
                         "(BOOTRST fuse: high fuse 0x90)\r\n"));
        return 0;
    }
    eeprom_update_byte(BOOT_REQUEST_EE, BOOT_REQ_UPDATE);
    uart_puts_P(PSTR("update: restarting into the bootloader; send the new firmware "
                     "(web page, or make ota)\r\n"));
    return 1;
}

static void use_defaults(void)
{
    static const uint8_t ip[4]      = NET_IP;
    static const uint8_t subnet[4]  = NET_SUBNET;
    static const uint8_t gateway[4] = NET_GATEWAY;
    static const uint8_t mac[6]     = NET_MAC;
    static const uint8_t server[4]  = NET_SERVER_IP;
    static const uint8_t ntp[4]     = NET_NTP_IP;

    memset(&settings, 0, sizeof settings);
    memcpy(settings.ip, ip, 4);
    memcpy(settings.subnet, subnet, 4);
    memcpy(settings.gateway, gateway, 4);
    memcpy(settings.mac, mac, 6);
    memcpy(settings.server_ip, server, 4);
    settings.server_port = NET_SERVER_PORT;
    strncpy(settings.worker, NET_WORKER, WORKER_MAX);
    memcpy(settings.ntp_ip, ntp, 4);
    settings.check_cert = 1;
}

/* ---- how much RAM the stack has needed ---------------------------------
 * The RAM between the program's data (ending at __heap_start; no malloc is
 * used) and the stack is filled with a marker byte once, early. The stack
 * grows down into it; whatever the stack ever touched no longer holds the
 * marker. Counting the untouched bytes from the bottom gives the closest the
 * stack has ever come to the data: if that reaches 0, the stack has
 * overwritten data (a crash or worse). The "mem" command prints it. */

extern uint8_t __heap_start;
#define STACK_PAINT 0xC5
static uint8_t painted;

static void stack_paint(void)
{
    uint8_t *p = &__heap_start;
    uint8_t *end = (uint8_t *)SP - 64;  /* leave this function's own frame alone */

    while (p < end)
        *p++ = STACK_PAINT;
    painted = 1;
}

uint16_t stack_least_free(void)
{
    const uint8_t *p = &__heap_start;
    uint16_t n = 0;

    if (!painted)
        return 0;
    while (p < (const uint8_t *)RAMEND && *p == STACK_PAINT) {
        p++;
        n++;
    }
    return n;
}

void settings_load(void)
{
    struct stored st;

    stack_paint();                      /* every program calls this early, from main() */

    eeprom_read_block(&st, EEPROM_ADDRESS, sizeof st);
    if (st.magic == STORED_MAGIC && st.checksum == checksum(&st.settings)) {
        settings = st.settings;
        uart_puts_P(PSTR("settings: loaded from EEPROM\r\n"));
    } else {
        use_defaults();
        uart_puts_P(PSTR("settings: nothing stored, using built-in defaults\r\n"));
    }
    settings.worker[WORKER_MAX] = '\0';
    settings.ssid[SSID_MAX] = '\0';
    settings.pass[PASS_MAX] = '\0';
    settings.console_pass[CONSOLE_PASS_MAX] = '\0';
    boot_creds_sync();
}

uint8_t settings_changed;

/* Bytes [from, to) of the two settings differ? */
#define DIFFERS(a, b, from, to) \
    (memcmp((const uint8_t *)(a) + offsetof(struct settings, from), \
            (const uint8_t *)(b) + offsetof(struct settings, from), \
            offsetof(struct settings, to) - offsetof(struct settings, from)) != 0)

/* Compare what is about to be stored with what is stored now. The struct
 * (about 240 bytes) is reused for the write, so this costs no extra RAM. */
static void note_changes(struct stored *st)
{
    const struct settings *old = &st->settings;
    const struct settings *now = &settings;

    eeprom_read_block(st, EEPROM_ADDRESS, sizeof *st);
    if (st->magic != STORED_MAGIC || st->checksum != checksum(old)) {
        settings_changed = SETTINGS_CHANGED_NET | SETTINGS_CHANGED_POOL;
        return;
    }
    settings_changed = 0;
    /* Field order (settings.h): ip subnet gateway mac | server_ip server_port
     * worker ntp_ip check_cert authority_set authority | dhcp ssid pass |
     * console_pass. Whole byte ranges are compared, including any padding
     * and the bytes after the end of each text; at worst that reports a
     * change that was not one (a needless restart), never the reverse. */
    if (DIFFERS(old, now, ip, server_ip) || DIFFERS(old, now, dhcp, console_pass))
        settings_changed |= SETTINGS_CHANGED_NET;
    if (DIFFERS(old, now, server_ip, dhcp))
        settings_changed |= SETTINGS_CHANGED_POOL;
}

static void save(void)
{
    struct stored st;

    note_changes(&st);
    memset(&st, 0, sizeof st);
    st.magic = STORED_MAGIC;
    st.settings = settings;
    st.checksum = checksum(&settings);
    /* "update" only rewrites bytes that differ, which spares the EEPROM
     * (each cell is good for about 100,000 writes). */
    eeprom_update_block(&st, EEPROM_ADDRESS, sizeof st);
    boot_creds_sync();
}

/* ---- printing ----------------------------------------------------------- */

/* name is in flash: put_ip(PSTR("  ip="), ...). */
static void put_ip(const char *name, const uint8_t ip[4])
{
    uart_puts_P(name);
    for (uint8_t i = 0; i < 4; i++) {
        if (i)
            uart_putc('.');
        uart_put_u32(ip[i]);
    }
    uart_puts_P(PSTR("\r\n"));
}

/* Grouped by section, in the order help lists them. The ethernet.* settings
 * only exist in the Ethernet build (make ... NET=w5500): on WiFi the module
 * does DHCP itself and has its own MAC. */
void settings_show(void)
{
    uart_puts_P(PSTR("  wifi.ssid="));
    if (settings.ssid[0]) {
        uart_putc('"');
        uart_puts(settings.ssid);
        uart_putc('"');
    } else {
        uart_puts_P(PSTR("(not set)"));
    }
    uart_puts_P(settings.pass[0] ? PSTR("\r\n  wifi.password=(set)")    /* not even its length */
                                 : PSTR("\r\n  wifi.password=(none: open network)"));
    uart_puts_P(settings.console_pass[0]
                ? PSTR("\r\n  console.password=(set)\r\n")
                : PSTR("\r\n  console.password=(none: anyone on the network can type commands)\r\n"));
    put_ip(PSTR("  pool.address="), settings.server_ip);
    uart_puts_P(PSTR("  pool.port="));
    uart_put_u32(settings.server_port);
    uart_puts_P(PSTR("\r\n  pool.worker="));
    uart_puts(settings.worker);
    uart_puts_P(PSTR("\r\n"));
    put_ip(PSTR("  time_server="), settings.ntp_ip);
    uart_puts_P(PSTR("  sv2.authority_key="));
    if (settings.authority_set)
        uart_put_hex(settings.authority, 32);
    else
        uart_puts_P(PSTR("(not set)"));
    uart_puts_P(settings.check_cert ? PSTR("\r\n  sv2.check_cert=1\r\n") : PSTR("\r\n  sv2.check_cert=0\r\n"));
#ifdef NET_W5500
    put_ip(PSTR("  ethernet.ip="), settings.ip);
    put_ip(PSTR("  ethernet.netmask="), settings.subnet);
    put_ip(PSTR("  ethernet.gateway="), settings.gateway);
    uart_puts_P(PSTR("  ethernet.mac="));
    for (uint8_t i = 0; i < 6; i++) {
        if (i)
            uart_putc(':');
        uart_put_hex(&settings.mac[i], 1);
    }
    uart_puts_P(settings.dhcp ? PSTR("\r\n  ethernet.dhcp=1 (ip, netmask and gateway above are only the fallback)\r\n")
                              : PSTR("\r\n  ethernet.dhcp=0\r\n"));
#endif
}

/* ---- parsing ------------------------------------------------------------ */

/* Read a decimal number of at most `max`. Moves *p past it. Returns 1 on
 * success. */
static uint8_t parse_number(const char **p, uint16_t max, uint16_t *out)
{
    uint32_t n = 0;
    uint8_t digits = 0;

    while (**p >= '0' && **p <= '9') {
        n = n * 10 + (uint8_t)(**p - '0');
        if (n > max)
            return 0;
        (*p)++;
        digits++;
    }
    *out = (uint16_t)n;
    return digits > 0;
}

/* "192.168.1.22" -> 4 bytes. Nothing may follow it. */
static uint8_t parse_ip(const char *s, uint8_t out[4])
{
    uint8_t tmp[4];

    for (uint8_t i = 0; i < 4; i++) {
        uint16_t n;
        if (!parse_number(&s, 255, &n))
            return 0;
        tmp[i] = (uint8_t)n;
        if (i < 3 && *s++ != '.')
            return 0;
    }
    if (*s)
        return 0;
    memcpy(out, tmp, 4);
    return 1;
}

static int8_t hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

#ifdef NET_W5500
/* "02:62:72:65:61:64" -> 6 bytes (ethernet.mac; WiFi uses the module's own). */
static uint8_t parse_mac(const char *s, uint8_t out[6])
{
    uint8_t tmp[6];

    for (uint8_t i = 0; i < 6; i++) {
        int8_t hi = hex_value(s[0]);
        int8_t lo = hi < 0 ? -1 : hex_value(s[1]);
        if (hi < 0 || lo < 0)
            return 0;
        tmp[i] = (uint8_t)((hi << 4) | lo);
        s += 2;
        if (i < 5 && *s++ != ':')
            return 0;
    }
    if (*s)
        return 0;
    memcpy(out, tmp, 6);
    return 1;
}
#endif

/* A worker name goes inside a JSON string, so keep it to plain characters. */
static uint8_t parse_worker(const char *s, char out[WORKER_MAX + 1])
{
    uint8_t len = 0;

    for (const char *p = s; *p; p++, len++) {
        if (len >= WORKER_MAX || *p <= ' ' || *p > '~' || *p == '"' || *p == '\\')
            return 0;
    }
    if (len == 0)
        return 0;
    strcpy(out, s);
    return 1;
}

/* WiFi network name: 1-32 printable characters (spaces allowed). */
static uint8_t parse_ssid(const char *s, char out[SSID_MAX + 1])
{
    uint8_t len = 0;

    for (const char *p = s; *p; p++, len++) {
        if (len >= SSID_MAX || *p < ' ' || *p > '~')
            return 0;
    }
    if (len == 0)
        return 0;
    strcpy(out, s);
    return 1;
}

/* WPA2 password: 8-63 printable characters, or exactly 64 hex digits (the
 * raw key); empty = open network. */
static uint8_t parse_pass(const char *s, char out[PASS_MAX + 1])
{
    uint8_t len = 0, all_hex = 1;

    for (const char *p = s; *p; p++, len++) {
        if (len >= PASS_MAX || *p < ' ' || *p > '~')
            return 0;
        if (hex_value(*p) < 0)
            all_hex = 0;
    }
    if (len != 0 && (len < 8 || (len == 64 && !all_hex)))
        return 0;
    strcpy(out, s);
    return 1;
}

/* settings.h: the same checks as wifi.ssid= and wifi.password=, for details that arrive
 * another way (WiFi setup mode). */
uint8_t settings_wifi_ok(const char *ssid, const char *pass)
{
    char s[SSID_MAX + 1], p[PASS_MAX + 1];

    return parse_ssid(ssid, s) && parse_pass(pass, p);
}

/* If line starts with `name` (in flash: PSTR), return what follows it;
 * otherwise 0. Names are kept in flash: in RAM, every one of them would
 * cost RAM for good. */
static const char *after(const char *line, const char *name)
{
    size_t n = strlen_P(name);
    return strncmp_P(line, name, n) == 0 ? line + n : 0;
}

static uint8_t is(const char *line, const char *name)
{
    return strcmp_P(line, name) == 0;
}

uint8_t settings_command(const char *line)
{
    const char *v;
    uint8_t ok = 1;

    uart_puts_P(PSTR("cmd: "));
    if (after(line, PSTR("wifi.password=")))
        uart_puts_P(PSTR("wifi.password=(hidden)"));
    else if (after(line, PSTR("console.password=")))
        uart_puts_P(PSTR("console.password=(hidden)"));
    else
        uart_puts(line);
    uart_puts_P(PSTR("\r\n"));

    if (is(line, PSTR("show"))) {
        settings_show();
        return SETTINGS_HANDLED;
    }
    if (is(line, PSTR("save"))) {
        save();
        uart_puts_P(PSTR("settings: saved"));
        if (settings_changed & SETTINGS_CHANGED_NET)
            uart_puts_P(PSTR(" (network settings changed)"));
        else if (settings_changed & SETTINGS_CHANGED_POOL)
            uart_puts_P(PSTR(" (pool settings changed)"));
        else
            uart_puts_P(PSTR(" (no network or pool setting changed)"));
        uart_puts_P(PSTR("; now:\r\n"));
        settings_show();
        return SETTINGS_APPLY;
    }
    if (is(line, PSTR("net"))) {
        if (net_report)
            net_report();
        else
            uart_puts_P(PSTR("net: no network code in this program\r\n"));
        return SETTINGS_HANDLED;
    }
    if (is(line, PSTR("mem"))) {
        uart_puts_P(PSTR("mem: stack has come within "));
        uart_put_u32(stack_least_free());
        uart_puts_P(PSTR(" bytes of the data since start-up (0 = it has overwritten some); now "));
        uart_put_u32((uint16_t)SP - (uint16_t)&__heap_start);
        uart_puts_P(PSTR(" bytes free\r\n"));
        return SETTINGS_HANDLED;
    }
    if (is(line, PSTR("reboot")) || is(line, PSTR("update"))) {
        /* The watchdog, set to its shortest time and never fed, resets the
         * whole chip (and with it the WiFi module, whose EN pin falls back
         * to its pull-down). early_watchdog_off() undoes it at start-up. */
        if (line[0] == 'u' && !settings_request_update())
            return SETTINGS_HANDLED;
        uart_puts_P(PSTR("settings: rebooting\r\n"));
        if (watchdog_restart)
            watchdog_restart(WATCHDOG_ASKED);   /* the same, remembering that it was asked for */
        cli();
        wdt_enable(WDTO_15MS);
        for (;;) {
        }
    }
    if (is(line, PSTR("defaults"))) {
        /* Keep what reaches the network: restoring the built-in (empty) WiFi
         * name or password over the network console would cut it off. */
        char ssid[SSID_MAX + 1], pass[PASS_MAX + 1], cpass[CONSOLE_PASS_MAX + 1];
        memcpy(ssid, settings.ssid, sizeof ssid);
        memcpy(pass, settings.pass, sizeof pass);
        memcpy(cpass, settings.console_pass, sizeof cpass);
        use_defaults();
        memcpy(settings.ssid, ssid, sizeof ssid);
        memcpy(settings.pass, pass, sizeof pass);
        memcpy(settings.console_pass, cpass, sizeof cpass);
        uart_puts_P(PSTR("settings: built-in defaults restored, except wifi.* and console.password "
                         "(not saved yet):\r\n"));
        settings_show();
        return SETTINGS_HANDLED;
    }
    if (is(line, PSTR("help"))) {
        uart_puts_P(PSTR("megaminer " FIRMWARE_VERSION " (" PROGRAM_NAME ")\r\n"
                  "commands: show  save  defaults  help  reboot  mem  net  resetbest  resetstats  resetrestarts\r\n"
                  "  update   restart into the bootloader to take a new firmware over WiFi\r\n"
                  "settings (name=value; no spaces except in the two wifi ones):\r\n"
                  "  wifi.ssid=NAME  wifi.password=PASSWORD\r\n"
                  "  console.password=PASSWORD   for commands over the network (empty: none)\r\n"
                  "  pool.address=a.b.c.d  pool.port=N  pool.worker=NAME\r\n"
                  "  time_server=a.b.c.d   SNTP server: log time stamps, SV2's certificate window\r\n"
                  "  sv2.authority_key=KEY  sv2.check_cert=0|1\r\n"));
#ifdef NET_W5500
        uart_puts_P(PSTR("  ethernet.ip=a.b.c.d  ethernet.netmask=a.b.c.d  ethernet.gateway=a.b.c.d\r\n"
                         "  ethernet.mac=xx:xx:xx:xx:xx:xx  ethernet.dhcp=0|1\r\n"));
#endif
        uart_puts_P(PSTR("changes take effect at \"save\"\r\n"));
        return SETTINGS_HANDLED;
    }

    if ((v = after(line, PSTR("wifi.ssid="))) != 0) {
        ok = parse_ssid(v, settings.ssid);
    } else if ((v = after(line, PSTR("wifi.password="))) != 0) {
        ok = parse_pass(v, settings.pass);
    } else if ((v = after(line, PSTR("console.password="))) != 0) {
        ok = strlen(v) <= CONSOLE_PASS_MAX;
        if (ok)
            strcpy(settings.console_pass, v);
    } else if ((v = after(line, PSTR("pool.address="))) != 0) {
        ok = parse_ip(v, settings.server_ip);
    } else if ((v = after(line, PSTR("pool.port="))) != 0) {
        uint16_t n = 0;
        ok = parse_number(&v, 65535, &n) && *v == '\0' && n != 0;
        if (ok)
            settings.server_port = n;
    } else if ((v = after(line, PSTR("pool.worker="))) != 0) {
        ok = parse_worker(v, settings.worker);
    } else if ((v = after(line, PSTR("sv2.check_cert="))) != 0) {
        ok = (v[0] == '0' || v[0] == '1') && v[1] == '\0';
        if (ok)
            settings.check_cert = (uint8_t)(v[0] - '0');
    } else if ((v = after(line, PSTR("time_server="))) != 0) {
        ok = parse_ip(v, settings.ntp_ip);
#ifdef NET_W5500
    } else if ((v = after(line, PSTR("ethernet.ip="))) != 0) {
        ok = parse_ip(v, settings.ip);
    } else if ((v = after(line, PSTR("ethernet.netmask="))) != 0) {
        ok = parse_ip(v, settings.subnet);
    } else if ((v = after(line, PSTR("ethernet.gateway="))) != 0) {
        ok = parse_ip(v, settings.gateway);
    } else if ((v = after(line, PSTR("ethernet.mac="))) != 0) {
        ok = parse_mac(v, settings.mac);
    } else if ((v = after(line, PSTR("ethernet.dhcp="))) != 0) {
        ok = (v[0] == '0' || v[0] == '1') && v[1] == '\0';
        if (ok)
            settings.dhcp = (uint8_t)(v[0] - '0');
#endif
    } else {
        uart_puts_P(PSTR("settings: unknown command; type help\r\n"));
        return SETTINGS_HANDLED;
    }

    uart_puts_P(ok ? PSTR("settings: changed (type save to store and use it)\r\n")
                   : PSTR("settings: bad value, nothing changed\r\n"));
    return SETTINGS_HANDLED;
}
