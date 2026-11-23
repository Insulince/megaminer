/*
 * net_winc.c - net.h for the ATWINC1500 WiFi module (build with NET=winc).
 *
 * The module runs WiFi, WPA2, DHCP and TCP/UDP itself; this file only sends
 * it requests and reacts to its replies, through winc.c. Message formats are
 * from Microchip's driver as shipped in the Arduino WiFi101 library
 * (m2m_wifi.c, socket.c, m2m_socket_host_if.h); sizes and offsets were
 * checked by compiling those headers on the PC (2026-11-18).
 *
 * How it maps onto net.h:
 *   - net_init()     start the module, join settings.ssid, wait (up to 20 s)
 *                    for the address from the router's DHCP.
 *   - net_link_up()  joined AND has an address.
 *   - net_poll()     handles the module's messages; rejoins after a WiFi
 *                    drop; opens the TCP connection when there is none;
 *                    keeps one "receive" request outstanding.
 *   - net_recv()     received data stays in the module's memory and is
 *                    read out piece by piece as asked for (no copy buffer
 *                    on the AVR).
 *   - net_send()     returns once the MODULE has taken the data. (The
 *                    W5500 version waited for the chip's "sent" flag; here
 *                    a failed send shows up as the connection closing.)
 *   - net_udp_exchange()  one UDP socket: bind, send, wait for one reply.
 *
 * Not supported yet: dhcp=0 (fixed address) on WiFi; the module always uses
 * DHCP. The mac= setting is ignored (the module keeps its own
 * factory MAC).
 *
 * Also here (net_service()): remote access, so the serial cable can go.
 *   - Network console, TCP port 2323 (PuTTY, connection type "Telnet"): the
 *     start-up output (first BOOT_LOG_SIZE bytes since reset) and the last
 *     LOG_SIZE bytes of output first, then everything printed from then
 *     on, live; lines typed are handled exactly like lines typed on the
 *     serial console. With console.password set, the first line must be it.
 *   - Web page, port 80: http://<address>/ shows the same output (refreshed
 *     every 5 s) and has a command box and a reboot button; commands need
 *     console.password if one is set. /log is the output as plain text.
 * Output is kept by log_tap(), which uart.c calls for every byte it prints.
 */

#include <avr/io.h>
#include <avr/pgmspace.h>
#include <util/delay.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "net.h"
#include "settings.h"
#include "version.h"
#include "uart.h"
#include "webpage_gz.h"           /* generated from webpage.h by the Makefile */
#include "winc.h"

/* Optional: time since start-up, from the program (uptime.c). */
uint32_t platform_seconds(void) __attribute__((weak));
uint32_t platform_millis(void) __attribute__((weak));

/* WiFi group (m2m_types.h). */
#define WIFI_REQ_CURRENT_RSSI    3
#define WIFI_RESP_CURRENT_RSSI   4
#define WIFI_REQ_SET_DEVICE_NAME 7
#define WIFI_REQ_START_PROVISION 8   /* sent with the data-packet flag, as m2m_wifi.c does */
#define WIFI_RESP_PROVISION_INFO 9
#define WIFI_REQ_CONNECT        40
#define WIFI_REQ_DISCONNECT     43
#define WIFI_RESP_CON_STATE     44
#define WIFI_REQ_DHCP_CONF      50
#define WIFI_RESP_IP_CONFLICT   52

/* IP group (m2m_socket_host_if.h). */
#define SOCKET_CMD_BIND      0x41
#define SOCKET_CMD_LISTEN    0x42
#define SOCKET_CMD_ACCEPT    0x43
#define SOCKET_CMD_CONNECT   0x44
#define SOCKET_CMD_SEND      0x45
#define SOCKET_CMD_RECV      0x46
#define SOCKET_CMD_SENDTO    0x47
#define SOCKET_CMD_RECVFROM  0x48
#define SOCKET_CMD_CLOSE     0x49

#define TCP_SOCKETS        7         /* sockets 0-6 are TCP, 7-10 UDP */
#define MINING_SOCKETS     5         /* the mining connection uses 0-4 (skipping any */
                                     /* a console or web visitor has been given) */
#define CONSOLE_LISTEN     5         /* listening sockets */
#define WEB_LISTEN         6
#define CONSOLE_PORT       2323
#define WEB_PORT           80
#define UDP_SOCKET         7
#define TCP_DATA_OFFSET    80        /* TCP_TX_PACKET_OFFSET */
#define UDP_DATA_OFFSET    68        /* UDP_TX_PACKET_OFFSET */
#define SEND_MAX           1400      /* SOCKET_BUFFER_MAX_LENGTH */
#define UDP_LOCAL_PORT     40123
#define AF_INET            2

#define JOIN_WAIT_MS       20000
#define REJOIN_SECONDS     10

/* WiFi state. */
static uint8_t  started;            /* module answered */
static uint8_t  joined;             /* associated with the access point */
static uint8_t  have_address;       /* DHCP answered since joining */
static uint8_t  join_sent;          /* a join request is outstanding */
static uint8_t  join_error;         /* reason given with the last drop */
static uint32_t rejoin_at;
static uint16_t polls_waited;       /* rejoin timing when there is no clock */
static uint8_t  address[4], gateway[4], netmask[4];
static int8_t   rssi;
static uint8_t  got_rssi;
static uint8_t  rssi_known;         /* rssi holds a real reading (for /status) */
static uint8_t *prov_info;          /* WiFi setup: where the page's answer goes (else 0) */
static uint8_t  prov_got;

/* TCP state. */
enum { TCP_NONE, TCP_CONNECTING, TCP_OPEN, TCP_PEER_CLOSED };
static uint8_t  tcp_state = TCP_NONE;
static uint8_t  tcp_sock = MINING_SOCKETS - 1;
static uint16_t tcp_session;
static uint16_t next_session;
static uint8_t  recv_pending;       /* a receive request is outstanding */
static uint32_t held_addr;          /* unread received data, in the module */
static uint16_t held_left;
static int8_t   tcp_error;          /* last error the module reported */
static uint32_t tcp_retry_at;       /* after a refused connection */

/* UDP exchange state. */
static uint16_t udp_session;
static uint8_t  udp_bound, udp_done;
static uint8_t *udp_buf;
static uint16_t udp_max, udp_got;

/* Console and web (further down). */
static uint8_t side_uses(uint8_t sock);
static void side_reset(void);
static void side_report(void);
static void side_accept(const uint8_t *reply);
static void side_listen_reply(uint8_t op, const uint8_t *reply);
static uint8_t side_recv(uint8_t sock, uint16_t session, int16_t status, uint32_t data);
static void side_sent(uint8_t sock, uint16_t session, int16_t sent);

static void put_ip(const uint8_t ip[4])
{
    for (uint8_t i = 0; i < 4; i++) {
        if (i)
            uart_putc('.');
        uart_put_u32(ip[i]);
    }
}

/* Why a join failed, as text in flash (uart_puts_P). */
static const char *join_reason(uint8_t e)
{
    switch (e) {
    case 1: return PSTR("network not found");
    case 2: return PSTR("joining failed");
    case 3: return PSTR("authentication failed (wrong password?)");
    case 4: return PSTR("association failed");
    case 5: return PSTR("a join was already in progress");
    default: return PSTR("no reason given");
    }
}

static uint16_t new_session(void)
{
    if (++next_session == 0)
        next_session = 1;
    return next_session;
}

static void tcp_forget(void)
{
    if (held_left) {
        winc_release();
        held_left = 0;
    }
    recv_pending = 0;
    tcp_state = TCP_NONE;
}

/* ---- messages from the module ------------------------------------------- */

static void on_wifi(uint8_t op, uint32_t addr)
{
    uint8_t b[20];

    switch (op) {
    case WIFI_RESP_CON_STATE:
        if (!winc_receive(addr, b, 4, 0))
            return;
        join_sent = 0;
        if (b[0] == 1) {
            joined = 1;
            uart_puts_P(PSTR("wifi: joined \""));
            uart_puts(settings.ssid);
            uart_puts_P(PSTR("\"; waiting for an address\r\n"));
        } else {
            if (joined)
                uart_puts_P(PSTR("wifi: DISCONNECTED ("));
            else
                uart_puts_P(PSTR("wifi: could not join ("));
            uart_puts_P(join_reason(b[1]));
            uart_puts_P(PSTR("); trying again shortly\r\n"));
            joined = 0;
            have_address = 0;
            join_error = b[1];
            tcp_forget();               /* the module drops its sockets too */
            side_reset();
            rejoin_at = platform_seconds ? platform_seconds() + REJOIN_SECONDS : 0;
            polls_waited = 0;
        }
        break;
    case WIFI_REQ_DHCP_CONF:
        if (!winc_receive(addr, b, 20, 0))
            return;
        memcpy(address, &b[0], 4);
        memcpy(gateway, &b[4], 4);
        memcpy(netmask, &b[12], 4);
        have_address = 1;
        uart_puts_P(PSTR("wifi: address "));
        put_ip(address);
        uart_puts_P(PSTR(" mask "));
        put_ip(netmask);
        uart_puts_P(PSTR(" gw "));
        put_ip(gateway);
        uart_puts_P(PSTR(", lease "));
        uart_put_u32(winc_u32(&b[16]));
        uart_puts_P(PSTR(" s\r\n"));
        break;
    case WIFI_RESP_CURRENT_RSSI:
        if (winc_receive(addr, b, 4, 0)) {
            rssi = (int8_t)b[0];
            got_rssi = rssi_known = 1;
        }
        break;
    case WIFI_RESP_IP_CONFLICT:
        uart_puts_P(PSTR("wifi: the module reports an address conflict\r\n"));
        break;
    case WIFI_RESP_PROVISION_INFO:      /* tstrM2MProvisionInfo: ssid 33, password 65, type, status */
        if (prov_info && winc_receive(addr, prov_info, 100, 0))
            prov_got = 1;
        break;
    default:
        break;                          /* not ours to care about */
    }
}

