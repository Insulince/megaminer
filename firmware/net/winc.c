/*
 * winc.c - see winc.h. The bus part is the same protocol that winc_id.c
 * proved on the chip (2026-11-18); this adds block writes, retries, and the
 * host interface (m2m_hif.c).
 */

#include <avr/io.h>
#include <avr/pgmspace.h>
#include <util/delay.h>
#include <string.h>

#include "winc.h"

#define WINC_EN     PB1
#define WINC_IRQ    PB2
#define WINC_RST    PB3
#define WINC_CS     PB4
#define SPI_MOSI    PB5
#define SPI_MISO    PB6
#define SPI_SCK     PB7

/* Bus commands (nmspi.c). */
#define CMD_DMA_EXT_WRITE 0xc7
#define CMD_DMA_EXT_READ  0xc8
#define CMD_SINGLE_WRITE  0xc9
#define CMD_SINGLE_READ   0xca
#define CMD_RESET         0xcf

/* Registers (nmspi.c, nmasic.c/h, m2m_hif.c). */
#define SPI_PROTOCOL_CONFIG  0xe824UL
#define CHIP_ID              0x1000UL
#define EFUSE_DONE_REG       0x1014UL
#define NMI_STATE_REG        0x108cUL
#define GP_REG_1             0x14a0UL
#define GP_REG_2             0xc0008UL
#define BOOTROM_REG          0xc000cUL
#define REV_REG              0x207acUL
#define WAIT_FOR_HOST_REG    0x207bcUL
#define NMI_PIN_MUX_0        0x1408UL
#define NMI_INTR_ENABLE      0x1a00UL
#define RCV_CTRL_0           0x1070UL   /* bit 0: message waiting; bit 1: host done with it */
#define RCV_CTRL_1           0x1084UL   /* where the waiting message is */
#define RCV_CTRL_2           0x1078UL   /* bit 1: host asks for buffer space */
#define RCV_CTRL_3           0x106cUL   /* host says: request written */
#define RCV_CTRL_4           0x150400UL /* the buffer the module gave us */

#define FINISH_BOOT_ROM      0x10add09eUL
#define START_FIRMWARE       0xef522f61UL
#define FINISH_INIT_STATE    0x02532636UL

#define HIF_HEADER  8               /* 4-byte header + 4 reserved (M2M_HIF_HDR_OFFSET) */
#define BLOCK_MAX   248             /* largest single block transfer (nm_bus capabilities) */
#define RETRIES     5

#define VERSION(maj, min, pat)  ((uint16_t)((maj) << 8 | ((min) & 15) << 4 | ((pat) & 15)))
#define DRIVER_INFO  ((uint32_t)VERSION(19, 5, 2) << 16 | VERSION(19, 5, 2))

/* Failure reasons, for printing. The bootloader (built with WINC_QUIET) has
 * nowhere to print them, and its 8 KB has no room for the ~400 bytes. */
#ifdef WINC_QUIET
#define WHY(s) ""
#define NO_ERROR ""
#else
#define WHY(s) PSTR(s)                  /* kept in flash: print with uart_puts_P */
static const char no_error[] PROGMEM = "";
#define NO_ERROR no_error
#endif

const char *winc_error = NO_ERROR;
uint32_t winc_chip_id, winc_version;
uint8_t  winc_mac[6];
uint16_t winc_bus_errors;
uint16_t winc_send_no_room;
uint32_t winc_last_value;

uint8_t  winc_seen[12], winc_seen_n;

static uint8_t crc_off;
static uint8_t seen[12], seen_n;    /* reply bytes of the exchange in progress */
static uint8_t started;             /* winc_start succeeded: SPI is set up */

/* The message being handled (hif_isr). */
static uint32_t rx_addr;
static uint16_t rx_size;
static uint8_t  rx_open;
static uint8_t  rx_held;            /* handler asked to keep it open */

#ifdef WINC_QUIET
/* The bootloader's build: no watchdog to feed, and it must stay
 * byte-identical to the one installed. */
static void wait_ms(uint16_t ms) { while (ms--) _delay_ms(1); }
#else
/* Every wait here is bounded (each loop gives up with an error), and the
 * module's start-up can take up to ~10 s: longer than watchdog.c allows
 * without feeding. So these waits feed it (weak: only where it exists). */
