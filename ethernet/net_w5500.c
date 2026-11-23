/*
 * net_w5500.c - net.h implemented with a WIZnet W5500 Ethernet module.
 *
 * The W5500 does TCP/IP by itself. We never build a packet: we write our
 * addresses into its registers, tell one of its eight "sockets" to connect,
 * and then copy bytes in and out of that socket's buffers. All of it happens
 * through SPI register reads and writes.
 *
 * Addresses come from the `settings` structure (settings.h). net_init() may
 * be called again later to restart the network with changed settings. With
 * dhcp=1 our own address, mask and gateway are asked of the LAN's DHCP
 * server instead (messages by dhcp.c), and renewed at half the lease time.
 *
 * Wiring: PB3 -> RST, PB4 -> SCS, PB5 -> MOSI, PB7 -> SCLK
 * and PB6 <- MISO, the last two through 220 ohm series resistors.
 *
 * Register addresses and values are from the W5500 datasheet v1.0.5.
 */

#include <avr/io.h>
#include <util/delay.h>

#include <string.h>

#include "dhcp.h"
#include "net.h"
#include "settings.h"
#include "uart.h"

/* The seconds clock (uptime.c). Renewing a DHCP lease needs it; a program
 * built without one (miner_net) still gets an address but never renews. */
uint32_t platform_seconds(void) __attribute__((weak));

#define W5500_RST   PB3
#define W5500_CS    PB4     /* also the AVR's own SS pin; must be an output */
#define SPI_MOSI    PB5
#define SPI_MISO    PB6
#define SPI_SCK     PB7

/* The W5500's memory is split into blocks; every SPI frame names one.
 * We only use socket 0. */
#define BLOCK_COMMON   0x00     /* chip-wide registers */
#define BLOCK_S0_REG   0x01     /* socket 0 registers */
#define BLOCK_S0_TX    0x02     /* socket 0 transmit buffer */
#define BLOCK_S0_RX    0x03     /* socket 0 receive buffer */

/* Common registers. */
#define GAR        0x0001   /* gateway address, 4 bytes */
#define SUBR       0x0005   /* subnet mask, 4 bytes */
#define SHAR       0x0009   /* our MAC address, 6 bytes */
#define SIPR       0x000F   /* our IP address, 4 bytes */
#define MR         0x0000   /* mode; bit 7 = software reset */
#define RTR        0x0019   /* retry time, 2 bytes; 0x07D0 after a reset */
#define RCR        0x001B   /* retry count; 0x08 after a reset */
#define PHYCFGR    0x002E   /* bit 0 = link up */
#define VERSIONR   0x0039   /* always 0x04 */

/* Socket registers. */
#define Sn_MR      0x0000   /* mode: 0x01 = TCP */
#define Sn_CR      0x0001   /* command; reads 0 once the chip has taken it */
#define Sn_IR      0x0002   /* event flags; write a 1 to clear a flag */
#define Sn_SR      0x0003   /* status */
#define Sn_PORT    0x0004   /* our port, 2 bytes */
#define Sn_DIPR    0x000C   /* server address, 4 bytes */
#define Sn_DPORT   0x0010   /* server port, 2 bytes */
#define Sn_TX_FSR  0x0020   /* free space in the transmit buffer, 2 bytes */
#define Sn_TX_WR   0x0024   /* where our next outgoing byte goes, 2 bytes */
#define Sn_RX_RSR  0x0026   /* bytes waiting in the receive buffer, 2 bytes */
#define Sn_RX_RD   0x0028   /* where the next unread byte is, 2 bytes */
#define Sn_KPALVTR  0x002F   /* keep-alive timer, in units of 5 s; 0 = off */

/* Socket 1 is used briefly for UDP (the time request). */
#define BLOCK_S1_REG   0x05
#define BLOCK_S1_TX    0x06
#define BLOCK_S1_RX    0x07

#define MR_TCP        0x01
#define MR_UDP        0x02
#define CR_CLOSE      0x10
#define SR_UDP        0x22

#define CR_OPEN       0x01
#define CR_CONNECT    0x04
#define CR_DISCON     0x08
#define CR_SEND       0x20
#define CR_RECV       0x40