static void on_ip(uint8_t op, uint32_t addr)
{
    uint8_t b[16];

    switch (op) {
    case SOCKET_CMD_CONNECT:            /* sock, error, data offset */
        if (!winc_receive(addr, b, 4, 0))
            return;
        if (b[0] != tcp_sock || tcp_state != TCP_CONNECTING)
            return;
        if (b[1] == 0) {
            tcp_state = TCP_OPEN;
        } else {
            tcp_error = (int8_t)b[1];
            tcp_state = TCP_NONE;       /* net_poll tries again, after a pause */
            tcp_retry_at = platform_seconds ? platform_seconds() + 2 : 0;
        }
        break;

    case SOCKET_CMD_SEND:               /* sock, -, sent (s16), session */
        if (!winc_receive(addr, b, 8, 0))
            return;
        side_sent(b[0], winc_u16(&b[4]), (int16_t)winc_u16(&b[2]));
        if (b[0] == tcp_sock && winc_u16(&b[4]) == tcp_session && (int16_t)winc_u16(&b[2]) < 0
            && tcp_state == TCP_OPEN) {
            tcp_error = (int8_t)winc_u16(&b[2]);
            tcp_state = TCP_PEER_CLOSED;
        }
        break;

    case SOCKET_CMD_RECV: {             /* remote addr (8), status, data offset, sock, -, session */
        if (!winc_receive(addr, b, 16, 0))
            return;
        int16_t status = (int16_t)winc_u16(&b[8]);
        if (side_recv(b[12], winc_u16(&b[14]), status, addr + winc_u16(&b[10])))
            return;                     /* console or web visitor: dealt with */
        if (b[12] != tcp_sock || winc_u16(&b[14]) != tcp_session)
            return;                     /* left over from an earlier connection */
        recv_pending = 0;
        if (status > 0 && tcp_state != TCP_OPEN) {
            /* Data for a connection we have already given up on: never
             * hold it (a held message nobody reads would block the module
             * for good); it is released when this handler returns. */
        } else if (status > 0) {
            held_addr = addr + winc_u16(&b[10]);
            held_left = (uint16_t)status;
            winc_hold();                /* net_recv reads it out */
        } else if (tcp_state == TCP_OPEN) {
            tcp_error = (int8_t)status; /* 0 = the server closed; < 0 = an error */
            tcp_state = TCP_PEER_CLOSED;
        }
        break;
    }

    case SOCKET_CMD_BIND:               /* sock, status, session */
        if (!winc_receive(addr, b, 4, 0))
            return;
        if (b[0] == UDP_SOCKET && winc_u16(&b[2]) == udp_session)
            udp_bound = (b[1] == 0) ? 1 : 2;
        else
            side_listen_reply(op, b);
        break;

    case SOCKET_CMD_LISTEN:             /* sock, status, session */
        if (winc_receive(addr, b, 4, 0))
            side_listen_reply(op, b);
        break;

    case SOCKET_CMD_ACCEPT:             /* remote addr (8), listening sock, new sock, offset */
        if (winc_receive(addr, b, 12, 0))
            side_accept(b);
        break;

    case SOCKET_CMD_RECVFROM: {
        if (!winc_receive(addr, b, 16, 0))
            return;
        int16_t status = (int16_t)winc_u16(&b[8]);
        if (b[12] != UDP_SOCKET || winc_u16(&b[14]) != udp_session)
            return;
        if (status > 0 && udp_buf) {
            uint16_t n = (uint16_t)status < udp_max ? (uint16_t)status : udp_max;
            if (winc_receive(addr + winc_u16(&b[10]), udp_buf, n, 0))
                udp_got = n;
        }
        udp_done = 1;
        break;
    }

    default:
        break;                          /* SENDTO replies etc.: nothing to do */
    }
}

static void on_message(uint8_t group, uint8_t op, uint16_t len, uint32_t addr)
{
    (void)len;
    if (group == WINC_GROUP_WIFI)
        on_wifi(op, addr);
    else if (group == WINC_GROUP_IP)
        on_ip(op, addr);
}

/* Handle whatever the module has queued (a few messages at most per call).
 * Every waiting loop in this file comes through here, so it also keeps the
 * heartbeat LED going (board.h) while the program waits on the module. */
static void service(void)
{
    board_beat();
    for (uint8_t i = 0; i < 8 && winc_poll(on_message); i++)
        ;
}

/* ---- requests ------------------------------------------------------------ */

static uint8_t send_join(void)
{
    static uint8_t req[108];            /* tstrM2mWifiConnect */

    if (!settings.ssid[0])
        return 0;
    memset(req, 0, sizeof req);
    strcpy((char *)&req[0], settings.pass);
    req[65] = settings.pass[0] ? 2 : 1; /* WPA2 or open */
    req[68] = 255;                      /* any channel */
    strcpy((char *)&req[70], settings.ssid);
    req[103] = 1;                       /* don't store the password in the module */
    join_sent = winc_send(WINC_GROUP_WIFI, WIFI_REQ_CONNECT, req, sizeof req, 0, 0, 0);
    return join_sent;
}

static void send_device_name(void)
{
    static uint8_t req[48];             /* shown as the host name in the router */

    memset(req, 0, sizeof req);
    strncpy((char *)req, settings.worker, sizeof req - 1);
    winc_send(WINC_GROUP_WIFI, WIFI_REQ_SET_DEVICE_NAME, req, sizeof req, 0, 0, 0);
}

/* tstrSockAddr: family (u16 LE), port (big-endian), address (as bytes). */
static void put_sockaddr(uint8_t *p, const uint8_t ip[4], uint16_t port)
{
    p[0] = AF_INET;
    p[1] = 0;
    p[2] = (uint8_t)(port >> 8);
    p[3] = (uint8_t)port;
    memcpy(&p[4], ip, 4);
}

static void tcp_open(void)
{
    uint8_t cmd[12];                    /* tstrConnectCmd */

    /* A fresh socket number each time, avoiding any a visitor has. */
    do {
        tcp_sock = (uint8_t)((tcp_sock + 1) % MINING_SOCKETS);
    } while (side_uses(tcp_sock));
    tcp_session = new_session();
    put_sockaddr(cmd, settings.server_ip, settings.server_port);
    cmd[8] = tcp_sock;
    cmd[9] = 0;                         /* no TLS */
    cmd[10] = (uint8_t)tcp_session;
    cmd[11] = (uint8_t)(tcp_session >> 8);
    tcp_forget();                       /* nothing from an older connection may stay held */
    tcp_error = 0;
    if (winc_send(WINC_GROUP_IP, SOCKET_CMD_CONNECT, cmd, sizeof cmd, 0, 0, 0))
        tcp_state = TCP_CONNECTING;
}

static void socket_close(uint8_t sock, uint16_t session)
{
    uint8_t cmd[4] = { sock, 0, (uint8_t)session, (uint8_t)(session >> 8) };
    winc_send(WINC_GROUP_IP, SOCKET_CMD_CLOSE, cmd, sizeof cmd, 0, 0, 0);
}

static void request_recv(uint8_t sock, uint16_t session, uint8_t op, uint32_t timeout_ms)
{
    uint8_t cmd[8];                     /* tstrRecvCmd */

    cmd[0] = (uint8_t)timeout_ms;
    cmd[1] = (uint8_t)(timeout_ms >> 8);
    cmd[2] = (uint8_t)(timeout_ms >> 16);
    cmd[3] = (uint8_t)(timeout_ms >> 24);
    cmd[4] = sock;
    cmd[5] = 0;
    cmd[6] = (uint8_t)session;
    cmd[7] = (uint8_t)(session >> 8);
    winc_send(WINC_GROUP_IP, op, cmd, sizeof cmd, 0, 0, 0);
}

/* ---- net.h ---------------------------------------------------------------- */

