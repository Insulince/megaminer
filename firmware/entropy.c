/* entropy.c - see entropy.h. */

#include "entropy.h"

#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/wdt.h>
#include <string.h>

#include "sv2crypto.h"

#define SAMPLES_PER_OUTPUT 256

static volatile uint8_t  pool[32];              /* samples folded together */
static volatile uint8_t  raw[ENTROPY_RAW_KEPT]; /* the latest samples as they came */
static volatile uint8_t  position;
static volatile uint16_t fresh;                 /* samples since the last output */
static uint8_t  previous[32];                   /* the last output, mixed into the next */
static uint32_t outputs;

void (*entropy_idle)(void);

/* watchdog.c, when the program has it (miner_sv2): the same ticks also
 * decide whether the chip has stopped. Weak: absent elsewhere. */
void watchdog_tick(void) __attribute__((weak));

/* Runs on every watchdog tick, about every 16 ms. */
ISR(WDT_vect)
{
    if (watchdog_tick)
        watchdog_tick();                        /* first: it must re-arm before the next tick */

    uint8_t sample = TCNT0;                     /* crystal-driven counter, right now */
    uint8_t i = position & 31;

    /* Rotate the old byte one bit and fold the sample in, so the eight
     * samples that land on each pool byte all leave their mark. */
    pool[i] = (uint8_t)((pool[i] << 1) | (pool[i] >> 7)) ^ sample;
    raw[position % ENTROPY_RAW_KEPT] = sample;
    position++;
    if (fresh < 0xffff)
        fresh++;
}

void entropy_start(void)
{
    uint8_t sreg = SREG;

    TCCR0A = 0;
    TCCR0B = _BV(CS00);                         /* Timer0 free-running at 20 MHz */

    cli();
    wdt_reset();
    MCUSR &= (uint8_t)~_BV(WDRF);               /* or the watchdog could not be changed */
    /* Changing the watchdog needs this two-step unlock (datasheet 11.9.2):
     * set WDCE and WDE together, then write the new setting within 4 clocks.
     * New setting: WDIE only = interrupt each ~16 ms, never a reset. */
    WDTCSR |= _BV(WDCE) | _BV(WDE);
    WDTCSR = _BV(WDIE);
    SREG = sreg;
}

void entropy_bytes(uint8_t out[32])
{
    sha256_ctx ctx;
    uint8_t snapshot[32];
    uint8_t sreg;

    while (fresh < SAMPLES_PER_OUTPUT) {        /* the interrupt is counting */
        if (entropy_idle)
            entropy_idle();
    }

    sreg = SREG;
    cli();
    memcpy(snapshot, (const uint8_t *)pool, sizeof snapshot);
    fresh = 0;
    SREG = sreg;

    outputs++;
    sha256_init(&ctx);
    sha256_update(&ctx, previous, sizeof previous);
    sha256_update(&ctx, snapshot, sizeof snapshot);
    sha256_update(&ctx, (const uint8_t *)&outputs, sizeof outputs);
    sha256_final(&ctx, out);
    memcpy(previous, out, sizeof previous);

    /* `previous` must not be the value we hand out, or the next output could
     * be guessed from this one if no new samples arrived. Hash it once more. */
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)"next", 4);
    sha256_update(&ctx, previous, sizeof previous);
    sha256_final(&ctx, previous);
}

void entropy_raw(uint8_t out[ENTROPY_RAW_KEPT])
{
    uint8_t sreg = SREG;

    cli();
    for (uint8_t i = 0; i < ENTROPY_RAW_KEPT; i++)
        out[i] = raw[(uint8_t)(position + i) % ENTROPY_RAW_KEPT];
    SREG = sreg;
}