#define IR_SEND_OK    0x10
#define IR_TIMEOUT    0x08

#define SR_CLOSED       0x00
#define SR_INIT         0x13
#define SR_ESTABLISHED  0x17
#define SR_CLOSE_WAIT   0x1C

/* With a connection open but nothing to say, neither end can tell whether
 * the other is still there. The chip can ask: after this long with no
 * traffic it sends an empty "still there?" packet, and if repeated tries get
 * no acknowledgement (about half a minute) it closes the socket, which we
 * then see as a lost connection and reconnect. */
#define KEEPALIVE_UNITS 2       /* 2 * 5 s = 10 s */

static uint8_t  connected;
static uint8_t  started;            /* net_init() has succeeded at least once */
static uint16_t next_port;
static uint16_t unread_at_close;    /* see SR_CLOSE_WAIT in net_poll */

/* ---- SPI ---------------------------------------------------------------- */

static uint8_t spi_transfer(uint8_t out)
{
    SPDR = out;
    while (!(SPSR & _BV(SPIF)))
        ;
    return SPDR;
}

/* Every frame starts the same way: chip select low, 2 address bytes, then a
 * control byte holding the block number and a read/write bit. */
static void frame_start(uint8_t block, uint16_t addr, uint8_t write)
{
    PORTB &= ~_BV(W5500_CS);
    spi_transfer(addr >> 8);
    spi_transfer(addr & 0xff);
    spi_transfer((uint8_t)(block << 3) | (write ? 0x04 : 0x00));
}

static void frame_end(void)
{
    PORTB |= _BV(W5500_CS);
}

static void reg_read(uint8_t block, uint16_t addr, uint8_t *buf, uint16_t len)
{
    frame_start(block, addr, 0);
    while (len--)
        *buf++ = spi_transfer(0x00);
    frame_end();
}

static void reg_write(uint8_t block, uint16_t addr, const uint8_t *buf, uint16_t len)
{
    frame_start(block, addr, 1);
    while (len--)
        spi_transfer(*buf++);
    frame_end();
}

static uint8_t read8(uint8_t block, uint16_t addr)
{
    uint8_t v;
    reg_read(block, addr, &v, 1);
    return v;
}

static void write8(uint8_t block, uint16_t addr, uint8_t v)
{
    reg_write(block, addr, &v, 1);
}

/* 16-bit registers are stored high byte first. */
static uint16_t read16(uint8_t block, uint16_t addr)
{
    uint8_t b[2];
    reg_read(block, addr, b, 2);
    return (uint16_t)b[0] << 8 | b[1];
}

static void write16(uint8_t block, uint16_t addr, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    reg_write(block, addr, b, 2);
}

/* The size registers can change while we are half-way through reading them.
 * The datasheet says to read until two readings in a row agree. */
static uint16_t read16_stable(uint8_t block, uint16_t addr)
{
    uint16_t a = read16(block, addr);
    for (;;) {
        uint16_t b = read16(block, addr);
        if (a == b)
            return a;
        a = b;
    }
}

/* Give a socket a command and wait for the chip to take it. */
static void command_on(uint8_t reg_block, uint8_t cmd)
{
    write8(reg_block, Sn_CR, cmd);
    for (uint16_t tries = 0; tries < 10000; tries++) {
        if (read8(reg_block, Sn_CR) == 0)
            return;
    }
}

static void command(uint8_t cmd)
{
    command_on(BLOCK_S0_REG, cmd);
}

/* ---- UDP on socket 1 ---------------------------------------------------- */

static uint8_t udp_open(uint16_t local_port)
{
    write8(BLOCK_S1_REG, Sn_IR, 0xff);
    write8(BLOCK_S1_REG, Sn_MR, MR_UDP);
    write16(BLOCK_S1_REG, Sn_PORT, local_port);
    command_on(BLOCK_S1_REG, CR_OPEN);
    return read8(BLOCK_S1_REG, Sn_SR) == SR_UDP;
}

static void udp_close(void)
{
    command_on(BLOCK_S1_REG, CR_CLOSE);
}

