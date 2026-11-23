/*
 * winc_id.c - "is the WiFi module wired right?" A small stand-alone program
 * that talks to the ATWINC1500 breakout over SPI without joining any
 * network: it proves the wiring and reads which firmware the module carries
 * (Adafruit says 19.3 or older is too old; the miner was written against
 * 19.5.2). Run it on a new board, or when the miner's WiFi will not start.
 *
 *   make flash TARGET=winc_id      over a USBasp, or
 *   make ota TARGET=winc_id        over WiFi, while the miner still runs
 *
 * The verdict shows on the lights, so no serial adapter is needed: traffic
 * green + slow heartbeat blink = all good, traffic red + fast blink =
 * failed. The details (and which step failed) are printed on the serial
 * console (250000 baud, PD0/PD1).
 *
 * To go back to the miner: hold the start-up button through a reset until
 * the menu shows 3 (stay in the bootloader), then `make ota` (no USBasp
 * needed; the bootloader is untouched by this program).
 *
 * Steps, each printed:
 *   1. MISO check with the module switched off, then on but not selected.
 *   2. Power-up: EN high, RST low 100 ms, RST high, wait 100 ms.
 *   3. Read the SPI protocol register with CRC on, then switch CRC off.
 *   4. Read the chip id (0x1000) 1000 times; all must agree.
 *   5. Let the boot ROM finish, start the module's firmware, read its version.
 *   6. Read the module's own MAC address.
 * Ends with "RESULT: ..." and the lights above.
 *
 * The protocol follows Microchip's host driver as shipped in the Arduino
 * WiFi101 library (src/driver/source/nmspi.c, nmasic.c, nmdrv.c, the
 * "USE_OLD_SPI_SW" path): every command and every response byte is its own
 * chip-select-low transaction, exactly as that driver does it.
 *
 * Wiring (as in the README): PB4 -> CS, PB5 -> MOSI, PB7 -> SCK (220 ohm),
 * PB6 <- MISO (220 ohm), PB3 -> RST, PB1 -> EN, PB2 <- IRQ.
 *
 * What the steps tell you when they fail:
 *   1-2  MISO reads 0 = something holds the shared SPI line low.
 *   3    no answer at all = power, CS/SCK/MOSI/MISO wiring, or a module
 *        stuck in a bad state: switch the whole board off and on (a reset
 *        alone does not power-cycle the module), then try again.
 *   4    a chip id that changes = an unreliable bus: loose jumpers, or the
 *        220 ohm resistors on MISO and SCK missing.
 *   5    the module answers but its firmware does not start.
 */

#include <avr/io.h>
#include <util/delay.h>

#include "uart.h"

#define WINC_EN     PB1
#define WINC_IRQ    PB2
#define WINC_RST    PB3
#define WINC_CS     PB4
#define SPI_MOSI    PB5
#define SPI_MISO    PB6
#define SPI_SCK     PB7
#define LED         PB0                 /* the heartbeat */
#define LIGHT_GREEN PA1                 /* traffic light: passed */
#define LIGHT_RED   PA3                 /* traffic light: failed */

/* Commands (nmspi.c). */
#define CMD_DMA_EXT_READ  0xc8
#define CMD_SINGLE_WRITE  0xc9
#define CMD_SINGLE_READ   0xca

/* Registers (nmspi.c, nmasic.h). */
#define SPI_PROTOCOL_CONFIG  0xe824UL
#define CHIP_ID              0x1000UL
#define EFUSE_DONE_REG       0x1014UL     /* bit 31: efuse loaded */
#define NMI_STATE_REG        0x108cUL
#define GP_REG_1             0x14a0UL
#define GP_REG_2             0xc0008UL
#define BOOTROM_REG          0xc000cUL
#define REV_REG              0x207acUL
#define WAIT_FOR_HOST_REG    0x207bcUL

#define FINISH_BOOT_ROM      0x10add09eUL
#define START_FIRMWARE       0xef522f61UL
#define FINISH_INIT_STATE    0x02532636UL

/* What we tell the firmware about ourselves: "driver" 19.5.2, the version of
 * the host driver this file follows (m2m_types.h of WiFi101). */
#define VERSION(maj, min, pat)  ((uint16_t)((maj) << 8 | ((min) & 15) << 4 | ((pat) & 15)))
#define DRIVER_INFO  ((uint32_t)VERSION(19, 5, 2) << 16 | VERSION(19, 5, 2))