void watchdog_feed(void) __attribute__((weak));
static void wait_ms(uint16_t ms)
{
    while (ms--) {
        _delay_ms(1);
        if (watchdog_feed)
            watchdog_feed();
    }
}
#endif

/* ---- SPI ---------------------------------------------------------------- */

static uint8_t spi_byte(uint8_t out)
{
    SPDR = out;
    while (!(SPSR & _BV(SPIF)))
        ;
    return SPDR;
}

/* One chip-select-low transaction; out or in may be 0. */
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
    if (seen_n < sizeof seen)
        seen[seen_n++] = b;
    return b;
}

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

static void send_command(uint8_t *bc, uint8_t len_with_crc)
{
    uint8_t len = len_with_crc;

    seen_n = 0;                         /* a new exchange: record its replies afresh */

    if (!crc_off) {
        uint8_t crc = 0x7f;
        for (uint8_t i = 0; i < len - 1; i++)
            crc = crc7_entry((uint8_t)((crc << 1) ^ bc[i]));
        bc[len - 1] = (uint8_t)(crc << 1);
    } else {
        len--;
    }
    spi_rw(bc, 0, len);
}

static uint8_t command_response(uint8_t cmd)
{
    uint8_t tries, b;

    for (tries = 0, b = read_one(); b != cmd && tries < 10; tries++)
        b = read_one();
    if (b != cmd)
        return 0;
    for (tries = 0, b = read_one(); b != 0x00 && tries < 10; tries++)
        b = read_one();
    return b == 0x00;
}

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

/* After a failed exchange: count it, pause, and let the caller try again.
 *
 * Microchip's driver also sends an SPI reset command (0xcf) here. The first
 * winc_join run (2026-11-18) did that and never got past the boot ROM,
 * where winc_id, which simply retries, got through every time. The driver
 * itself notes that reads during boot can fail ("Why bus error if this
 * delay is not here"), so one failed read during boot would have sent the
 * reset. Suspected, not proven: that reset also returns the module's SPI
 * settings to "CRC on", after which every command we send (without CRC) is
 * refused. So: no reset command, plain retries only. */
static void bus_retry(void)
{
    if (winc_bus_errors < 0xffff)
        winc_bus_errors++;
    memcpy(winc_seen, seen, seen_n);
    winc_seen_n = seen_n;
    wait_ms(1);
}

static uint8_t try_read_reg(uint32_t addr, uint32_t *value)
{
    uint8_t bc[5] = { CMD_SINGLE_READ, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, 0 };
    uint8_t d[4];

    send_command(bc, 5);
    if (!command_response(CMD_SINGLE_READ) || !data_read(d, 4))
        return 0;
    *value = winc_u32(d);
    return 1;
}

static uint8_t try_write_reg(uint32_t addr, uint32_t value)
{
    uint8_t bc[9] = { CMD_SINGLE_WRITE, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
                      (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8),
                      (uint8_t)value, 0 };

    send_command(bc, 9);
    return command_response(CMD_SINGLE_WRITE);
}

static uint8_t try_read_block(uint32_t addr, uint8_t *buf, uint16_t n)
{
    uint8_t bc[8] = { CMD_DMA_EXT_READ, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
                      0, (uint8_t)(n >> 8), (uint8_t)n, 0 };

    send_command(bc, 8);
    return command_response(CMD_DMA_EXT_READ) && data_read(buf, n);
}

/* nm_spi_write: command, then a 0xf3 byte ("only packet"), the data, and a
 * three-byte reply ending c3 00. */
static uint8_t try_write_block(uint32_t addr, const uint8_t *buf, uint16_t n)
{
    uint8_t bc[8] = { CMD_DMA_EXT_WRITE, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
                      0, (uint8_t)(n >> 8), (uint8_t)n, 0 };
    uint8_t mark = 0xf3, rsp[3];

    send_command(bc, 8);
    if (!command_response(CMD_DMA_EXT_WRITE))
        return 0;
    spi_rw(&mark, 0, 1);
    spi_rw(buf, 0, n);
    spi_rw(0, rsp, 3);
    return rsp[1] == 0xc3 && rsp[2] == 0x00;
}

