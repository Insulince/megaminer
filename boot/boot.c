/*
 * boot.c - the bootloader: lives in the 8 KB boot section (0x1E000), runs
 * first after every reset (BOOTRST fuse programmed, high fuse 0x90), and
 * normally starts the program at once. See bootshare.h for the contract with
 * the programs. Once installed it is
 * meant never to change, so it is kept small and depends only on winc.c and
 * the fixed-format EEPROM block (bootshare.h), not on the program's settings
 * layout.
 *
 * Lights in the bootloader (chosen so that no two states look like the
 * same blinking yellow):
 *   heartbeat  0.2 s on / 0.2 s off = in the bootloader (the program's
 *              heartbeat breathes)
 *   telnet     WiFi: blinking = joining, steady = joined with an address,
 *              dark = not using WiFi (button menu, no WiFi settings)
 *   the six    traffic + luck, read top to bottom (rows 52-54, 57-59) as one
 *              bar: menu choice / upload progress / result
 *
 * 1. Start-up button menu (button held through a reset): every 2 s all three
 *    traffic lights flash (also at the very start) and the luck bar moves
 *    on; release chooses
 *      0 lit  nothing (cancel)
 *      1 lit  clear the console password  } carried out by the program,
 *      2 lit  WiFi setup                  } told through GPIOR0
 *      3 lit  firmware update (stay here)
 *    and after 8 s it starts again from 0 lit (so holding on cancels).
 *
 * 2. Firmware update (chosen above, asked for by the program's "update"
 *    command, or because an earlier update did not finish): start the WiFi
 *    module, join with the saved ssid/pass, and serve HTTP on port 80:
 *      GET /boot          -> 200 "bootloader" (so a page or tool can wait for it)
 *      POST /flash        headers Content-Length, X-Crc32 (hex, of the whole
 *                         image), X-Pass (the console password, if one is set);
 *                         body = the program image as raw bytes from address 0
 *                         -> 200 when written and read back with a matching
 *                         CRC32 (then the chip restarts into it), 4xx/5xx else.
 *      anything else      -> 503 (the miner is not running)
 *    The six lights: all dark = waiting for an upload; while receiving they
 *    fill as a bar in sixths, the next one flashing; full, then all six
 *    flash 3 times = written and verified, starting the new program; red
 *    alone, steady = the last attempt failed (wrong password, bad image,
 *    CRC mismatch, cut off midway) - it keeps waiting for another try; red
 *    alone, blinking (telnet dark) = no WiFi settings saved, nothing to join.
 *    Without an unfinished update, a reset (or 10 minutes without an upload)
 *    returns to the program unchanged.
 *
 * Safety: never writes at or above BOOT_START (it cannot overwrite itself);
 * marks the program incomplete before the first erase and clears that only
 * after the read-back CRC matches. No interrupts are used anywhere.
 *
 * Above 64 KB: this code sits at 0x1E000, beyond the reach of LPM, the
 * instruction behind PSTR()/pgm_read_byte()/strcpy_P(), which only takes a
 * 16-bit address. So: NO PSTR or *_P functions in this file (they would read
 * the program's bytes at the same offset in the lower 64 KB). Text is in
 * plain strings, which the C start-up copies to RAM with ELPM; flash is read
 * only with the *_far functions; the Makefile adds -fno-jump-tables so no
 * switch reads a table out of flash either.
 */
#ifndef F_CPU
#define F_CPU 20000000UL
#endif

#include <avr/boot.h>
#include <avr/eeprom.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <string.h>
#include <util/delay.h>

#include "bootshare.h"
#include "winc.h"