uint8_t net_init(void)
{
    joined = have_address = join_sent = 0;
    tcp_state = TCP_NONE;
    recv_pending = 0;
    held_left = 0;
    side_reset();                       /* the module is about to restart */

    started = winc_start();
    if (!started) {
        uart_puts_P(PSTR("wifi: module did not start: "));
        uart_puts_P(winc_error);
        uart_puts_P(PSTR("\r\n"));
        return 0;
    }
    if (!settings.ssid[0]) {
        uart_puts_P(PSTR("wifi: no network set. Type wifi.ssid=NAME, wifi.password=PASSWORD, then save\r\n"));
        return 1;
    }
    if (!settings.dhcp)
        uart_puts_P(PSTR("wifi: note: dhcp=0 is not supported on WiFi; using DHCP\r\n"));

    send_device_name();
    uart_puts_P(PSTR("wifi: joining \""));
    uart_puts(settings.ssid);
    uart_puts_P(PSTR("\"\r\n"));
    if (!send_join()) {
        uart_puts_P(PSTR("wifi: the module did not take the join request\r\n"));
        return 1;                       /* net_poll retries */
    }
    /* Wait for the join and the address, like the W5500 version waits for
     * its cable link and DHCP; later drops are handled by net_poll. */
    for (uint16_t ms = 0; ms < JOIN_WAIT_MS && !(joined && have_address) && (join_sent || joined); ms++) {
        service();
        _delay_ms(1);
    }
    if (!(joined && have_address))
        uart_puts_P(PSTR("wifi: not connected yet; will keep trying\r\n"));
    return 1;
}

/* ---- WiFi setup mode (net.h; button menu choice 2) -------------------------
 *
 * The module's own firmware does the hard part (m2m_wifi_start_provision_mode
 * in Microchip's driver): it becomes an open access point, hands out
 * addresses, scans for networks and serves a setup page; when someone
 * presses Connect there it sends us the chosen network and password. Layouts
 * from Microchip's m2m_types.h 19.5.2 (the module's firmware version),
 * offsets checked by compiling it on the PC. Following the Arduino WiFi101
 * library: open, visible, channel 1, the module at 192.168.1.1, every web
 * address redirected to the page, and on success a join with the new details
 * straight away (here: the TEST, before anything is saved). */
#define SETUP_AP_NAME   "megaminer-setup"
#define SETUP_PAGE_NAME "megaminer.setup"
#define SETUP_SHOW_FAIL_MS 3000

static uint8_t setup_start_page(void)
{
    uint8_t req[204];                   /* tstrM2MProvisionModeConfig */

    memset(req, 0, sizeof req);
    strcpy_P((char *)&req[0], PSTR(SETUP_AP_NAME));     /* access point name */
    req[33] = 1;                        /* channel 1 (counted from 1 since firmware 19.5) */
    req[63] = 1;                        /* open (only while this mode runs: you are at the board) */
    req[64] = 0;                        /* name visible */
    req[65] = 192;                      /* the module's own address on that network */
    req[66] = 168;
    req[67] = 1;
    req[68] = 1;
    strcpy_P((char *)&req[136], PSTR(SETUP_PAGE_NAME)); /* the page's web name */
    req[200] = 1;                       /* any web address leads to the page */
    prov_got = 0;
    return winc_send(WINC_GROUP_WIFI, WIFI_REQ_START_PROVISION | WINC_DATA_PACKET, req, sizeof req, 0, 0, 0);
}

uint8_t net_wifi_setup(void)
{
    uint8_t info[100];                  /* tstrM2MProvisionInfo */
    char old_ssid[SSID_MAX + 1], old_pass[PASS_MAX + 1];

    prov_info = info;
    for (;;) {
        uint16_t ms = 0;

        /* A fresh module each round: also ends any earlier access point. */
        joined = have_address = join_sent = 0;
        side_reset();
        started = winc_start();
        if (!started) {
            uart_puts_P(PSTR("wifi setup: the module did not start: "));
            uart_puts_P(winc_error);
            uart_puts_P(PSTR("\r\n"));
            prov_info = 0;
            return 0;
        }
        if (!setup_start_page()) {
            uart_puts_P(PSTR("wifi setup: the module did not take the request; trying again\r\n"));
            _delay_ms(1000);
            continue;
        }
        uart_puts_P(PSTR("wifi setup: on a phone, join the open WiFi network \"" SETUP_AP_NAME "\" and open\r\n"
                         "  http://" SETUP_PAGE_NAME "/ (or http://192.168.1.1/; it may open by itself).\r\n"
                         "  Pick the network, type its password, Connect. The details are tried\r\n"
                         "  before they are kept. A reset leaves without changing anything.\r\n"));

        while (!prov_got) {             /* as long as it takes */
            service();                  /* (and the heartbeat) */
            board_setup_light(BOARD_SETUP_WAITING, (ms / 500) & 1);
            _delay_ms(1);
            if (++ms == 1000)
                ms = 0;
        }

        info[32] = '\0';                /* never trust a string to be ended */
        info[97] = '\0';
        if (info[99] != 0) {            /* status: 0 = M2M_SUCCESS */
            uart_puts_P(PSTR("wifi setup: the page reported a failure; offering it again\r\n"));
            continue;
        }
        if (!settings_wifi_ok((const char *)&info[0], (const char *)&info[33])) {
            /* Anyone near the board can use the open setup network, so its
             * answer is checked like a typed wifi.ssid= and wifi.password=. */
            uart_puts_P(PSTR("wifi setup: the page sent a network name or password that wifi.ssid= / wifi.password= "
                             "would refuse; offering it again\r\n"));
            continue;
        }

        /* The test: join with the new details, keeping the old ones aside. */
        strcpy(old_ssid, settings.ssid);
        strcpy(old_pass, settings.pass);
        strcpy(settings.ssid, (const char *)&info[0]);
        strcpy(settings.pass, (const char *)&info[33]);
        uart_puts_P(PSTR("wifi setup: trying \""));
        uart_puts(settings.ssid);
        uart_puts_P(PSTR("\"\r\n"));
        board_setup_light(BOARD_SETUP_TRYING, 1);
        if (info[98] == 3) {            /* WEP: never supported here (send_join speaks WPA2 or open) */
            uart_puts_P(PSTR("wifi setup: that network uses WEP, which this miner cannot join\r\n"));
        } else {
            send_device_name();
            if (send_join())
                for (uint16_t t = 0; t < JOIN_WAIT_MS && !(joined && have_address) && (join_sent || joined); t++) {
                    service();
                    _delay_ms(1);
                }
        }
        if (joined && have_address) {
            settings_command("save");   /* also the bootloader's copy (settings.c) */
            prov_info = 0;
            uart_puts_P(PSTR("wifi setup: joined with the new details and SAVED; starting normally\r\n"));
            board_setup_light(BOARD_SETUP_DONE, 0);
            return 1;
        }
        strcpy(settings.ssid, old_ssid);    /* not kept: back to what was there */
        strcpy(settings.pass, old_pass);
        uart_puts_P(PSTR("wifi setup: could not join with those details; NOTHING SAVED. "
                         "The setup network comes back in a moment\r\n"));
        board_setup_light(BOARD_SETUP_FAILED, 1);
        for (uint16_t t = 0; t < SETUP_SHOW_FAIL_MS; t++) {
            service();
            _delay_ms(1);
        }
    }
}

/* WiFi dropped or never came up: ask again every REJOIN_SECONDS. Returns 1
 * while not joined. */
static uint8_t rejoin_if_due(void)
{
    if (joined)
        return 0;
    if (!join_sent && settings.ssid[0]) {
        uint8_t due = platform_seconds ? (int32_t)(platform_seconds() - rejoin_at) >= 0
                                       : ++polls_waited >= 150;   /* ~10 s of polls */
        if (due) {
            polls_waited = 0;
            send_join();
        }
    }
    return 1;
}

uint8_t net_link_up(void)
{
    if (started) {
        service();                      /* the miner may not have polled for a while */
        /* Rejoining here too: miner_sv2 waits for the link without calling
         * net_poll (which would also open the pool connection too early),
         * and would otherwise wait forever after a drop at that moment. */
        rejoin_if_due();
    }
    return joined && have_address;
}

void net_poll(void)
{
    if (!started)
        return;
    service();

    if (rejoin_if_due())
        return;
    if (!(joined && have_address))
        return;

    switch (tcp_state) {
    case TCP_NONE:
        if (!platform_seconds || (int32_t)(platform_seconds() - tcp_retry_at) >= 0)
            tcp_open();
        break;
    case TCP_OPEN:
        /* Keep one receive request outstanding; the module answers it when
         * data arrives (or the connection ends). */
        if (!recv_pending && held_left == 0) {
            request_recv(tcp_sock, tcp_session, SOCKET_CMD_RECV, 0xffffffffUL);
            recv_pending = 1;
        }
        break;
    case TCP_PEER_CLOSED:
        /* Keep reporting "connected" until what the server sent before
         * closing has been read (its last words), then close our side. */
        if (held_left == 0) {
            socket_close(tcp_sock, tcp_session);
            tcp_state = TCP_NONE;       /* the next net_poll opens a new one */
            recv_pending = 0;
        }
        break;
    default:
        break;                          /* connecting: the module answers */
    }
}