/* The chip finds the way there by itself: it asks the LAN who has that
 * address, or hands the packet to the gateway. 255.255.255.255 goes to
 * everyone on the LAN with no questions asked. */
static void udp_send(const uint8_t ip[4], uint16_t port, const uint8_t *data, uint16_t len)
{
    uint16_t at;

    reg_write(BLOCK_S1_REG, Sn_DIPR, ip, 4);
    write16(BLOCK_S1_REG, Sn_DPORT, port);
    at = read16(BLOCK_S1_REG, Sn_TX_WR);
    reg_write(BLOCK_S1_TX, at, data, len);
    write16(BLOCK_S1_REG, Sn_TX_WR, at + len);
    command_on(BLOCK_S1_REG, CR_SEND);
}

/* Take one received datagram, if there is one: copies up to max bytes of it
 * and returns how many (a longer datagram is cut). 0 = nothing waiting. */
static uint16_t udp_receive(uint8_t *buf, uint16_t max)
{
    /* A received datagram is stored as: sender address (4), sender port
     * (2), data length (2), then the data. */
    uint8_t head[8];
    uint16_t at, size, kept;

    if (read16_stable(BLOCK_S1_REG, Sn_RX_RSR) < 8)
        return 0;
    at = read16(BLOCK_S1_REG, Sn_RX_RD);
    reg_read(BLOCK_S1_RX, at, head, 8);
    size = (uint16_t)head[6] << 8 | head[7];
    kept = size > max ? max : size;
    reg_read(BLOCK_S1_RX, at + 8, buf, kept);
    write16(BLOCK_S1_REG, Sn_RX_RD, at + 8 + size);        /* done with all of it */
    command_on(BLOCK_S1_REG, CR_RECV);
    return kept;
}

/* ---- DHCP --------------------------------------------------------------- */

#define DHCP_BUFFER   420       /* replies longer than this are cut; ours are 300 */
#define DHCP_WAIT_MS  3000
#define DHCP_ROUNDS   3         /* then fall back to the typed-in address */

static const uint8_t EVERYONE[4] = { 255, 255, 255, 255 };
static const uint8_t NO_ADDRESS[4] = { 0, 0, 0, 0 };

static uint8_t  dhcp_buffer[DHCP_BUFFER];
static uint8_t  dhcp_wanted;        /* we still have to get an address */
static uint8_t  dhcp_failures;
static uint32_t dhcp_xid;
static struct dhcp_lease lease;     /* seconds == 0: no lease to renew */
static uint32_t renew_at, expires_at;

static void put_address(const char *label, const uint8_t ip[4])
{
    uart_puts(label);
    for (uint8_t i = 0; i < 4; i++) {
        if (i)
            uart_putc('.');
        uart_put_u32(ip[i]);
    }
}

/* What we last wrote, so the guard in net_poll() can tell whether the chip
 * still holds it. */
static uint8_t set_ip[4], set_subnet[4], set_gateway[4];

static void use_addresses(const uint8_t ip[4], const uint8_t subnet[4], const uint8_t gateway[4])
{
    memcpy(set_ip, ip, 4);
    memcpy(set_subnet, subnet, 4);
    memcpy(set_gateway, gateway, 4);
    reg_write(BLOCK_COMMON, GAR,  gateway, 4);
    reg_write(BLOCK_COMMON, SUBR, subnet,  4);
    reg_write(BLOCK_COMMON, SIPR, ip,      4);
}

/* Send the message now in dhcp_buffer to every machine on the LAN and wait
 * for a reply of ours (same xid and MAC). Returns its type, or 0. */
static uint8_t dhcp_ask(struct dhcp_lease *answer)
{
    uint8_t type = 0;

    if (!udp_open(68))
        return 0;
    udp_send(EVERYONE, 67, dhcp_buffer, DHCP_MESSAGE_BYTES);
    for (uint16_t ms = 0; ms < DHCP_WAIT_MS && !type; ms++) {
        uint16_t got = udp_receive(dhcp_buffer, sizeof dhcp_buffer);

        if (got)
            type = dhcp_parse(dhcp_buffer, got, dhcp_xid, settings.mac, answer);
        else
            _delay_ms(1);
    }
    udp_close();
    return type;
}