static uint8_t read_reg(uint32_t addr, uint32_t *value)
{
    for (uint8_t i = 0; i < RETRIES; i++) {
        if (try_read_reg(addr, value))
            return 1;
        bus_retry();
    }
    return 0;
}

static uint8_t write_reg(uint32_t addr, uint32_t value)
{
    for (uint8_t i = 0; i < RETRIES; i++) {
        if (try_write_reg(addr, value))
            return 1;
        bus_retry();
    }
    return 0;
}

static uint8_t read_block(uint32_t addr, uint8_t *buf, uint16_t n)
{
    while (n) {
        uint16_t part = n > BLOCK_MAX ? BLOCK_MAX : n;
        uint8_t i;
        for (i = 0; i < RETRIES; i++) {
            if (try_read_block(addr, buf, part))
                break;
            bus_retry();
        }
        if (i == RETRIES)
            return 0;
        addr += part;
        buf += part;
        n -= part;
    }
    return 1;
}

static uint8_t write_block(uint32_t addr, const uint8_t *buf, uint16_t n)
{
    while (n) {
        uint16_t part = n > BLOCK_MAX ? BLOCK_MAX : n;
        uint8_t i, two[2];
        const uint8_t *from = buf;
        uint16_t len = part;
        /* The module mishandles a one-byte block write ("hardware problem
         * with single byte transfers", nmspi.c), so the driver writes two
         * bytes instead: the wanted one plus one more. The module goes by
         * the length in the message header, so the extra byte is ignored.
         *
         * This used to REFUSE one-byte writes ("our requests are whole
         * words"). Wrong: a typed character echoed by the network console
         * is one byte of data. And the refusal came after winc_send had
         * already been given a buffer by the module, which then waited
         * forever for a message that never came: every later send was
         * refused, chip-wide (seen 2026-11-18: console and web went silent
         * at the first keystroke). */
        if (part == 1) {
            two[0] = buf[0];
            two[1] = 0;
            from = two;
            len = 2;
        }
        for (i = 0; i < RETRIES; i++) {
            if (try_write_block(addr, from, len))
                break;
            bus_retry();
        }
        if (i == RETRIES)
            return 0;
        addr += part;
        buf += part;
        n -= part;
    }
    return 1;
}

/* ---- start-up ------------------------------------------------------------ */

static uint8_t fail(const char *what)
{
    winc_error = what;
    if (winc_seen_n == 0) {             /* failed without a retry: keep this exchange's replies */
        memcpy(winc_seen, seen, seen_n);
        winc_seen_n = seen_n;
    }
    return 0;
}