uint8_t net_connected(void)
{
    return tcp_state == TCP_OPEN || (tcp_state == TCP_PEER_CLOSED && held_left);
}

uint16_t net_recv(uint8_t *buf, uint16_t max)
{
    uint16_t n;

    if (held_left == 0 || max == 0)
        return 0;
    n = held_left < max ? held_left : max;
    if (!winc_receive(held_addr, buf, n, n == held_left)) {
        winc_release();
        held_left = 0;
        return 0;
    }
    held_addr += n;
    held_left -= n;
    return n;
}

uint8_t net_send(const uint8_t *buf, uint16_t len)
{
    while (len) {
        uint16_t n = len > SEND_MAX ? SEND_MAX : len;
        uint8_t cmd[16];                /* tstrSendCmd */
        uint8_t ok = 0;

        if (tcp_state != TCP_OPEN)
            return 0;
        memset(cmd, 0, sizeof cmd);
        cmd[0] = tcp_sock;
        cmd[2] = (uint8_t)n;
        cmd[3] = (uint8_t)(n >> 8);
        cmd[12] = (uint8_t)tcp_session;
        cmd[13] = (uint8_t)(tcp_session >> 8);
        /* The module may be out of buffer space for a moment: keep handling
         * its messages and try again, for up to about two seconds. */
        for (uint16_t tries = 0; tries < 2000 && !ok; tries++) {
            ok = winc_send(WINC_GROUP_IP, SOCKET_CMD_SEND | WINC_DATA_PACKET, cmd, sizeof cmd,
                           buf, n, TCP_DATA_OFFSET);
            if (!ok) {
                service();
                _delay_ms(1);
            }
        }
        if (!ok)
            return 0;
        buf += n;
        len -= n;
    }
    return 1;
}

void net_close(void)
{
    if (tcp_state != TCP_NONE)
        socket_close(tcp_sock, tcp_session);
    tcp_forget();
}

void net_report(void)
{
    uart_puts_P(joined ? PSTR("net: wifi joined") : PSTR("net: wifi NOT joined"));
    if (!joined && join_error) {
        uart_puts_P(PSTR(" (last reason: "));
        uart_puts_P(join_reason(join_error));
        uart_putc(')');
    }
    uart_puts_P(have_address ? PSTR(", address ") : PSTR(", no address"));
    if (have_address)
        put_ip(address);
    if (joined) {
        got_rssi = 0;
        if (winc_send(WINC_GROUP_WIFI, WIFI_REQ_CURRENT_RSSI, 0, 0, 0, 0, 0)) {
            for (uint16_t ms = 0; ms < 500 && !got_rssi && !winc_holding(); ms++) {
                service();
                _delay_ms(1);
            }
        }
        if (got_rssi) {
            uart_puts_P(PSTR(", signal "));
            if (rssi < 0) {
                uart_putc('-');
                uart_put_u32((uint8_t)-rssi);
            } else {
                uart_put_u32((uint8_t)rssi);
            }
            uart_puts_P(PSTR(" dBm"));
        }
    }
    uart_puts_P(PSTR(", tcp "));
    uart_puts_P(tcp_state == 0 ? PSTR("none") : tcp_state == 1 ? PSTR("connecting")
                : tcp_state == 2 ? PSTR("open") : PSTR("closed by server"));
    if (tcp_error) {
        uart_puts_P(PSTR(" (module error "));
        if (tcp_error < 0) {
            uart_putc('-');
            uart_put_u32((uint8_t)-tcp_error);
        } else {
            uart_put_u32((uint8_t)tcp_error);
        }
        uart_putc(')');
    }
    uart_puts_P(PSTR(", SPI retries "));
    uart_put_u32(winc_bus_errors);
    uart_puts_P(PSTR(", no-room refusals "));
    uart_put_u32(winc_send_no_room);
    uart_puts_P(PSTR(", mining socket "));
    uart_put_u32(tcp_sock);
    uart_puts_P(recv_pending ? PSTR(", receive asked") : PSTR(", no receive asked"));
    uart_puts_P(PSTR(", unread "));
    uart_put_u32(held_left);
    if (winc_holding())
        uart_puts_P(PSTR(" (message held)"));
    uart_puts_P(PSTR("\r\n"));
    side_report();
}

uint16_t net_udp_exchange(const uint8_t ip[4], uint16_t port,
                          const uint8_t *req, uint16_t req_len,
                          uint8_t *resp, uint16_t resp_max)
{
    static const uint8_t ANY[4] = { 0, 0, 0, 0 };
    uint8_t cmd[16];

    /* A held TCP message blocks every other reply from the module; the
     * caller (time fetch) simply tries again later. */
    if (!net_link_up() || winc_holding())
        return 0;

    udp_session = new_session();
    udp_bound = udp_done = 0;
    udp_buf = resp;
    udp_max = resp_max;
    udp_got = 0;

    /* bind (tstrBindCmd): our port, so the reply has somewhere to go */
    put_sockaddr(cmd, ANY, UDP_LOCAL_PORT);
    cmd[8] = UDP_SOCKET;
    cmd[9] = 0;
    cmd[10] = (uint8_t)udp_session;
    cmd[11] = (uint8_t)(udp_session >> 8);
    if (!winc_send(WINC_GROUP_IP, SOCKET_CMD_BIND, cmd, 12, 0, 0, 0))
        goto done;
    for (uint16_t ms = 0; ms < 1000 && !udp_bound && !winc_holding(); ms++) {
        service();
        _delay_ms(1);
    }
    if (udp_bound != 1)
        goto done;

    /* sendto (tstrSendCmd with the destination) */
    memset(cmd, 0, sizeof cmd);
    cmd[0] = UDP_SOCKET;
    cmd[2] = (uint8_t)req_len;
    cmd[3] = (uint8_t)(req_len >> 8);
    put_sockaddr(&cmd[4], ip, port);
    cmd[12] = (uint8_t)udp_session;
    cmd[13] = (uint8_t)(udp_session >> 8);
    if (!winc_send(WINC_GROUP_IP, SOCKET_CMD_SENDTO | WINC_DATA_PACKET, cmd, 16,
                   req, req_len, UDP_DATA_OFFSET))
        goto done;

    /* recvfrom with a 2 s timeout; the module answers either way */
    request_recv(UDP_SOCKET, udp_session, SOCKET_CMD_RECVFROM, 2000);
    for (uint16_t ms = 0; ms < 2500 && !udp_done && !winc_holding(); ms++) {
        service();
        _delay_ms(1);
    }

done:
    socket_close(UDP_SOCKET, udp_session);
    udp_session = 0;                    /* late replies are ignored */
    udp_buf = 0;
    return udp_got;
}

/* ==========================================================================
 * Remote access: network console (port 2323) and web page (port 80).
 * ========================================================================== */

static uint32_t now_ms(void)
{
    static uint32_t calls;              /* rough stand-in when there is no clock */

    if (platform_millis)
        return platform_millis();
    return (calls += 10);
}

/* ---- output: the start-up part and the recent part ------------------------
 *
 * Two buffers, both fed every byte printed (log_tap, called by uart.c):
 *   - boot_log: the FIRST BOOT_LOG_SIZE bytes since start-up, then never
 *     touched again. Start-up is where problems show (settings, WiFi, time,
 *     handshake), and with no serial cable there is no other way to see it:
 *     the network only comes up partway through it.
 *   - log_ring: the most recent LOG_SIZE bytes, overwritten as it goes.
 * Every byte has a position, counted from start-up. Positions below
 * boot_len are in boot_log; positions from log_oldest() on are in the ring.
 * Anything between the two (when more than both have been printed) is gone;
 * the console and the web page show a marker line there (log_piece). */

#define BOOT_LOG_SIZE 1536
#define LOG_SIZE      4096

static char     boot_log[BOOT_LOG_SIZE];
static uint16_t boot_len;
static char     log_ring[LOG_SIZE];
static uint16_t log_head;               /* where the next byte goes */
static uint32_t log_total;              /* bytes printed since start-up */

void log_tap(char c)
{
    if (boot_len < BOOT_LOG_SIZE)
        boot_log[boot_len++] = c;
    log_ring[log_head] = c;
    if (++log_head == LOG_SIZE)
        log_head = 0;
    log_total++;
}

static uint32_t log_oldest(void)
{
    return log_total > LOG_SIZE ? log_total - LOG_SIZE : 0;
}

/* Output position pos (counted since start-up, not older than log_oldest())
 * -> the bytes from there that lie in one piece in the ring. */