/* One try at the whole conversation. May take up to about 6 seconds. */
static uint8_t dhcp_acquire(void)
{
    struct dhcp_lease offer, got;

    dhcp_xid = dhcp_xid * 1103515245UL + 12345 + TCNT1 + ((uint32_t)TCNT0 << 16);
    use_addresses(NO_ADDRESS, NO_ADDRESS, NO_ADDRESS);

    dhcp_build(dhcp_buffer, DHCP_DISCOVER, dhcp_xid, settings.mac, settings.worker, 0, 0);
    if (dhcp_ask(&offer) != DHCP_OFFER)
        return 0;
    dhcp_build(dhcp_buffer, DHCP_REQUEST, dhcp_xid, settings.mac, settings.worker, &offer, 0);
    if (dhcp_ask(&got) != DHCP_ACK)
        return 0;

    lease = got;
    use_addresses(lease.ip, lease.subnet, lease.gateway);
    if (platform_seconds && lease.seconds) {
        renew_at = platform_seconds() + lease.seconds / 2;
        expires_at = platform_seconds() + lease.seconds;
    } else {
        lease.seconds = 0;
    }
    put_address("dhcp: address ", lease.ip);
    put_address(" mask ", lease.subnet);
    put_address(" gw ", lease.gateway);
    put_address(" from ", lease.server);
    uart_puts_P(PSTR(", lease "));
    uart_put_u32(got.seconds);
    uart_puts_P(PSTR(" s\r\n"));
    return 1;
}

/* Get an address if we still need one and the cable is in. */
static void dhcp_if_wanted(void)
{
    if (!dhcp_wanted || !net_link_up())
        return;
    if (dhcp_acquire()) {
        dhcp_wanted = 0;
        dhcp_failures = 0;
    } else if (++dhcp_failures >= DHCP_ROUNDS) {
        uart_puts_P(PSTR("dhcp: no server answered; using the typed-in address instead\r\n"));
        use_addresses(settings.ip, settings.subnet, settings.gateway);
        dhcp_wanted = 0;
        dhcp_failures = 0;
    } else {
        uart_puts_P(PSTR("dhcp: no answer yet\r\n"));
    }
}

static void dhcp_start_again(void)
{
    lease.seconds = 0;
    dhcp_wanted = 1;
    command(CR_DISCON);                     /* the connection used the old address */
    connected = 0;
}

/* Ask to keep our address once half its time has gone. */
static void dhcp_renew_if_due(void)
{
    struct dhcp_lease got;
    uint32_t now;
    uint8_t type;

    if (!lease.seconds)
        return;
    now = platform_seconds();
    if ((int32_t)(now - renew_at) < 0)
        return;

    dhcp_xid++;
    dhcp_build(dhcp_buffer, DHCP_REQUEST, dhcp_xid, settings.mac, settings.worker, 0, lease.ip);
    type = dhcp_ask(&got);
    if (type == DHCP_ACK && memcmp(got.ip, lease.ip, 4) == 0 && got.seconds) {
        lease.seconds = got.seconds;
        renew_at = now + got.seconds / 2;
        expires_at = now + got.seconds;
        uart_puts_P(PSTR("dhcp: lease renewed\r\n"));
    } else if (type == DHCP_NAK || type == DHCP_ACK || (int32_t)(now - expires_at) >= 0) {
        uart_puts_P(PSTR("dhcp: the address is no longer ours; asking for a new one\r\n"));
        dhcp_start_again();
    } else {
        renew_at = now + 60;                /* no answer: ask again in a minute */
    }
}

/* ---- net.h -------------------------------------------------------------- */