static uint8_t crc_off;                 /* CRC switched off in the module yet */
static uint8_t last_seen[12];           /* response bytes, for error reports */
static uint8_t last_seen_n;

/* ---- small helpers ---- */

static void say(const char *s) { uart_puts(s); uart_puts_P(PSTR("\r\n")); }

static void put_hex32(uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    uart_put_hex(b, 4);
}

static void wait_ms(uint16_t ms) { while (ms--) _delay_ms(1); }

__attribute__((noreturn)) static void blink_forever(uint16_t half_period_ms)
{
    for (;;) {
        PORTB ^= _BV(LED);
        wait_ms(half_period_ms);
    }
}

/* ---- SPI ---- */

static uint8_t spi_byte(uint8_t out)
{
    SPDR = out;
    while (!(SPSR & _BV(SPIF)))
        ;
    return SPDR;
}

/* One chip-select-low transaction. Either `out` or `in` may be 0: then zeros
 * are sent, or what comes back is dropped (nm_bus_wrapper's behaviour). */
static void spi_rw(const uint8_t *out, uint8_t *in, uint16_t n)
{
    PORTB &= ~_BV(WINC_CS);
    while (n--) {
        uint8_t got = spi_byte(out ? *out++ : 0);
        if (in)
            *in++ = got;
    }
    PORTB |= _BV(WINC_CS);
}

static uint8_t read_one(void)
{
    uint8_t b;
    spi_rw(0, &b, 1);
    if (last_seen_n < sizeof last_seen)
        last_seen[last_seen_n++] = b;
    return b;
}

/* ---- CRC7 (poly x^7 + x^3 + 1), as the driver's crc7_syndrome_table ---- */

static uint8_t crc7_entry(uint8_t i)
{
    uint8_t c = (i & 0x80) ? i ^ 0x89 : i;
    for (uint8_t bit = 0; bit < 7; bit++) {
        c <<= 1;
        if (c & 0x80)
            c ^= 0x89;
    }
    return c & 0x7f;
}

static uint8_t crc7(const uint8_t *b, uint8_t n)
{
    uint8_t crc = 0x7f;
    while (n--)
        crc = crc7_entry((uint8_t)((crc << 1) ^ *b++));
    return crc;
}

/* ---- the protocol (nmspi.c) ---- */

/* Send a command; the CRC byte is added while CRC is on. */
static void send_command(uint8_t *bc, uint8_t len_with_crc)
{
    uint8_t len = len_with_crc;

    if (!crc_off)
        bc[len - 1] = (uint8_t)(crc7(bc, len - 1) << 1);
    else
        len--;
    spi_rw(bc, 0, len);
}

/* The module echoes the command, then a state byte 0x00. */
static uint8_t command_response(uint8_t cmd)
{
    uint8_t tries, b;

    last_seen_n = 0;
    for (tries = 0, b = read_one(); b != cmd && tries < 10; tries++)
        b = read_one();
    if (b != cmd)
        return 0;
    for (tries = 0, b = read_one(); b != 0x00 && tries < 10; tries++)
        b = read_one();
    return b == 0x00;
}

/* Data comes after a header byte whose top four bits are all ones, and
 * (with CRC on) is followed by two CRC bytes, which the driver ignores. */
static uint8_t data_read(uint8_t *buf, uint16_t n)
{
    uint8_t tries, b;
    uint8_t crc[2];

    for (tries = 0, b = read_one(); (b >> 4) != 0xf && tries < 10; tries++)
        b = read_one();
    if ((b >> 4) != 0xf)
        return 0;
    spi_rw(0, buf, n);
    if (!crc_off)
        spi_rw(0, crc, 2);
    return 1;
}

static uint8_t read_reg(uint32_t addr, uint32_t *value)
{
    uint8_t bc[5] = { CMD_SINGLE_READ, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, 0 };
    uint8_t d[4];

    send_command(bc, 5);
    if (!command_response(CMD_SINGLE_READ) || !data_read(d, 4))
        return 0;
    *value = d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
    return 1;
}

static uint8_t write_reg(uint32_t addr, uint32_t value)
{
    uint8_t bc[9] = { CMD_SINGLE_WRITE, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
                      (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8),
                      (uint8_t)value, 0 };

    send_command(bc, 9);
    return command_response(CMD_SINGLE_WRITE);
}