/* ---- lights and button (same pins as board.c) ---- */
#define TELNET      _BV(PA0)                               /* here: WiFi */
#define T_GREEN     _BV(PA1)
#define T_YELLOW    _BV(PA2)
#define T_RED       _BV(PA3)
#define TRAFFIC     (T_GREEN | T_YELLOW | T_RED)
#define LUCK_BAR(n) ((uint8_t)(((1 << (n)) - 1) << PA4))   /* n = 0..3 lit */
#define BAR(n)      ((uint8_t)(((1 << (n)) - 1) << PA1))   /* n = 0..6 of the six lit */
#define LED_PINS    (0x7f)                                 /* PA0-PA6 */
#define MENU_STEP   20                  /* 2 s per menu step, in 0.1 s */
#define BEAT        _BV(PB0)
#define BUTTON_DOWN (!(PIND & _BV(PD2)))

/* After a watchdog reset the watchdog is still running (15 ms): switch it
 * off before anything else, and keep the reset cause for the program. */
void boot_early(void) __attribute__((naked, used, section(".init3")));
void boot_early(void)
{
    GPIOR1 = MCUSR;
    MCUSR = 0;
    wdt_disable();
}

static void restart(void)
{
    wdt_enable(WDTO_15MS);
    for (;;) {
    }
}

static void start_program(uint8_t choice)
{
    PORTA = 0;                          /* pins back as after a reset */
    DDRA = 0;
    PORTD = 0;
    DDRB = 0;
    PORTB = 0;
    GPIOR0 = BOOT_HANDOFF | choice;
    __asm__ __volatile__("jmp 0");
}

/* ---- 1. the button menu ---- */

static uint8_t button_up(void)
{
    if (BUTTON_DOWN)
        return 0;
    _delay_ms(20);                      /* a bounce is not a release */
    return !BUTTON_DOWN;
}

static uint8_t button_menu(void)
{
    uint8_t t = 0;                      /* 0.1 s steps into the 8 s cycle */
    uint8_t choice = 0;

    while (!button_up()) {
        uint8_t a;

        choice = t / MENU_STEP;
        a = LUCK_BAR(choice);
        if (t % MENU_STEP < 3)
            a |= TRAFFIC;               /* flash as each choice begins, the first one too */
        PORTA = a;
        if (t % 2 == 0)
            PINB = BEAT;                /* 0.2 s on / 0.2 s off: the bootloader */
        _delay_ms(100);
        if (++t == 4 * MENU_STEP)
            t = 0;
    }
    PORTA = 0;
    return choice;
}

/* ---- 2. firmware update ---- */

/* WiFi and socket messages (net_winc.c has the details and sources). */
#define WIFI_REQ_CONNECT    40
#define WIFI_RESP_CON_STATE 44
#define WIFI_REQ_DHCP_CONF  50
#define SOCKET_CMD_BIND     0x41
#define SOCKET_CMD_LISTEN   0x42
#define SOCKET_CMD_ACCEPT   0x43
#define SOCKET_CMD_SEND     0x45
#define SOCKET_CMD_RECV     0x46
#define SOCKET_CMD_CLOSE    0x49
#define TCP_DATA_OFFSET     80
#define LISTEN_SOCK         0

static struct boot_creds creds;
static uint8_t  joined, have_address, join_sent, listening, listen_asked;
static int8_t   client = -1;
static uint16_t client_session, session_seq = 0x4000;
static uint8_t  want_recv;              /* ask for the next piece of the request */
static uint8_t  sent_reply;             /* the module answered our send */
static uint32_t ms, last_activity;

/* the HTTP request being read */
enum { H_LINE, H_HEADERS, H_BODY, H_VERIFY, H_DONE };
static uint8_t  h_state;
static char     line[100];
static uint8_t  line_len;
static uint8_t  is_flash, is_get, is_boot, pass_ok, have_crc;
static uint32_t content_len, crc_wanted, body_got;
static const char *reply;               /* set when the answer is ready */

/* the image being written */
static uint8_t  page[SPM_PAGESIZE];
static uint16_t page_fill;
static uint32_t page_addr;
static uint8_t  failed;                 /* red light */
static uint8_t  finished;               /* success: restart after the reply */