uint8_t net_init(void)
{
    /* Drive chip select and reset high BEFORE making them outputs, so the
     * module never sees a stray low. */
    PORTB |= _BV(W5500_CS) | _BV(W5500_RST);
    DDRB  |= _BV(W5500_CS) | _BV(W5500_RST) | _BV(SPI_MOSI) | _BV(SPI_SCK);
    DDRB  &= ~_BV(SPI_MISO);

    /* SPI master, mode 0, most significant bit first, F_CPU / 16 = 1.25 MHz. */
    SPCR = _BV(SPE) | _BV(MSTR) | _BV(SPR0);
    SPSR = 0;

    /* Hardware reset: low for at least 500 us, then time to start up. */
    PORTB &= ~_BV(W5500_RST);
    _delay_ms(2);
    PORTB |= _BV(W5500_RST);
    _delay_ms(100);

    if (read8(BLOCK_COMMON, VERSIONR) != 0x04)
        return 0;

    reg_write(BLOCK_COMMON, SHAR, settings.mac, 6);
    lease.seconds = 0;
    dhcp_failures = 0;
    dhcp_wanted = settings.dhcp;
    if (dhcp_wanted) {
        /* The cable's link takes a second or two to come up after the
         * reset above. If it is still down after 5 s, net_poll() gets the
         * address later, once it is up. */
        use_addresses(NO_ADDRESS, NO_ADDRESS, NO_ADDRESS);
        for (uint8_t tenths = 0; tenths < 50 && !net_link_up(); tenths++)
            _delay_ms(100);
        for (uint8_t round = 0; round < DHCP_ROUNDS && dhcp_wanted; round++)
            dhcp_if_wanted();
    } else {
        use_addresses(settings.ip, settings.subnet, settings.gateway);
    }

    /* Start our port numbers somewhere that differs from run to run (the
     * timer has been counting since power-up), so the PC does not confuse a
     * new connection with a stale one from before a reset. */
    next_port = 49152 + (TCNT1 & 0x0fff);
    connected = 0;
    unread_at_close = 0;
    started = 1;
    return 1;
}

/* ---- guard: notice the chip losing its settings ---------------------------
 *
 * Seen 2026-11-17: after 12 h the chip held address 0.0.0.0 and gateway
 * 0.0.0.0 although we never wrote that, so every connection attempt went
 * nowhere. Once a second this compares the chip's addresses with what we
 * wrote and watches the cable link. On a difference it prints everything that
 * tells the possible causes apart, then starts the chip over:
 *   - MAC also zero, RTR/RCR back to 07d0/08, keep-alive 0: the whole chip
 *     reset (power dip on the module, its RST line, or a software reset).
 *     A link drop just before points at power or RST.
 *   - MAC intact, only some values wrong: a stray write over SPI.
 * A version that does not read 04 is counted as an SPI read error. */

static uint8_t  link_was = 2;       /* 2 = not known yet */
static uint32_t link_lost_at;
static uint16_t spi_glitches;

static void put_bytes(const char *label, uint8_t block, uint16_t addr, uint8_t len)
{
    uint8_t b[6];

    reg_read(block, addr, b, len);
    uart_puts(label);
    uart_put_hex(b, len);
}

static void dump_registers(void)
{
    uart_puts_P(PSTR("net: registers"));
    put_bytes(" mac ", BLOCK_COMMON, SHAR, 6);
    put_bytes(" ip ", BLOCK_COMMON, SIPR, 4);
    put_bytes(" mask ", BLOCK_COMMON, SUBR, 4);
    put_bytes(" gw ", BLOCK_COMMON, GAR, 4);
    put_bytes(" mr ", BLOCK_COMMON, MR, 1);
    put_bytes(" rtr ", BLOCK_COMMON, RTR, 2);
    put_bytes(" rcr ", BLOCK_COMMON, RCR, 1);
    put_bytes(" phy ", BLOCK_COMMON, PHYCFGR, 1);
    put_bytes(" s0 mode ", BLOCK_S0_REG, Sn_MR, 1);
    put_bytes(" state ", BLOCK_S0_REG, Sn_SR, 1);
    put_bytes(" keepalive ", BLOCK_S0_REG, Sn_KPALVTR, 1);
    put_bytes(" port ", BLOCK_S0_REG, Sn_PORT, 2);
    uart_puts_P(PSTR("\r\n"));
}

