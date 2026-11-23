/* watchdog.c - see watchdog.h. ATmega1284P datasheet, "Watchdog Timer":
 * WDE + WDIE = interrupt and system reset mode; executing the interrupt
 * clears WDIE, and if it is not set again before the next time-out, that
 * time-out resets the chip. */

#include "watchdog.h"

#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>

#include "uart.h"

static volatile uint8_t fed;        /* set by the program, taken by the next tick */
static uint16_t unfed;              /* ticks since the last feeding (interrupt only) */
static uint8_t  armed;

/* Survives a reset that is not a power cut: the C start-up code leaves
 * .noinit alone, and the bootloader's RAM use is far from it (its variables
 * at the bottom of RAM, its short stack at the very top). */
#define NOTE_MAGIC 0x57D0
static struct {
    uint16_t magic;
    uint8_t  why;
    uint8_t  check;                 /* ~why */
} note __attribute__((section(".noinit")));

/* Counted in EEPROM, clear of the settings (0), the best share (0x200) and
 * the bootloader's block (0xF00-0xFFF). Written only when one happens. */
#define COUNTS_EE ((void *)0x280)
#define COUNTS_MARKER 0xD0
static struct {
    uint8_t  marker;
    uint16_t frozen, stuck;
    uint8_t  check;
} counts;

enum { C_UNKNOWN, C_POWER, C_BUTTON, C_BROWNOUT, C_FROZEN, C_STUCK, C_ASKED, C_BOOT };
static uint8_t cause;
static const char cause_0[] PROGMEM = "unknown";
static const char cause_1[] PROGMEM = "power on";
static const char cause_2[] PROGMEM = "reset button";
static const char cause_3[] PROGMEM = "supply dip";
static const char cause_4[] PROGMEM = "FROZEN";
static const char cause_5[] PROGMEM = "stuck rule";
static const char cause_6[] PROGMEM = "on request";
static const char cause_7[] PROGMEM = "bootloader";
static const char *const cause_names[] PROGMEM = {
    cause_0, cause_1, cause_2, cause_3, cause_4, cause_5, cause_6, cause_7,
};

static void remember(uint8_t why)
{
    note.magic = NOTE_MAGIC;
    note.why = why;
    note.check = (uint8_t)~why;
}

static uint8_t counts_check(void)
{
    return (uint8_t)(0x5A + counts.frozen + (counts.frozen >> 8) + counts.stuck + (counts.stuck >> 8));
}

void watchdog_feed(void)
{
    fed = 1;
}

void watchdog_start(void)
{
    uint8_t sreg = SREG;

    cli();
    fed = 1;
    unfed = 0;
    wdt_reset();
    MCUSR &= (uint8_t)~_BV(WDRF);       /* or WDE could not be changed */
    WDTCSR |= _BV(WDCE) | _BV(WDE);     /* timed sequence: change within 4 clocks */
    WDTCSR = _BV(WDE) | _BV(WDIE);      /* ~16 ms ticks (as entropy.c), interrupt and reset */
    armed = 1;
    SREG = sreg;
}

void watchdog_tick(void)
{
    if (!armed)
        return;
    if (fed) {
        fed = 0;
        unfed = 0;
    } else if (unfed < 0xffff) {
        unfed++;
    }
    if (unfed < WATCHDOG_TICKS)
        WDTCSR |= _BV(WDIE);            /* stay in interrupt mode for one more tick */
    else
        remember(WATCHDOG_FROZEN);      /* not re-armed: the next tick resets the chip */
}

void watchdog_restart(uint8_t why)
{
    remember(why);
    cli();
    wdt_enable(WDTO_15MS);              /* reset mode only, never fed: 15 ms */
    for (;;) {
    }
}

void watchdog_report(void)
{
    uint8_t flags = GPIOR1;             /* MCUSR as the bootloader found it (bootshare.h) */
    uint8_t why = (note.magic == NOTE_MAGIC && (uint8_t)(note.check ^ note.why) == 0xff) ? note.why : 0;
    uint8_t count = 0;

    note.magic = 0;                     /* used once */
    eeprom_read_block(&counts, COUNTS_EE, sizeof counts);
    if (counts.marker != COUNTS_MARKER || counts.check != counts_check()) {
        counts.marker = COUNTS_MARKER;  /* first time (or damaged): start from zero */
        counts.frozen = counts.stuck = 0;
    }

    if (flags & _BV(PORF))
        cause = C_POWER;
    else if (flags & _BV(WDRF))
        switch (why) {
        case WATCHDOG_FROZEN: cause = C_FROZEN; counts.frozen++; count = 1; break;
        case WATCHDOG_STUCK:  cause = C_STUCK;  counts.stuck++;  count = 1; break;
        case WATCHDOG_ASKED:  cause = C_ASKED; break;
        default:              cause = C_BOOT; break;
        }
    else if (flags & _BV(EXTRF))
        cause = C_BUTTON;
    else if (flags & _BV(BORF))
        cause = C_BROWNOUT;
    else
        cause = C_UNKNOWN;
    if (count) {
        counts.check = counts_check();
        eeprom_update_block(&counts, COUNTS_EE, sizeof counts);
    }

    uart_puts_P(PSTR("start-up: "));
    switch (cause) {
    case C_FROZEN:
        uart_puts_P(PSTR("the WATCHDOG restarted the chip: the program had stopped for ~8 s"));
        break;
    case C_STUCK:
        uart_puts_P(PSTR("restarted by the stuck rule (running, but not hashing for too long)"));
        break;
    case C_BOOT:
        uart_puts_P(PSTR("restarted by the bootloader (after a firmware upload), or by the "
                         "watchdog while interrupts were off"));
        break;
    default:
        uart_puts_P((const char *)pgm_read_word(&cause_names[cause]));
        break;
    }
    uart_puts_P(PSTR("; watchdog restarts so far: "));
    uart_put_u32(counts.frozen);
    uart_puts_P(PSTR(" frozen, "));
    uart_put_u32(counts.stuck);
    uart_puts_P(PSTR(" stuck\r\n"));
}

void watchdog_forget(void)
{
    counts.marker = COUNTS_MARKER;
    counts.frozen = counts.stuck = 0;
    counts.check = counts_check();
    eeprom_update_block(&counts, COUNTS_EE, sizeof counts);
    uart_puts_P(PSTR("cmd: resetrestarts\r\nwatchdog restart counts: back to 0 frozen, 0 stuck\r\n"));
}

void watchdog_json(void)
{
    uart_puts_P(PSTR(",\"restart\":\""));
    uart_puts_P((const char *)pgm_read_word(&cause_names[cause]));
    uart_puts_P(PSTR("\",\"frozen\":"));
    uart_put_u32(counts.frozen);
    uart_puts_P(PSTR(",\"stuck\":"));
    uart_put_u32(counts.stuck);
}