static uint16_t log_span(uint32_t pos, const char **p)
{
    uint16_t back = (uint16_t)(log_total - pos);
    uint16_t i = log_head >= back ? log_head - back : (uint16_t)(log_head + LOG_SIZE - back);
    uint16_t n = (uint16_t)(LOG_SIZE - i);

    if (back < n)
        n = back;
    *p = &log_ring[i];
    return n;
}

static const char LOG_GAP[] PROGMEM =
    "\r\n----- (start-up output above; later output not kept until here) -----\r\n";

/* The output at position *pos, as one piece: from the start-up buffer, or
 * from the ring, or (in the gap between them) the marker line, after which
 * *pos jumps to the ring. *flash says the piece is in flash (the marker).
 * Returns 0 when there is nothing at or after *pos yet. */
static uint16_t log_piece(uint32_t *pos, const char **p, uint8_t *flash)
{
    *flash = 0;
    if (*pos < boot_len) {
        *p = &boot_log[*pos];
        return (uint16_t)(boot_len - *pos);
    }
    if (*pos < log_oldest()) {
        *pos = log_oldest();
        *p = LOG_GAP;
        *flash = 1;
        return sizeof LOG_GAP - 1;
    }
    if (*pos >= log_total)
        return 0;
    return log_span(*pos, p);
}

/* ---- listening ------------------------------------------------------------ */

enum { L_IDLE, L_BINDING, L_LISTENING, L_READY };

static struct listener {
    uint8_t  sock;
    uint16_t port;
    uint8_t  state;
    uint16_t session;
    uint32_t retry_at;                  /* ms */
} listeners[2] = {
    { CONSOLE_LISTEN, CONSOLE_PORT, L_IDLE, 0, 0 },
    { WEB_LISTEN,     WEB_PORT,     L_IDLE, 0, 0 },
};

static void listen_setup(struct listener *ls)
{
    static const uint8_t ANY[4] = { 0, 0, 0, 0 };
    uint8_t cmd[12];                    /* tstrBindCmd */

    if ((int32_t)(now_ms() - ls->retry_at) < 0)
        return;
    ls->retry_at = now_ms() + 5000;     /* if this goes nowhere, again in 5 s */
    if (ls->session)
        socket_close(ls->sock, ls->session);   /* in case the module still has it */
    ls->session = new_session();
    put_sockaddr(cmd, ANY, ls->port);
    cmd[8] = ls->sock;
    cmd[9] = 0;
    cmd[10] = (uint8_t)ls->session;
    cmd[11] = (uint8_t)(ls->session >> 8);
    if (winc_send(WINC_GROUP_IP, SOCKET_CMD_BIND, cmd, sizeof cmd, 0, 0, 0))
        ls->state = L_BINDING;
}

static void side_listen_reply(uint8_t op, const uint8_t *reply)
{
    for (uint8_t i = 0; i < 2; i++) {
        struct listener *ls = &listeners[i];
        if (reply[0] != ls->sock || winc_u16(&reply[2]) != ls->session)
            continue;
        if (reply[1] != 0) {            /* refused: start over later */
            ls->state = L_IDLE;
            return;
        }
        if (op == SOCKET_CMD_BIND) {
            uint8_t cmd[4] = { ls->sock, 2, (uint8_t)ls->session, (uint8_t)(ls->session >> 8) };
            if (winc_send(WINC_GROUP_IP, SOCKET_CMD_LISTEN, cmd, sizeof cmd, 0, 0, 0))
                ls->state = L_LISTENING;
            else
                ls->state = L_IDLE;
        } else {
            ls->state = L_READY;
        }
        return;
    }
}

/* ---- sending to a visitor ---------------------------------------------- */

static uint8_t client_send_once(uint8_t sock, uint16_t session, const void *buf, uint16_t n);

/* Counters for the "net" command (side_report). */
static uint16_t vis_accepts, vis_turned_away, vis_sends_ok, vis_sends_failed;
static uint16_t vis_send_errors, vis_recv_replies;
static int16_t  vis_last_send_error;

static uint8_t client_send(uint8_t sock, uint16_t session, const void *buf, uint16_t n)
{
    uint8_t ok = client_send_once(sock, session, buf, n);

    if (ok) {
        if (vis_sends_ok < 0xffff)
            vis_sends_ok++;
    } else if (vis_sends_failed < 0xffff) {
        vis_sends_failed++;
    }
    return ok;
}

static uint8_t client_send_once(uint8_t sock, uint16_t session, const void *buf, uint16_t n)
{
    uint8_t cmd[16];                    /* tstrSendCmd */

    memset(cmd, 0, sizeof cmd);
    cmd[0] = sock;
    cmd[2] = (uint8_t)n;
    cmd[3] = (uint8_t)(n >> 8);
    cmd[12] = (uint8_t)session;
    cmd[13] = (uint8_t)(session >> 8);
    return winc_send(WINC_GROUP_IP, SOCKET_CMD_SEND | WINC_DATA_PACKET, cmd, sizeof cmd,
                     buf, n, TCP_DATA_OFFSET);
}

/* Commands typed remotely follow the serial console's rule: spaces are
 * dropped, except in the two "wifi." lines (wifi.ssid=, wifi.password=). */
static void drop_spaces(char *s)
{
    char *out = s;

    if (strncmp_P(s, PSTR("wifi."), 5) == 0)
        return;
    for (; *s; s++)
        if (*s != ' ' && *s != '\t')
            *out++ = *s;
    *out = '\0';
}

/* ---- the network console (telnet) -----------------------------------------
 *
 * Telnet rather than raw TCP so that the CHIP does the echoing, as the serial
 * console does: on connecting we say "I will echo" (T_IAC T_WILL ECHO) and "no
 * go-aheads" (T_IAC T_WILL SUPPRESS-GO-AHEAD); PuTTY in Telnet mode then stops
 * echoing locally and sends each key as it is pressed, and passwords can be
 * shown as stars. Other options the client offers or asks for are refused. */

#define T_IAC   255
#define T_DONT  254
#define T_DO    253
#define T_WONT  252
#define T_WILL  251
#define T_SB    250
#define T_SE    240
#define OPT_ECHO 1
#define OPT_SGA  3

static int8_t   con_sock = -1;
static uint16_t con_session;
static uint8_t  con_recv_pending, con_greet, con_authed, con_sends;
static uint32_t con_pos;                /* next output byte to send */
static uint32_t con_last_send;          /* ms */
static char     con_line[100];          /* room for sv2.authority_key=<64 hex digits> */
static uint8_t  con_len, con_last_cr, con_too_long;
static uint8_t  con_iac, con_iac_cmd;   /* telnet command being read */
static uint8_t  con_out[32];            /* echo and telnet answers, sent promptly */
static uint8_t  con_out_len;

static const char CON_BANNER[] PROGMEM =
    "megaminer console (connect with PuTTY, connection type Telnet).\r\n"
    "This start-up's first output follows, then recent and live output.\r\nCommands as on the serial\r\n"
    "console (help); quit to leave.\r\n";
static const char CON_LOCKED[] PROGMEM =
    "Commands need the console password: type it on a line of its own.\r\n";

static void con_out_put(uint8_t c)
{
    if (con_out_len < sizeof con_out)
        con_out[con_out_len++] = c;
}

static void con_drop(uint8_t say_so)
{
    if (con_sock < 0)
        return;
    socket_close((uint8_t)con_sock, con_session);
    con_sock = -1;
    if (say_so)
        uart_puts_P(PSTR("net: console visitor left\r\n"));
}

/* Password guessing: after a wrong console password, every password check
 * (telnet and the web page) is refused unchecked for PASS_LOCKOUT_MS. That
 * limits guessing to one try every two seconds, without ever stopping the
 * hashing to wait. */
#define PASS_LOCKOUT_MS 2000
#define PASS_OK     1
#define PASS_WRONG  0
#define PASS_LOCKED 2
static uint32_t pass_wrong_at;
static uint8_t  pass_was_wrong;

static uint8_t pass_check(const char *given)
{
    if (pass_was_wrong && now_ms() - pass_wrong_at < PASS_LOCKOUT_MS)
        return PASS_LOCKED;
    pass_was_wrong = 0;
    if (strcmp(given, settings.console_pass) == 0)
        return PASS_OK;
    pass_was_wrong = 1;
    pass_wrong_at = now_ms();
    return PASS_WRONG;
}

static void con_handle_line(void)
{
    if (strcmp_P(con_line, PSTR("quit")) == 0 || strcmp_P(con_line, PSTR("exit")) == 0) {
        con_drop(1);                    /* allowed before the password too */
        return;
    }
    if (!con_authed) {
        uint8_t r = pass_check(con_line);
        if (r == PASS_OK) {
            con_authed = 1;
            uart_puts_P(PSTR("net: console unlocked\r\n"));
            return;
        }
        /* The visitor stays connected and simply tries again; the log
         * (which the visitor also sees) says why it did not work. */
        uart_puts_P(r == PASS_LOCKED
                    ? PSTR("net: console password not checked: too soon after a wrong one; wait 2 s\r\n")
                    : PSTR("net: wrong console password\r\n"));
        return;
    }
    drop_spaces(con_line);
    if (con_line[0] && !uart_inject_line(con_line))
        uart_puts_P(PSTR("net: busy; type it again\r\n"));
}