static uint8_t addresses_differ(void)
{
    uint8_t ip[4], subnet[4], gateway[4], mac[6];

    reg_read(BLOCK_COMMON, SIPR, ip, 4);
    reg_read(BLOCK_COMMON, SUBR, subnet, 4);
    reg_read(BLOCK_COMMON, GAR, gateway, 4);
    reg_read(BLOCK_COMMON, SHAR, mac, 6);
    return memcmp(ip, set_ip, 4) || memcmp(subnet, set_subnet, 4)
        || memcmp(gateway, set_gateway, 4) || memcmp(mac, settings.mac, 6);
}

static void guard(void)
{
    static uint32_t last_second;
    static uint8_t polls;
    uint8_t version, link;

    if (!started)
        return;
    if (platform_seconds) {
        uint32_t now = platform_seconds();
        if (now == last_second)
            return;
        last_second = now;
    } else if (++polls & 15) {
        return;
    }

    version = read8(BLOCK_COMMON, VERSIONR);
    if (version != 0x04) {
        spi_glitches++;
        uart_puts_P(PSTR("net: version read as "));
        uart_put_hex(&version, 1);
        uart_puts_P(PSTR(" (SPI read error "));
        uart_put_u32(spi_glitches);
        uart_puts_P(PSTR(" since start-up)\r\n"));
        return;
    }

    link = net_link_up();
    if (link != link_was) {
        if (link_was != 2) {
            if (!link) {
                link_lost_at = platform_seconds ? platform_seconds() : 0;
                uart_puts_P(PSTR("net: cable link went DOWN\r\n"));
            } else {
                uart_puts_P(PSTR("net: cable link back up after "));
                uart_put_u32(platform_seconds ? platform_seconds() - link_lost_at : 0);
                uart_puts_P(PSTR(" s\r\n"));
            }
        }
        link_was = link;
    }

    /* Read twice, so one damaged read cannot trigger a restart. */
    if (!addresses_differ() || !addresses_differ())
        return;
    uart_puts_P(PSTR("net: THE CHIP NO LONGER HOLDS OUR SETTINGS\r\n"));
    dump_registers();
    uart_puts_P(PSTR("net: restarting the network chip\r\n"));
    link_was = 2;
    if (!net_init())
        uart_puts_P(PSTR("net: the chip did not answer after its reset\r\n"));
}

uint8_t net_link_up(void)
{
    return read8(BLOCK_COMMON, PHYCFGR) & 0x01;
}

uint8_t net_wifi_setup(void)
{
    uart_puts_P(PSTR("net: WiFi setup does not apply to the Ethernet chip\r\n"));
    return 0;
}

void net_poll(void)
{
    uint8_t status;

    guard();
    dhcp_renew_if_due();
    status = read8(BLOCK_S0_REG, Sn_SR);

    switch (status) {
    case SR_CLOSED:
        /* Nothing open: open a TCP socket on a fresh local port. */
        if (!net_link_up())
            break;
        if (dhcp_wanted) {
            dhcp_if_wanted();
            break;
        }
        write8(BLOCK_S0_REG, Sn_IR, 0xff);          /* clear old event flags */
        write8(BLOCK_S0_REG, Sn_MR, MR_TCP);
        write16(BLOCK_S0_REG, Sn_PORT, next_port);
        if (++next_port == 0)
            next_port = 49152;
        write8(BLOCK_S0_REG, Sn_KPALVTR, KEEPALIVE_UNITS);
        command(CR_OPEN);
        unread_at_close = 0;
        break;

    case SR_INIT:
        /* Open but not connected: ask the chip to connect to the server.
         * It retries by itself and falls back to CLOSED if nobody answers. */
        reg_write(BLOCK_S0_REG, Sn_DIPR, settings.server_ip, 4);
        write16(BLOCK_S0_REG, Sn_DPORT, settings.server_port);
        command(CR_CONNECT);
        break;

    case SR_CLOSE_WAIT: {
        /* The server closed its side. Whatever it sent just before closing
         * (often the reason) is still in the receive buffer, so keep
         * reporting "connected" until the program has read it, and only
         * then close our side. If the program is not reading (the amount
         * waiting did not shrink since the last poll), close anyway. */
        uint16_t unread = read16_stable(BLOCK_S0_REG, Sn_RX_RSR);

        if (unread != 0 && unread != unread_at_close) {
            unread_at_close = unread;
            connected = 1;
            return;
        }
        command(CR_DISCON);
        break;
    }

    default:
        /* Connecting, established, or closing: nothing to do. */
        break;
    }

    connected = (status == SR_ESTABLISHED);
}