uint8_t winc_start(void)
{
    uint32_t value;
    uint16_t n;

    winc_error = NO_ERROR;
    winc_last_value = 0;
    winc_bus_errors = 0;
    winc_seen_n = 0;
    rx_open = 0;
    rx_held = 0;
    started = 0;

    /* Pins: CS high before it is an output; module off and in reset. */
    PORTB |= _BV(WINC_CS);
    PORTB &= ~(_BV(WINC_EN) | _BV(WINC_RST) | _BV(SPI_MISO));
    DDRB |= _BV(WINC_CS) | _BV(WINC_EN) | _BV(WINC_RST) | _BV(SPI_MOSI) | _BV(SPI_SCK);
    DDRB &= ~(_BV(SPI_MISO) | _BV(WINC_IRQ));
    wait_ms(10);                        /* a restart: let it really go off */

    /* nm_bsp_init / nm_bsp_reset. */
    PORTB |= _BV(WINC_EN);
    wait_ms(10);
    wait_ms(100);                       /* RST has been low all along */
    PORTB |= _BV(WINC_RST);
    wait_ms(100);

    /* SPI master, mode 0, F_CPU / 16 = 1.25 MHz. */
    SPCR = _BV(SPE) | _BV(MSTR) | _BV(SPR0);
    SPSR = 0;

    /* nm_spi_init: CRC is on after a reset; turn it off, 8 KB packets. */
    crc_off = 0;
    if (!try_read_reg(SPI_PROTOCOL_CONFIG, &value)) {
        crc_off = 1;
        if (!try_read_reg(SPI_PROTOCOL_CONFIG, &value))
            return fail(WHY("no answer on the SPI bus"));
    }
    if (!crc_off) {
        value &= ~0x7cUL;
        value |= 0x5UL << 4;
        if (!try_write_reg(SPI_PROTOCOL_CONFIG, value))
            return fail(WHY("could not switch CRC off"));
        crc_off = 1;
    }

    if (!read_reg(CHIP_ID, &winc_chip_id))
        return fail(WHY("could not read the chip id"));

    /* winc_id read the chip id 1000 times here (a bus check) and then got
     * through the boot ROM wait every time; the first winc_join, which went
     * straight on, never did (2026-11-18). Same check here: it proves the bus
     * and also gives the boot ROM the same ~0.2 s head start. */
    for (n = 0; n < 1000; n++) {
        if (!read_reg(CHIP_ID, &value) || value != winc_chip_id) {
            winc_last_value = value;
            return fail(WHY("chip id reads disagree (bus not reliable)"));
        }
    }

    /* wait_for_bootrom */
    for (n = 0; ; n++) {
        if (read_reg(EFUSE_DONE_REG, &value) && (value & 0x80000000UL))
            break;
        if (n > 1000)
            return fail(WHY("efuse never finished loading"));
        wait_ms(1);
    }
    if (!read_reg(WAIT_FOR_HOST_REG, &value))
        return fail(WHY("could not read the wait-for-host register"));
    if (!(value & 1)) {
        /* The driver waits forever here (TIMEOUT 0xffffffff); 10 s will do. */
        for (n = 0; ; n++) {
            if (read_reg(BOOTROM_REG, &value) && value == FINISH_BOOT_ROM)
                break;
            winc_last_value = value;
            if (n > 10000)
                return fail(WHY("boot ROM never finished"));
            wait_ms(1);
        }
    }
    write_reg(NMI_STATE_REG, DRIVER_INFO);
    value = ((winc_chip_id & 0xfff) >= 0x3a0) ? (1UL << 1 | 1UL << 8) : (1UL << 8);
    for (n = 0; ; n++) {
        uint32_t back;
        write_reg(GP_REG_1, value);
        if (read_reg(GP_REG_1, &back) && back == value)
            break;
        if (n > 100)
            return fail(WHY("configuration register would not keep its value"));
    }
    write_reg(BOOTROM_REG, START_FIRMWARE);

    /* wait_for_firmware_start */
    for (n = 0; ; n++) {
        wait_ms(2);
        if (read_reg(NMI_STATE_REG, &value) && value == FINISH_INIT_STATE)
            break;
        winc_last_value = value;
        if (n > 2000)
            return fail(WHY("firmware did not start within 4 s"));
    }
    write_reg(NMI_STATE_REG, 0);

    /* enable_interrupts: route the interrupt to the IRQ pin and enable it. */
    if (!read_reg(NMI_PIN_MUX_0, &value) || !write_reg(NMI_PIN_MUX_0, value | 1UL << 8))
        return fail(WHY("could not route the IRQ pin"));
    if (!read_reg(NMI_INTR_ENABLE, &value) || !write_reg(NMI_INTR_ENABLE, value | 1UL << 16))
        return fail(WHY("could not enable the module's interrupt"));

    if (!read_reg(REV_REG, &winc_version))
        return fail(WHY("could not read the firmware version"));

    /* MAC: GP_REG_2 -> pointer block -> MAC (nmi_get_mac_address). */
    memset(winc_mac, 0, 6);
    if (read_reg(GP_REG_2, &value) && value) {
        uint8_t gp[8];
        if (read_block(value | 0x30000UL, gp, 8))
            read_block((winc_u32(gp) & 0xffff) | 0x30000UL, winc_mac, 6);
    }
    started = 1;
    return 1;
}

/* ---- host interface (m2m_hif.c) ----------------------------------------- */