/* Should the character just added at con_line[con_len - 1] show as a star? */
static uint8_t con_secret(void)
{
    if (!con_authed)
        return 1;                       /* this line is the console password */
    if (con_len > 14 && strncmp_P(con_line, PSTR("wifi.password="), 14) == 0)
        return 1;
    if (con_len > 17 && strncmp_P(con_line, PSTR("console.password="), 17) == 0)
        return 1;
    return 0;
}

static void con_char(uint8_t c)
{
    if (c == '\r' || (c == '\n' && !con_last_cr)) {
        con_out_put('\r');
        con_out_put('\n');
        if (con_too_long) {
            /* Cut short, a command could do something else than meant
             * (a shorter password saved, say), so it is not used at all. */
            uart_puts_P(PSTR("net: console line too long (over 99 characters); ignored\r\n"));
            con_too_long = 0;
            con_len = 0;
        } else if (con_len) {
            con_line[con_len] = '\0';
            con_len = 0;
            con_handle_line();
        }
    } else if (c == '\b' || c == 0x7f) {
        if (con_len) {
            con_len--;
            con_out_put('\b');
            con_out_put(' ');
            con_out_put('\b');
        }
    } else if (c >= ' ' && c <= '~') {
        if (con_len < sizeof con_line - 1) {
            con_line[con_len++] = (char)c;
            con_out_put(con_secret() ? '*' : c);
        } else {
            con_too_long = 1;
        }
    }
    con_last_cr = (c == '\r');
}

/* Every byte from the visitor: telnet commands are taken out here. */
static void con_byte(uint8_t c)
{
    switch (con_iac) {
    case 1:                             /* after T_IAC */
        if (c >= T_WILL && c <= T_DONT) {
            con_iac_cmd = c;
            con_iac = 2;
        } else {
            con_iac = (c == T_SB) ? 3 : 0;
        }
        return;
    case 2:                             /* after T_IAC T_WILL/T_WONT/T_DO/T_DONT: the option */
        if (con_iac_cmd == T_WILL) {      /* "I will ...": no thanks */
            con_out_put(T_IAC); con_out_put(T_DONT); con_out_put(c);
        } else if (con_iac_cmd == T_DO && c != OPT_ECHO && c != OPT_SGA) {
            con_out_put(T_IAC); con_out_put(T_WONT); con_out_put(c);
        }                               /* T_DO ECHO / T_DO SGA: agreeing with us */
        con_iac = 0;
        return;
    case 3:                             /* inside T_IAC T_SB ... T_IAC T_SE: skipped */
        if (c == T_IAC)
            con_iac = 4;
        return;
    case 4:
        con_iac = (c == T_SE) ? 0 : 3;
        return;
    default:
        break;
    }
    if (c == T_IAC)
        con_iac = 1;
    else
        con_char(c);
}

static void con_send_P(const char *p)
{
    char buf[80];
    uint8_t n = 0;

    for (;;) {
        char c = (char)pgm_read_byte(p++);
        if (c)
            buf[n++] = c;
        if ((n == sizeof buf || !c) && n) {
            if (client_send((uint8_t)con_sock, con_session, buf, n))
                con_sends++;
            n = 0;
        }
        if (!c)
            return;
    }
}

static void con_service(void)
{
    const char *p;
    uint16_t n;

    if (!con_recv_pending) {
        request_recv((uint8_t)con_sock, con_session, SOCKET_CMD_RECV, 0xffffffffUL);
        con_recv_pending = 1;
    }
    if (con_greet) {
        static const uint8_t OFFER[6] = { T_IAC, T_WILL, OPT_ECHO, T_IAC, T_WILL, OPT_SGA };
        con_greet = 0;
        if (client_send((uint8_t)con_sock, con_session, OFFER, sizeof OFFER))
            con_sends++;
        con_send_P(CON_BANNER);
        if (!con_authed)
            con_send_P(CON_LOCKED);
    }
    if (con_out_len) {                  /* echo: straight away */
        if (client_send((uint8_t)con_sock, con_session, con_out, con_out_len)) {
            con_sends++;
            con_out_len = 0;
        }
    }
    /* New output: in pieces, a few times a second at most, and no more than
     * a few pieces waiting in the module (a slow or vanished visitor must not
     * use up the module's memory). */
    if (log_total == con_pos || con_sends >= 3 || now_ms() - con_last_send < 100)
        return;
    {
        uint32_t pos = con_pos;         /* (a gap moves it to the ring) */
        uint8_t flash;
        n = log_piece(&pos, &p, &flash);
        if (n == 0)
            return;
        if (flash) {                    /* the gap marker */
            con_send_P(p);
            con_pos = pos;
            con_last_send = now_ms();
            return;
        }
        if (n > 512)
            n = 512;
        if (client_send((uint8_t)con_sock, con_session, p, n)) {
            con_pos = pos + n;
            con_sends++;
            con_last_send = now_ms();
        }
    }
}

/* ---- the web page --------------------------------------------------------- */

#define WEB_VISITORS 2                  /* browsers often fetch two things at once */

enum { W_REQUEST, W_RESPOND, W_DRAIN };

static struct web {
    int8_t   sock;
    uint16_t session;
    uint8_t  recv_pending, state, sends;
    uint32_t since;                     /* ms */
    /* the request, as much as we need of it */
    char     line[40];                  /* "GET /log HTTP/1.1" */
    char     hdr[24];                   /* current header line, lower case */
    char     body[200];                 /* "pass=...&cmd=..." (URL-encoded) */
    uint8_t  line_len, hdr_len, body_len, part, got;
    uint8_t  ours;                      /* sent our X-Megaminer header (see web_respond) */
    uint8_t  too_long;                  /* the body did not fit */
    uint16_t clen, body_seen;
} webs[WEB_VISITORS];

static uint8_t web_reboot;              /* 1 = asked for from the page, 2 = answer sent: do it */