/* CRC-32 as zlib, Go's crc32.ChecksumIEEE and the browser page compute it
 * (reflected, polynomial 0xEDB88320), half a byte at a time from a 16-entry
 * table: about 0.5 s for a whole program instead of ~5 s bit by bit. */
static const uint32_t crc_nibble[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

static uint32_t crc32_update(uint32_t crc, uint8_t b)
{
    crc = crc_nibble[(crc ^ b) & 0x0f] ^ (crc >> 4);
    return crc_nibble[(crc ^ (b >> 4)) & 0x0f] ^ (crc >> 4);
}

static void write_page(void)
{
    while (page_fill < SPM_PAGESIZE)
        page[page_fill++] = 0xff;
    if (page_addr >= APP_MAX)           /* never the boot section */
        return;
    boot_page_erase_safe(page_addr);
    boot_spm_busy_wait();
    for (uint16_t i = 0; i < SPM_PAGESIZE; i += 2)
        boot_page_fill_safe(page_addr + i, page[i] | (uint16_t)page[i + 1] << 8);
    boot_page_write_safe(page_addr);
    boot_spm_busy_wait();
    boot_rww_enable_safe();
    page_addr += SPM_PAGESIZE;
    page_fill = 0;
}

static void answer(const char *text, uint8_t ok)
{
    reply = text;
    h_state = H_DONE;
    if (!ok)
        failed = 1;
}

/* Read the whole image back out of flash and check it (from the main loop,
 * not inside a module message: it takes about half a second). */
static void verify(void)
{
    uint32_t crc = 0xffffffffUL;

    for (uint32_t a = 0; a < content_len; a++)
        crc = crc32_update(crc, pgm_read_byte_far(a));
    if ((crc ^ 0xffffffffUL) != crc_wanted) {
        answer("500 written, but the read-back CRC does not match; send it again\n", 0);
        return;
    }
    eeprom_update_byte(BOOT_STATE_EE, 0xff);    /* complete */
    finished = 1;
    answer("200 ok: written and verified; starting it now\n", 1);
}

/* A number in the given base (10 or 16) at the start of s; stops at the
 * first other character. (The library's strtoul is 250 bytes.) */
static uint32_t number(const char *s, uint8_t base)
{
    uint32_t n = 0;

    for (;; s++) {
        uint8_t d;
        if (*s >= '0' && *s <= '9')
            d = (uint8_t)(*s - '0');
        else if (base == 16 && (*s | 0x20) >= 'a' && (*s | 0x20) <= 'f')
            d = (uint8_t)((*s | 0x20) - 'a' + 10);
        else
            return n;
        n = n * base + d;
    }
}

static uint8_t starts(const char *s, const char *with)
{
    return strncasecmp(s, with, strlen(with)) == 0;
}

static void header_line(void)
{
    const char *v = strchr(line, ':');

    if (h_state == H_LINE) {
        is_flash = starts(line, "POST /flash ");
        is_get = starts(line, "GET ");
        is_boot = starts(line, "GET /boot");
        pass_ok = creds.console_pass[0] == '\0';
        have_crc = 0;
        content_len = 0;
        h_state = H_HEADERS;
        return;
    }
    if (line_len) {
        if (!v)
            return;
        for (v++; *v == ' '; v++)
            ;
        if (starts(line, "content-length:"))
            content_len = number(v, 10);
        else if (starts(line, "x-crc32:")) {
            crc_wanted = number(v, 16);
            have_crc = 1;
        } else if (starts(line, "x-pass:") && creds.console_pass[0])
            pass_ok = strcmp(v, creds.console_pass) == 0;
        return;
    }
    /* end of the headers */
    if (is_boot)
        answer("200 bootloader\n", 1);
    else if (is_get)
        answer("503 the miner is in its bootloader (firmware update)\n", 1);
    else if (!is_flash)
        answer("404 not found\n", 0);
    else if (!pass_ok)
        answer("403 wrong console password\n", 0);
    else if (!have_crc || content_len == 0 || content_len > APP_MAX)
        answer("400 needs Content-Length (1-122880) and X-Crc32\n", 0);
    else {
        /* From here on the old program is gone until this one checks out. */
        eeprom_update_byte(BOOT_STATE_EE, BOOT_INCOMPLETE);
        page_addr = 0;
        page_fill = 0;
        body_got = 0;
        failed = 0;
        h_state = H_BODY;
    }
}

static void http_byte(uint8_t c)
{
    if (h_state == H_BODY) {
        page[page_fill++] = c;
        if (page_fill == SPM_PAGESIZE)
            write_page();
        if (++body_got == content_len) {
            if (page_fill)
                write_page();           /* the last, partly filled page */
            h_state = H_VERIFY;
        }
        return;
    }
    if (h_state >= H_VERIFY || c == '\r')
        return;
    if (c != '\n') {
        if (line_len < sizeof line - 1)
            line[line_len++] = (char)c;
        return;
    }
    line[line_len] = '\0';
    header_line();
    line_len = 0;
}

static void client_reset(void)
{
    h_state = H_LINE;
    line_len = 0;
    reply = 0;
    sent_reply = 0;
}

static void socket_close(uint8_t sock, uint16_t session)
{
    uint8_t cmd[4] = { sock, 0, (uint8_t)session, (uint8_t)(session >> 8) };
    winc_send(WINC_GROUP_IP, SOCKET_CMD_CLOSE, cmd, sizeof cmd, 0, 0, 0);
}

static void drop_client(void)
{
    if (client >= 0)
        socket_close((uint8_t)client, client_session);
    client = -1;
}

static void on_message(uint8_t group, uint8_t op, uint16_t len, uint32_t addr)
{
    uint8_t b[16];
    /* The replies differ in length (bind/listen 4, send 8, accept 12, recv
     * 16 + data), and winc_receive refuses to read past the end of a
     * message: read what there is, up to 16. (Reading a flat 16 from every
     * reply was the first-flash bug: the 4-byte bind reply was refused, so
     * port 80 never got to "listen" - yellow flashing for ever.) */
    uint8_t n = len < sizeof b ? (uint8_t)len : (uint8_t)sizeof b;

    memset(b, 0, sizeof b);
    if (group == WINC_GROUP_WIFI) {
        if (op == WIFI_RESP_CON_STATE && winc_receive(addr, b, 4, 0)) {
            join_sent = 0;
            joined = b[0] == 1;
            if (!joined) {              /* (the telnet LED shows it) */
                have_address = listening = listen_asked = 0;
                if (client >= 0)
                    failed = 1;         /* someone was cut off */
                client = -1;
            }
        } else if (op == WIFI_REQ_DHCP_CONF) {
            have_address = 1;
        }
        return;
    }
    if (group != WINC_GROUP_IP || !winc_receive(addr, b, n, 0))
        return;
    switch (op) {
    case SOCKET_CMD_BIND:               /* sock, status, session */
        if (b[1] == 0) {
            uint8_t cmd[4] = { LISTEN_SOCK, 2, b[2], b[3] };
            winc_send(WINC_GROUP_IP, SOCKET_CMD_LISTEN, cmd, sizeof cmd, 0, 0, 0);
        } else {
            listen_asked = 0;           /* try again */
        }
        break;
    case SOCKET_CMD_LISTEN:
        listening = b[1] == 0;
        if (!listening)
            listen_asked = 0;
        break;
    case SOCKET_CMD_ACCEPT: {           /* remote addr (8), listening sock, new sock */
        int8_t sock = (int8_t)b[9];
        if (sock < 0)
            break;
        if (client >= 0) {
            /* One at a time. But browsers open spare connections that may
             * never send anything: a newcomer replaces one of those. */
            if (h_state == H_LINE && line_len == 0) {
                drop_client();
            } else {
                socket_close((uint8_t)sock, ++session_seq);
                break;
            }
        }
        client = sock;
        client_session = ++session_seq;
        client_reset();
        want_recv = 1;
        last_activity = ms;
        break;
    }
    case SOCKET_CMD_RECV: {             /* remote addr (8), status, offset, sock, -, session */
        int16_t status = (int16_t)winc_u16(&b[8]);
        uint32_t data = addr + winc_u16(&b[10]);
        if (client < 0 || b[12] != (uint8_t)client || winc_u16(&b[14]) != client_session)
            break;
        last_activity = ms;
        if (status <= 0) {              /* they left */
            if (h_state == H_BODY)
                failed = 1;             /* in the middle of an upload: red (the image stays marked incomplete) */
            drop_client();
            break;
        }
        while (status > 0 && h_state < H_VERIFY) {
            uint8_t buf[64];
            uint8_t n = status > (int16_t)sizeof buf ? sizeof buf : (uint8_t)status;
            if (!winc_receive(data, buf, n, 0))
                break;
            for (uint8_t i = 0; i < n && h_state < H_VERIFY; i++)
                http_byte(buf[i]);
            data += n;
            status -= n;
        }
        if (h_state < H_VERIFY)
            want_recv = 1;
        break;
    }
    case SOCKET_CMD_SEND:
        sent_reply = 1;
        break;
    default:
        break;
    }
}

static void request_recv(void)
{
    uint8_t cmd[8] = { 0xff, 0xff, 0xff, 0xff, (uint8_t)client, 0,
                       (uint8_t)client_session, (uint8_t)(client_session >> 8) };
    if (winc_send(WINC_GROUP_IP, SOCKET_CMD_RECV, cmd, sizeof cmd, 0, 0, 0))
        want_recv = 0;
}

static void send_reply(void)
{
    static char out[128];
    uint8_t cmd[16];
    uint16_t n;

    /* "200 ok..." -> "HTTP/1.0 200 ok...", status code first, text as the body too */
    strcpy(out, "HTTP/1.0 ");
    n = 9;
    memcpy(out + n, reply, 3);
    n += 3;
    strcpy(out + n, " \r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
    n = (uint16_t)strlen(out);
    strncpy(out + n, reply + 4, sizeof out - n - 1);
    out[sizeof out - 1] = '\0';
    n = (uint16_t)strlen(out);

    memset(cmd, 0, sizeof cmd);
    cmd[0] = (uint8_t)client;
    cmd[2] = (uint8_t)n;
    cmd[3] = (uint8_t)(n >> 8);
    cmd[12] = (uint8_t)client_session;
    cmd[13] = (uint8_t)(client_session >> 8);
    if (winc_send(WINC_GROUP_IP, SOCKET_CMD_SEND | WINC_DATA_PACKET, cmd, sizeof cmd,
                  out, n, TCP_DATA_OFFSET)) {
        reply = 0;                      /* sent; wait for the module's answer */
        last_activity = ms;
    }
}

static void send_join(void)
{
    static uint8_t req[108];            /* tstrM2mWifiConnect, as net_winc.c */

    memset(req, 0, sizeof req);
    strcpy((char *)&req[0], creds.pass);
    req[65] = creds.pass[0] ? 2 : 1;    /* WPA2 or open */
    req[68] = 255;                      /* any channel */
    strcpy((char *)&req[70], creds.ssid);
    req[103] = 1;
    join_sent = winc_send(WINC_GROUP_WIFI, WIFI_REQ_CONNECT, req, sizeof req, 0, 0, 0);
}

static void ask_listen(void)
{
    uint8_t cmd[12] = { 2, 0, BOOT_HTTP_PORT >> 8, BOOT_HTTP_PORT & 0xff, 0, 0, 0, 0,
                        LISTEN_SOCK, 0, 0x01, 0x10 };   /* AF_INET, port, any address, sock, session */
    if (winc_send(WINC_GROUP_IP, SOCKET_CMD_BIND, cmd, sizeof cmd, 0, 0, 0))
        listen_asked = 1;
}

static void update_mode(uint8_t forever)
{
    uint32_t next_try = 0, give_up = 600000UL;  /* 10 minutes, unless forever */
    uint8_t ready = 0;

    eeprom_read_block(&creds, BOOT_CREDS_EE, sizeof creds);
    creds.ssid[sizeof creds.ssid - 1] = creds.pass[sizeof creds.pass - 1] = 0;
    creds.console_pass[sizeof creds.console_pass - 1] = 0;
    if (creds.magic != BOOT_CREDS_MAGIC || creds.sum != BOOT_CREDS_SUM(&creds) || !creds.ssid[0])
        failed = 2;                     /* no WiFi settings: nothing to join with */

    for (;; ms++) {
        _delay_ms(1);
        if (ms % 200 == 0)
            PINB = BEAT;                /* 0.2 s on / 0.2 s off: the bootloader */
        if (failed == 2) {              /* red blinking alone: needs the USBasp or a program that saves ssid/pass */
            PORTA = (ms / 500) & 1 ? T_RED : 0;
            if (!forever && ms >= give_up)
                restart();
            continue;
        }
        if (!ready && ms >= next_try) { /* start (or restart) the module and join */
            next_try = ms + 30000;
            joined = have_address = listening = listen_asked = 0;
            client = -1;
            if (winc_start())
                send_join();
            ready = 0;
        }
        while (winc_poll(on_message))
            ;
        ready = joined && have_address;
        if (ready && !listening && !listen_asked)
            ask_listen();
        if (client >= 0) {
            if (h_state == H_VERIFY)
                verify();
            if (want_recv)
                request_recv();
            if (reply)
                send_reply();
            if (h_state == H_DONE && !reply && (sent_reply || ms - last_activity > 2000)) {
                drop_client();
                if (finished) {         /* success: all six flash 3 times */
                    for (uint8_t i = 0; i < 6; i++) {
                        PORTA = (i & 1 ? 0 : BAR(6)) | TELNET;
                        PINB = BEAT;
                        _delay_ms(200);
                    }
                    restart();          /* -> bootloader -> the new program */
                }
            } else if (ms - last_activity > 20000) {
                drop_client();          /* stalled: an unfinished image stays marked */
                failed = 1;
            }
        }

        /* lights */
        {
            uint8_t a = 0;

            if (finished) {
                a = BAR(6);                         /* full while the answer goes out */
            } else if (client >= 0 && h_state == H_BODY) {
                uint8_t s = (uint8_t)(body_got * 6 / content_len);  /* 0-5 sixths done */
                a = BAR(s);
                if ((ms / 150) & 1)
                    a |= (uint8_t)(T_GREEN << s);   /* the next one flashes */
            } else if (failed) {
                a = T_RED;                          /* last attempt failed */
            }
            if (ready || (ms / 250) & 1)
                a |= TELNET;                        /* WiFi: steady joined, blinking joining */
            PORTA = a;
        }

        if (!forever && h_state != H_BODY && ms >= give_up && client < 0)
            restart();                  /* no update came: back to the program */
    }
}

int main(void)
{
    uint8_t choice = BOOT_CHOICE_NONE, update = 0, incomplete, have_program;

    DDRA = LED_PINS;
    DDRB = BEAT;
    PORTD = _BV(PD2);                   /* button pull-up */
    _delay_ms(5);
    if (BUTTON_DOWN)
        choice = button_menu();

    if (eeprom_read_byte(BOOT_REQUEST_EE) == BOOT_REQ_UPDATE) {
        eeprom_update_byte(BOOT_REQUEST_EE, 0xff);  /* once only */
        update = 1;
    }
    if (choice == BOOT_CHOICE_UPDATE)
        update = 1;
    incomplete = eeprom_read_byte(BOOT_STATE_EE) == BOOT_INCOMPLETE;
    have_program = pgm_read_word_far(0) != 0xffff;

    if (!update && !incomplete && have_program)
        start_program(choice);
    update_mode(incomplete || !have_program);
    return 0;
}