static uint8_t read_block(uint32_t addr, uint8_t *buf, uint16_t n)
{
    uint8_t bc[8] = { CMD_DMA_EXT_READ, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
                      0, (uint8_t)(n >> 8), (uint8_t)n, 0 };

    send_command(bc, 8);
    return command_response(CMD_DMA_EXT_READ) && data_read(buf, n);
}

__attribute__((noreturn)) static void report_failure(const char *what)
{
    uart_puts_P(PSTR("FAILED: "));
    uart_puts(what);
    uart_puts_P(PSTR(" (bytes seen: "));
    uart_put_hex(last_seen, last_seen_n);
    say(")");
    say("RESULT: failed (red light, fast blink)");
    PORTA |= _BV(LIGHT_RED);
    blink_forever(100);
}

static uint8_t miso_with_pullup(void)
{
    PORTB |= _BV(SPI_MISO);             /* MISO is an input: this turns on its pull-up */
    _delay_us(50);
    uint8_t level = (PINB & _BV(SPI_MISO)) ? 1 : 0;
    PORTB &= ~_BV(SPI_MISO);
    return level;
}

int main(void)
{
    uint32_t value, id, rev;
    uint16_t n;

    uart_init();
    DDRB |= _BV(LED);
    PORTA &= (uint8_t)~(_BV(LIGHT_GREEN) | _BV(LIGHT_RED));
    DDRA |= _BV(LIGHT_GREEN) | _BV(LIGHT_RED);

    /* CS high and EN low before they become outputs, so the module never
     * sees a stray select and stays off until we say so. RST low too. */
    PORTB |= _BV(WINC_CS);
    PORTB &= ~(_BV(WINC_EN) | _BV(WINC_RST));
    DDRB |= _BV(WINC_CS) | _BV(WINC_EN) | _BV(WINC_RST) | _BV(SPI_MOSI) | _BV(SPI_SCK);
    DDRB &= ~(_BV(SPI_MISO) | _BV(WINC_IRQ));

    say("");
    say("winc_id ready");

    /* 1. Is anything holding MISO low? With the pull-up on, a line nobody
     * drives reads 1. */
    uart_puts_P(PSTR("1. MISO with pull-up, module off: "));
    uart_put_u32(miso_with_pullup());
    say(" (1 = nothing drives it)");

    /* 2. Power-up, as nm_bsp_init/nm_bsp_reset do. */
    PORTB |= _BV(WINC_EN);
    wait_ms(10);
    PORTB &= ~_BV(WINC_RST);
    wait_ms(100);
    PORTB |= _BV(WINC_RST);
    wait_ms(100);
    uart_puts_P(PSTR("2. module on. MISO with pull-up, not selected: "));
    uart_put_u32(miso_with_pullup());
    uart_puts_P(PSTR(" ; IRQ pin "));
    uart_put_u32((PINB & _BV(WINC_IRQ)) ? 1 : 0);
    say("");

    /* SPI master, mode 0, MSB first, F_CPU / 16 = 1.25 MHz (as for the W5500;
     * the module allows up to 48 MHz, but slow first). */
    SPCR = _BV(SPE) | _BV(MSTR) | _BV(SPR0);
    SPSR = 0;

    /* 3. nm_spi_init: read the protocol register with CRC on (after a reset
     * the module expects it), then turn CRC off and set 8 KB packets. */
    crc_off = 0;
    if (!read_reg(SPI_PROTOCOL_CONFIG, &value)) {
        crc_off = 1;                    /* maybe still off from an earlier run */
        if (!read_reg(SPI_PROTOCOL_CONFIG, &value))
            report_failure("no answer to the first command, with CRC on or off");
        say("3. note: the module already had CRC off (left over from an earlier run)");
    }
    uart_puts_P(PSTR("3. protocol register "));
    put_hex32(value);
    if (!crc_off) {
        value &= ~0x0cUL;               /* CRC checking off */
        value &= ~0x70UL;
        value |= 0x5UL << 4;            /* data packets up to 8 KB */
        if (!write_reg(SPI_PROTOCOL_CONFIG, value))
            report_failure("writing the protocol register");
        crc_off = 1;
        uart_puts_P(PSTR(", CRC now off"));
    }
    say("");

    /* 4. Chip id, many times: one value, every time. */
    if (!read_reg(CHIP_ID, &id))
        report_failure("reading the chip id");
    for (n = 0; n < 1000; n++) {
        if (!read_reg(CHIP_ID, &value) || value != id) {
            uart_puts_P(PSTR("4. chip id read "));
            uart_put_u32(n);
            uart_puts_P(PSTR(" gave "));
            put_hex32(value);
            uart_puts_P(PSTR(" instead of "));
            put_hex32(id);
            say("");
            report_failure("chip id not stable");
        }
    }
    uart_puts_P(PSTR("4. chip id "));
    put_hex32(id);
    say(" (1000 reads agree; raw 0010xxxx, which Microchip's driver reports as 0015xxxx; revision 2b0 or 3a0 at the end)");

    /* 5. Boot: wait_for_bootrom + wait_for_firmware_start (nmasic.c). */
    for (n = 0; ; n++) {
        if (read_reg(EFUSE_DONE_REG, &value) && (value & 0x80000000UL))
            break;
        if (n > 1000)
            report_failure("efuse never finished loading");
        wait_ms(1);
    }
    if (!read_reg(WAIT_FOR_HOST_REG, &value))
        report_failure("reading the wait-for-host register");
    if (!(value & 1)) {
        for (n = 0; ; n++) {
            if (read_reg(BOOTROM_REG, &value) && value == FINISH_BOOT_ROM)
                break;
            if (n > 2000)
                report_failure("boot ROM never finished (firmware missing from flash?)");
            wait_ms(1);
        }
    }
    say("5. boot ROM done; starting the firmware");
    write_reg(NMI_STATE_REG, DRIVER_INFO);
    /* chip_apply_conf: "use PMU" (revision 3a0 and later) + reserved bit. */
    value = ((id & 0xfff) >= 0x3a0) ? (1UL << 1 | 1UL << 8) : (1UL << 8);
    for (n = 0; ; n++) {
        uint32_t back;
        write_reg(GP_REG_1, value);
        if (read_reg(GP_REG_1, &back) && back == value)
            break;
        if (n > 100)
            report_failure("configuration register would not keep its value");
    }
    write_reg(BOOTROM_REG, START_FIRMWARE);
    for (n = 0; ; n++) {
        wait_ms(2);
        if (read_reg(NMI_STATE_REG, &value) && value == FINISH_INIT_STATE)
            break;
        if (n > 2000)
            report_failure("firmware did not report itself running within 4 s");
    }
    write_reg(NMI_STATE_REG, 0);
    uart_puts_P(PSTR("5. firmware running after about "));
    uart_put_u32((uint32_t)n * 2);
    say(" ms");

    if (!read_reg(REV_REG, &rev))
        report_failure("reading the version register");
    uart_puts_P(PSTR("5. FIRMWARE "));
    uart_put_u32((rev >> 8) & 0xff);
    uart_putc('.');
    uart_put_u32((rev >> 4) & 0x0f);
    uart_putc('.');
    uart_put_u32(rev & 0x0f);
    uart_puts_P(PSTR(" (needs host driver at least "));
    uart_put_u32((rev >> 24) & 0xff);
    uart_putc('.');
    uart_put_u32((rev >> 20) & 0x0f);
    uart_putc('.');
    uart_put_u32((rev >> 16) & 0x0f);
    uart_puts_P(PSTR(", raw "));
    put_hex32(rev);
    say(")");

    /* 6. MAC address: GP_REG_2 points at two words; the first word's low
     * half points at the MAC (nmi_get_mac_address). */
    if (read_reg(GP_REG_2, &value) && value) {
        uint8_t gp[8], mac[6];
        if (read_block(value | 0x30000UL, gp, 8)) {
            uint32_t mib = gp[0] | (uint32_t)gp[1] << 8 | (uint32_t)gp[2] << 16 | (uint32_t)gp[3] << 24;
            if (read_block((mib & 0xffff) | 0x30000UL, mac, 6)) {
                uart_puts_P(PSTR("6. module MAC "));
                for (uint8_t i = 0; i < 6; i++) {
                    if (i)
                        uart_putc(':');
                    uart_put_hex(&mac[i], 1);
                }
                say("");
            } else {
                say("6. could not read the MAC");
            }
        } else {
            say("6. could not read the pointer block");
        }
    } else {
        say("6. no MAC pointer (old firmware keeps it elsewhere)");
    }

    uart_puts_P(PSTR("IRQ pin now "));
    uart_put_u32((PINB & _BV(WINC_IRQ)) ? 1 : 0);
    say("");
    say("RESULT: module answers, firmware read (green light, slow blink)");
    PORTA |= _BV(LIGHT_GREEN);
    blink_forever(500);
}