static void http_char(struct web *w, char c)
{
    w->got = 1;
    switch (w->part) {
    case 0:                             /* request line */
        if (c == '\n')
            w->part = 1;
        else if (c != '\r' && w->line_len < sizeof w->line - 1)
            w->line[w->line_len++] = c;
        break;
    case 1:                             /* headers, until an empty line */
        if (c == '\n') {
            if (w->hdr_len == 0) {
                w->part = (w->clen && strncmp_P(w->line, PSTR("POST"), 4) == 0) ? 2 : 3;
            } else {
                w->hdr[w->hdr_len] = '\0';
                if (strncmp_P(w->hdr, PSTR("content-length:"), 15) == 0)
                    w->clen = (uint16_t)strtoul(w->hdr + 15, 0, 10);
                else if (strncmp_P(w->hdr, PSTR("x-megaminer:"), 12) == 0)
                    w->ours = 1;
                w->hdr_len = 0;
            }
        } else if (c != '\r' && w->hdr_len < sizeof w->hdr - 1) {
            w->hdr[w->hdr_len++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        break;
    case 2:                             /* body */
        if (w->body_len < sizeof w->body - 1)
            w->body[w->body_len++] = c;
        else
            w->too_long = 1;
        if (++w->body_seen >= w->clen)
            w->part = 3;
        break;
    default:
        break;                          /* anything after the request: ignored */
    }
}

static void web_drop(struct web *w)
{
    if (w->sock < 0)
        return;
    socket_close((uint8_t)w->sock, w->session);
    w->sock = -1;
}

/* Send to a web visitor, waiting (up to ~0.5 s) for room in the module. */
static uint8_t web_put(struct web *w, const void *buf, uint16_t n)
{
    for (uint16_t tries = 0; tries < 500; tries++) {
        if (w->sock < 0)
            return 0;
        if (client_send((uint8_t)w->sock, w->session, buf, n)) {
            w->sends++;
            return 1;
        }
        service();
        _delay_ms(1);
    }
    return 0;
}

/* `len` bytes from flash (any bytes, zeros included). */
static void web_put_flash(struct web *w, const char *p, uint16_t len)
{
    char buf[128];

    while (len) {
        uint8_t n = len < sizeof buf ? (uint8_t)len : (uint8_t)sizeof buf;
        memcpy_P(buf, p, n);
        if (!web_put(w, buf, n))
            return;
        p += n;
        len -= n;
    }
}

/* A text string from flash. */
static void web_put_P(struct web *w, const char *p)
{
    web_put_flash(w, p, (uint16_t)strlen_P(p));
}

static void web_head(struct web *w, const char *status_P, const char *type_P)
{
    web_put_P(w, PSTR("HTTP/1.0 "));
    web_put_P(w, status_P);
    web_put_P(w, PSTR("\r\nContent-Type: "));
    web_put_P(w, type_P);
    web_put_P(w, PSTR("\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"));
}

static int8_t hex_digit(char c)
{
    if (c >= '0' && c <= '9') return (int8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (int8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int8_t)(c - 'A' + 10);
    return -1;
}

/* name=value from a form body ("a=1&b=2"), %XX and + decoded. Returns 0
 * if the value did not fit in `out` or holds a character that is not
 * printable (a command cut short or carrying control characters is never
 * used); a missing name gives "" and 1. */
static uint8_t form_value(const char *body, const char *name, char *out, uint8_t max)
{
    uint8_t len = (uint8_t)strlen(name), n = 0;
    const char *p = body;

    out[0] = '\0';
    while (*p) {
        if (strncmp(p, name, len) == 0 && p[len] == '=') {
            for (p += len + 1; *p && *p != '&'; p++) {
                char c = *p;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%' && hex_digit(p[1]) >= 0 && hex_digit(p[2]) >= 0) {
                    c = (char)(hex_digit(p[1]) << 4 | hex_digit(p[2]));
                    p += 2;
                }
                if (n == max - 1 || c < ' ' || c > '~')
                    return 0;
                out[n++] = c;
            }
            out[n] = '\0';
            return 1;
        }
        while (*p && *p != '&')
            p++;
        if (*p)
            p++;
    }
    return 1;
}

/* Handle a command from the page. Returns the answer; *ok = 0 for a refusal. */
static const char *web_command(struct web *w, uint8_t *ok)
{
    char pass[CONSOLE_PASS_MAX + 2], cmd[100];  /* (pass: one over, so too long = wrong) */
    uint8_t pass_ok = form_value(w->body, "pass", pass, sizeof pass);

    *ok = 0;
    if (w->too_long || !form_value(w->body, "cmd", cmd, sizeof cmd))
        return PSTR("refused: command too long or not plain text");
    if (settings.console_pass[0]) {
        uint8_t r = pass_ok ? pass_check(pass) : PASS_WRONG;
        if (r == PASS_LOCKED)
            return PSTR("refused: too soon after a wrong password; wait 2 s");
        if (r != PASS_OK) {
            if (!pass_ok) {             /* (pass_check was not asked: lock out anyway) */
                pass_was_wrong = 1;
                pass_wrong_at = now_ms();
            }
            uart_puts_P(PSTR("net: web command refused (wrong console password)\r\n"));
            return PSTR("refused: wrong console password");
        }
    }
    drop_spaces(cmd);
    if (!cmd[0]) {
        /* No command: the page's "unlock" button, checking the password. */
        *ok = 1;
        return PSTR("unlocked");
    }
    if (strcmp(cmd, "update") == 0) {
        /* As reboot, but into the bootloader (bootshare.h), which then
         * answers on this same address and port for the new firmware. */
        if (!settings_request_update())
            return PSTR("refused: no working bootloader (the log says why)");
        web_reboot = 1;
        *ok = 1;
        return PSTR("restarting into the bootloader");
    }
    if (strcmp(cmd, "reboot") == 0) {
        /* Not yet: first let this answer reach the browser. */
        web_reboot = 1;
        uart_puts_P(PSTR("net: reboot requested from the web page\r\n"));
        *ok = 1;
        return PSTR("rebooting; the page comes back in about a minute");
    }
    if (!uart_inject_line(cmd))
        return PSTR("busy; try again");
    *ok = 1;
    return PSTR("sent; the answer appears in the log");
}

/* /status: the program's figures as JSON. The ordinary printing functions
 * write it (uart_redirect), gathered here and sent in pieces. While a piece
 * is being sent the redirect is off, because sending may handle module
 * messages, which may print, and that must not end up in the JSON. */
void status_json(void) __attribute__((weak));   /* the miner adds its members */

static struct web *status_to;
static char    status_buf[64];
static uint8_t status_len;

static void status_flush(void)
{
    void (*was)(char) = uart_redirect;

    uart_redirect = 0;
    if (status_len)
        web_put(status_to, status_buf, status_len);
    status_len = 0;
    uart_redirect = was;
}

static void status_char(char c)
{
    status_buf[status_len++] = c;
    if (status_len == sizeof status_buf)
        status_flush();
}

static void web_status(struct web *w)
{
    web_head(w, PSTR("200 OK"), PSTR("application/json"));
    status_to = w;
    status_len = 0;
    uart_redirect = status_char;
    uart_puts_P(PSTR("{\"rssi\":"));
    if (rssi_known) {
        if (rssi < 0)
            uart_putc('-');
        uart_put_u32((uint8_t)(rssi < 0 ? -(int16_t)rssi : rssi));
    } else {
        uart_puts_P(PSTR("null"));
    }
    uart_puts_P(PSTR(",\"worker\":\""));
    for (const char *p = settings.worker; *p; p++)
        if (*p != '"' && *p != '\\')
            uart_putc(*p);
    uart_puts_P(settings.console_pass[0] ? PSTR("\",\"pw\":1") : PSTR("\",\"pw\":0"));
    uart_puts_P(PSTR(",\"program\":\"" PROGRAM_NAME "\",\"version\":\"" FIRMWARE_VERSION "\""));
    if (status_json)
        status_json();
    uart_putc('}');
    uart_redirect = 0;
    status_flush();
    /* A fresh signal reading for the next request (the reply is handled
     * whenever it comes). */
    if (joined && !winc_holding())
        winc_send(WINC_GROUP_WIFI, WIFI_REQ_CURRENT_RSSI, 0, 0, 0, 0, 0);
}

static void web_respond(struct web *w)
{
    w->line[w->line_len] = '\0';
    w->body[w->body_len] = '\0';

    if (strncmp_P(w->line, PSTR("GET /status"), 11) == 0
        && (w->line[11] == ' ' || w->line[11] == '?')) {
        web_status(w);
    } else if (strncmp_P(w->line, PSTR("GET / "), 6) == 0) {
        /* Stored gzipped; the browser unpacks it (every browser accepts
         * gzip, so the request's Accept-Encoding is not checked). */
        web_put_P(w, PSTR("HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                          "Content-Encoding: gzip\r\nCache-Control: no-store\r\n"
                          "Connection: close\r\n\r\n"));
        web_put_flash(w, (const char *)WEB_PAGE_GZ, WEB_PAGE_GZ_LEN);
    } else if (strncmp_P(w->line, PSTR("GET /log"), 8) == 0
               && (w->line[8] == ' ' || w->line[8] == '?')) {
        /* GET /log?from=N: the output from position N on (positions count
         * bytes printed since start-up). The page asks from 0 once (start-up
         * part, gap marker, recent part), then from where the last answer
         * ended, so each poll carries only what is new. X-Log-End says where
         * this answer ends; X-Up (ms since start-up) going backwards tells the
         * page the miner has restarted, and it starts again from 0. */
        uint32_t pos = 0, end = log_total;
        char num[11];
        const char *q = strstr_P(w->line, PSTR("from="));
        if (q)
            pos = strtoul(q + 5, 0, 10);
        if (pos > end)
            pos = 0;                    /* from an earlier run of the miner */
        else if (pos >= boot_len && pos < log_oldest())
            pos = log_oldest();         /* fell behind: skip silently, no start-up marker */
        web_put_P(w, PSTR("HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                          "Cache-Control: no-store\r\nX-Log-End: "));
        web_put(w, num, (uint16_t)strlen(ultoa(end, num, 10)));
        web_put_P(w, PSTR("\r\nX-Up: "));
        web_put(w, num, (uint16_t)strlen(ultoa(now_ms(), num, 10)));
        web_put_P(w, PSTR("\r\nConnection: close\r\n\r\n"));
        while (pos < end && w->sock >= 0) {
            const char *p;
            uint8_t flash;
            uint16_t n = log_piece(&pos, &p, &flash);
            if (n == 0)
                break;
            if (flash) {                /* the gap marker */
                web_put_P(w, p);
                continue;
            }
            if (n > end - pos)
                n = (uint16_t)(end - pos);
            if (n > 1024)
                n = 1024;
            if (!web_put(w, p, n))
                break;
            pos += n;
        }
    } else if (strncmp_P(w->line, PSTR("POST /cmd "), 10) == 0) {
        /* Commands must carry the X-Megaminer header. A browser only sends a
         * custom header from the miner's own page: another web site can
         * make a visitor's browser POST a plain form here, but not with this
         * header (that needs the miner's permission, which it never gives),
         * so such a forged command is refused without being looked at. */
        uint8_t ok = 0;
        const char *answer = w->ours ? web_command(w, &ok)
                                     : PSTR("refused: commands come from the miner's own page (X-Megaminer header)");
        web_head(w, ok ? PSTR("200 OK") : PSTR("403 Forbidden"), PSTR("text/plain; charset=utf-8"));
        web_put_P(w, answer);
    } else {
        web_head(w, PSTR("404 Not Found"), PSTR("text/plain"));
        web_put_P(w, PSTR("not found\r\n"));
    }
    w->state = W_DRAIN;
    w->since = now_ms();
}

static void web_service(struct web *w)
{
    switch (w->state) {
    case W_REQUEST:
        if (!w->recv_pending) {
            request_recv((uint8_t)w->sock, w->session, SOCKET_CMD_RECV, 0xffffffffUL);
            w->recv_pending = 1;
        }
        if (now_ms() - w->since > 10000)
            web_drop(w);                /* connected but never asked for anything */
        break;
    case W_RESPOND:
        web_respond(w);
        break;
    case W_DRAIN:
        /* Close once the module has sent everything (or after 3 s). */
        if (w->sends == 0 || now_ms() - w->since > 3000) {
            web_drop(w);
            if (web_reboot == 1)
                web_reboot = 2;         /* answer delivered: reboot now (net_service) */
        }
        break;
    }
}

/* ---- the module's messages about visitors -------------------------------- */

static void side_accept(const uint8_t *reply)
{
    uint8_t listen = reply[8];
    int8_t sock = (int8_t)reply[9];
    struct web *w = 0;

    if (sock < 0)
        return;
    vis_accepts++;
    if (listen == CONSOLE_LISTEN) {
        con_drop(0);                    /* the newest visitor wins */
        con_sock = sock;
        con_session = new_session();
        con_recv_pending = con_sends = con_len = con_iac = con_out_len = con_last_cr = con_too_long = 0;
        con_greet = 1;
        con_authed = !settings.console_pass[0];
        con_pos = 0;                    /* start-up output first, then the recent part */
        uart_puts_P(PSTR("net: console visitor from "));
        put_ip(&reply[4]);
        uart_puts_P(PSTR("\r\n"));
        return;
    }
    if (listen != WEB_LISTEN) {
        socket_close((uint8_t)sock, new_session());
        return;
    }
    /* A free place; else one held by an idle connection (browsers open
     * spares that may never be used); else turn the newcomer away (the
     * browser tries again). */
    for (uint8_t i = 0; i < WEB_VISITORS && !w; i++)
        if (webs[i].sock < 0)
            w = &webs[i];
    for (uint8_t i = 0; i < WEB_VISITORS && !w; i++)
        if (webs[i].state == W_REQUEST && !webs[i].got) {
            w = &webs[i];
            web_drop(w);
        }
    if (!w) {
        vis_turned_away++;
        socket_close((uint8_t)sock, new_session());
        return;
    }
    memset(w, 0, sizeof *w);
    w->sock = sock;
    w->session = new_session();
    w->state = W_REQUEST;
    w->since = now_ms();
}

/* Returns 1 if the reply was for the console or a web visitor. */
static uint8_t side_recv(uint8_t sock, uint16_t session, int16_t status, uint32_t data)
{
    uint8_t buf[32];
    struct web *w = 0;
    uint8_t is_con = con_sock >= 0 && sock == (uint8_t)con_sock && session == con_session;

    for (uint8_t i = 0; i < WEB_VISITORS && !is_con && !w; i++)
        if (webs[i].sock >= 0 && sock == (uint8_t)webs[i].sock && session == webs[i].session)
            w = &webs[i];
    if (!is_con && !w)
        return 0;
    vis_recv_replies++;
    if (is_con)
        con_recv_pending = 0;
    else
        w->recv_pending = 0;
    if (status <= 0) {                  /* they left */
        if (is_con)
            con_drop(1);
        else
            web_drop(w);
        return 1;
    }
    while (status > 0) {
        uint8_t n = status > (int16_t)sizeof buf ? sizeof buf : (uint8_t)status;
        if (!winc_receive(data, buf, n, 0))
            break;
        for (uint8_t i = 0; i < n; i++) {
            if (is_con) {
                if (con_sock < 0)
                    return 1;           /* "quit" earlier in this piece */
                con_byte(buf[i]);
            } else {
                http_char(w, (char)buf[i]);
            }
        }
        data += n;
        status -= n;
    }
    if (w && w->part == 3 && w->state == W_REQUEST)
        w->state = W_RESPOND;
    return 1;
}

static void side_sent(uint8_t sock, uint16_t session, int16_t sent)
{
    if (sent < 0 && side_uses(sock)) {
        vis_send_errors++;
        vis_last_send_error = sent;
    }
    if (con_sock >= 0 && sock == (uint8_t)con_sock && session == con_session && con_sends)
        con_sends--;
    for (uint8_t i = 0; i < WEB_VISITORS; i++) {
        struct web *w = &webs[i];
        if (w->sock >= 0 && sock == (uint8_t)w->sock && session == w->session && w->sends)
            w->sends--;
    }
}

static uint8_t side_uses(uint8_t sock)
{
    if (con_sock >= 0 && sock == (uint8_t)con_sock)
        return 1;
    for (uint8_t i = 0; i < WEB_VISITORS; i++)
        if (webs[i].sock >= 0 && sock == (uint8_t)webs[i].sock)
            return 1;
    return 0;
}

/* label is in flash: put_count(PSTR("label "), n). */
static void put_count(const char *label, int16_t n)
{
    uart_puts_P(label);
    if (n < 0) {
        uart_putc('-');
        n = (int16_t)-n;
    }
    uart_put_u32((uint16_t)n);
}

static void put_listener_state(uint8_t state)
{
    uart_puts_P(state == 0 ? PSTR("idle") : state == 1 ? PSTR("binding")
                : state == 2 ? PSTR("listening asked") : PSTR("ready"));
}

/* Second line of net_report(): the console and web side. */
static void side_report(void)
{
    uart_puts_P(PSTR("net: console listener "));
    put_listener_state(listeners[0].state);
    uart_puts_P(PSTR(", web listener "));
    put_listener_state(listeners[1].state);
    put_count(PSTR(", console socket "), con_sock);
    if (con_sock >= 0) {
        put_count(PSTR(" (pending sends "), con_sends);
        uart_puts_P(con_authed ? PSTR(", unlocked)") : PSTR(", locked)"));
    }
    for (uint8_t i = 0; i < WEB_VISITORS; i++) {
        put_count(PSTR(", web socket "), webs[i].sock);
        if (webs[i].sock >= 0)
            put_count(PSTR(" state "), webs[i].state);
    }
    uart_puts_P(PSTR("\r\nnet: visitors accepted "));
    uart_put_u32(vis_accepts);
    uart_puts_P(PSTR(", turned away "));
    uart_put_u32(vis_turned_away);
    uart_puts_P(PSTR(", data/close replies "));
    uart_put_u32(vis_recv_replies);
    uart_puts_P(PSTR(", sends ok "));
    uart_put_u32(vis_sends_ok);
    uart_puts_P(PSTR(", sends refused "));
    uart_put_u32(vis_sends_failed);
    uart_puts_P(PSTR(", send errors "));
    uart_put_u32(vis_send_errors);
    put_count(PSTR(" (last "), vis_last_send_error);
    uart_puts_P(PSTR(")\r\n"));
}

static void side_reset(void)
{
    listeners[0].state = listeners[1].state = L_IDLE;
    listeners[0].retry_at = listeners[1].retry_at = 0;
    con_sock = -1;                      /* the module has dropped them */
    for (uint8_t i = 0; i < WEB_VISITORS; i++)
        webs[i].sock = -1;
    web_reboot = 0;
}

/* Stack room left right now: between the program's data and the stack. */
extern uint8_t __heap_start;
#define SERVICE_STACK_NEEDED 768        /* answering a visitor goes ~600 bytes deeper */

void net_service(void)
{
    board_telnet(con_sock >= 0);        /* the "someone is on the console" light */
    if (!started)
        return;
    service();
    if (!(joined && have_address))
        return;
    /* net_service is also called from inside the miner's longest
     * calculations, where the stack is deepest. Answering a visitor there
     * could push the stack into the program's data. Leave visitors waiting
     * until the stack has unwound (they get their answer a few seconds
     * later, or the browser retries). */
    if ((uint16_t)SP - (uint16_t)&__heap_start < SERVICE_STACK_NEEDED)
        return;
    for (uint8_t i = 0; i < 2; i++) {
        struct listener *ls = &listeners[i];
        if (ls->state != L_IDLE && ls->state != L_READY && (int32_t)(now_ms() - ls->retry_at) >= 0)
            ls->state = L_IDLE;         /* no answer to bind/listen: start over */
        if (ls->state == L_IDLE)
            listen_setup(ls);
    }
    if (con_sock >= 0)
        con_service();
    for (uint8_t i = 0; i < WEB_VISITORS; i++)
        if (webs[i].sock >= 0)
            web_service(&webs[i]);
    if (web_reboot == 2 && uart_inject_line("reboot"))
        web_reboot = 0;                 /* the program handles it like a typed one */
}