void net_service(void)
{
    /* No network console or web page on the W5500 (retired 2026-11-18). */
}

uint8_t net_connected(void)
{
    return connected;
}

uint16_t net_recv(uint8_t *buf, uint16_t max)
{
    uint16_t waiting = read16_stable(BLOCK_S0_REG, Sn_RX_RSR);
    if (waiting == 0)
        return 0;
    if (waiting > max)
        waiting = max;

    /* Read from where the chip says the unread data starts, then move that
     * marker forward and tell the chip, which frees the space. The chip
     * wraps the address around the end of the buffer for us. */
    uint16_t at = read16(BLOCK_S0_REG, Sn_RX_RD);
    reg_read(BLOCK_S0_RX, at, buf, waiting);
    write16(BLOCK_S0_REG, Sn_RX_RD, at + waiting);
    command(CR_RECV);
    return waiting;
}

uint8_t net_send(const uint8_t *buf, uint16_t len)
{
    /* Wait for room in the transmit buffer (2 KB; our lines are tiny). */
    while (read16_stable(BLOCK_S0_REG, Sn_TX_FSR) < len) {
        if (read8(BLOCK_S0_REG, Sn_SR) != SR_ESTABLISHED)
            return 0;
    }

    uint16_t at = read16(BLOCK_S0_REG, Sn_TX_WR);
    reg_write(BLOCK_S0_TX, at, buf, len);
    write16(BLOCK_S0_REG, Sn_TX_WR, at + len);
    command(CR_SEND);

    /* Wait until the chip reports the data sent, or gives up. */
    for (;;) {
        uint8_t events = read8(BLOCK_S0_REG, Sn_IR);
        if (events & IR_SEND_OK) {
            write8(BLOCK_S0_REG, Sn_IR, IR_SEND_OK);
            return 1;
        }
        if (events & IR_TIMEOUT) {
            write8(BLOCK_S0_REG, Sn_IR, IR_TIMEOUT);
            return 0;
        }
        if (read8(BLOCK_S0_REG, Sn_SR) != SR_ESTABLISHED)
            return 0;
    }
}

void net_report(void)
{
    uint8_t ip[4], gw[4];

    reg_read(BLOCK_COMMON, SIPR, ip, 4);
    reg_read(BLOCK_COMMON, GAR, gw, 4);
    uart_puts(net_link_up() ? "net: link up" : "net: link DOWN");
    put_address(", chip address ", ip);
    put_address(", gw ", gw);
    uart_puts_P(PSTR(", version "));
    uart_put_hex((const uint8_t[]){ read8(BLOCK_COMMON, VERSIONR) }, 1);
    uart_puts_P(PSTR(", socket state "));
    uart_put_hex((const uint8_t[]){ read8(BLOCK_S0_REG, Sn_SR) }, 1);
    uart_puts_P(PSTR(", SPI read errors "));
    uart_put_u32(spi_glitches);
    uart_puts_P(PSTR("\r\n"));
    dump_registers();
}

void net_close(void)
{
    command(CR_DISCON);
    connected = 0;
}

uint16_t net_udp_exchange(const uint8_t ip[4], uint16_t port,
                          const uint8_t *req, uint16_t req_len,
                          uint8_t *resp, uint16_t resp_max)
{
    uint16_t got = 0;

    dhcp_if_wanted();                       /* we need an address of our own first */
    if (!udp_open(40123))
        goto done;
    udp_send(ip, port, req, req_len);

    /* Wait up to ~2 seconds for an answer. */
    for (uint16_t ms = 0; ms < 2000 && !got; ms++) {
        if (read8(BLOCK_S1_REG, Sn_IR) & IR_TIMEOUT)    /* nobody to send it to */
            break;
        got = udp_receive(resp, resp_max);
        if (!got)
            _delay_ms(1);
    }

done:
    command_on(BLOCK_S1_REG, CR_CLOSE);
    return got;
}