uint8_t winc_send(uint8_t group, uint8_t op, const void *ctrl, uint16_t ctrl_len,
                  const void *data, uint16_t data_len, uint16_t data_offset)
{
    if (!started)
        return 0;
    uint16_t length = HIF_HEADER + (data ? data_offset + data_len : ctrl_len);
    uint32_t reg, dma_addr = 0;
    uint8_t header[HIF_HEADER] = { group, (uint8_t)(op & 0x7f), (uint8_t)length, (uint8_t)(length >> 8), 0, 0, 0, 0 };

    /* Ask for buffer space of `length` bytes. */
    if (!write_reg(NMI_STATE_REG, group | (uint32_t)op << 8 | (uint32_t)length << 16))
        return 0;
    if (!write_reg(RCV_CTRL_2, 1UL << 1))
        return 0;
    for (uint16_t i = 0; i < 1000; i++) {
        if (!read_reg(RCV_CTRL_2, &reg))
            return 0;
        if (!(reg & (1UL << 1))) {
            if (!read_reg(RCV_CTRL_4, &dma_addr))
                return 0;
            break;
        }
        if (i >= 500)
            wait_ms(1);
    }
    if (dma_addr == 0) {
        if (winc_send_no_room < 0xffff)
            winc_send_no_room++;
        return 0;                       /* module had no room */
    }

    /* Header (8 bytes, the last four reserved; written as zeros), control
     * part, data part; then hand it over. */
    uint32_t at = dma_addr;
    if (!write_block(at, header, HIF_HEADER))
        return 0;
    at += HIF_HEADER;
    if (ctrl && ctrl_len) {
        if (!write_block(at, ctrl, ctrl_len))
            return 0;
        at += ctrl_len;
    }
    if (data && data_len) {
        at += data_offset - ctrl_len;
        if (!write_block(at, data, data_len))
            return 0;
    }
    return write_reg(RCV_CTRL_3, dma_addr << 2 | 1UL << 1);
}

static uint8_t rx_done(void)
{
    uint32_t reg;

    rx_open = 0;
    rx_held = 0;
    if (!read_reg(RCV_CTRL_0, &reg))
        return 0;
    return write_reg(RCV_CTRL_0, reg | 1UL << 1);
}

uint8_t winc_receive(uint32_t addr, void *buf, uint16_t n, uint8_t done)
{
    uint8_t ok;

    if (!rx_open || addr < rx_addr || addr + n > rx_addr + rx_size)
        return 0;
    ok = n == 0 || read_block(addr, buf, n);
    if (done || addr + n >= rx_addr + rx_size)
        rx_done();
    return ok;
}

void winc_hold(void)
{
    if (rx_open)
        rx_held = 1;
}

void winc_release(void)
{
    if (rx_open)
        rx_done();
}

uint8_t winc_holding(void)
{
    return rx_open && rx_held;
}

uint8_t winc_poll(winc_handler_t handler)
{
    uint32_t reg, addr;
    uint8_t header[4];
    uint16_t size, length;

    if (rx_open)
        return 0;                       /* a held message is still being read */
    /* Before winc_start() the SPI port is not even switched on, and a
     * transfer would wait forever (seen 2026-11-18: the console froze). The
     * module is off then too, so its IRQ pin floats and may read low. */
    if (!started)
        return 0;
    if (PINB & _BV(WINC_IRQ))
        return 0;                       /* IRQ is active low: nothing waiting */
    if (!read_reg(RCV_CTRL_0, &reg) || !(reg & 1))
        return 0;                       /* "false interrupt": ignored, as the Arduino port does */
    if (!write_reg(RCV_CTRL_0, reg & ~1UL))
        return 0;
    size = (uint16_t)((reg >> 2) & 0xfff);
    if (size == 0 || !read_reg(RCV_CTRL_1, &addr) || !read_block(addr, header, 4)) {
        rx_open = 1;
        rx_done();
        return 0;
    }
    length = winc_u16(&header[2]);
    rx_addr = addr;
    rx_size = size;
    rx_open = 1;
    if (length < HIF_HEADER || (length != size && size - length > 4)) {
        rx_done();                      /* corrupted header: drop it */
        return 0;
    }
    rx_held = 0;
    handler(header[0], header[1], (uint16_t)(length - HIF_HEADER), addr + HIF_HEADER);
    if (rx_open && !rx_held)
        rx_done();
    return 1;
}
