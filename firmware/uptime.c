/*
 * uptime.c - see uptime.h. ATmega1284P at 20 MHz, Timer1.
 */
#ifndef F_CPU
#define F_CPU 20000000UL
#endif
#if F_CPU != 20000000UL
#error "the one-second timer value below assumes a 20 MHz clock"
#endif

#include <avr/interrupt.h>
#include <avr/io.h>

#include "board.h"
#include "uptime.h"

static volatile uint32_t uptime;

/* (The heartbeat LED is board.c's, on Timer2.) */
ISR(TIMER1_COMPA_vect)
{
    uptime++;
}

void uptime_start(void)
{
    /* One interrupt a second: 20 MHz / 1024 = 19,531.25 ticks per second. */
    OCR1A  = 19530;
    TCCR1A = 0;
    TCCR1B = _BV(WGM12) | _BV(CS12) | _BV(CS10);
    TIMSK1 = _BV(OCIE1A);
}

uint32_t platform_seconds(void)
{
    uint32_t s;
    uint8_t sreg = SREG;

    cli();
    s = uptime;
    SREG = sreg;
    return s;
}

/* Timer1 counts 0..19530 within each second (51.2 us a step), so reading it
 * gives the fraction of the current second. */
uint32_t platform_millis(void)
{
    uint32_t s;
    uint16_t t;
    uint8_t sreg = SREG;

    cli();
    t = TCNT1;
    s = uptime;
    /* The second may have rolled over with its interrupt still waiting
     * (interrupts are off here). If so, and the counter was read after the
     * roll-over (it is small), count that second. */
    if ((TIFR1 & _BV(OCF1A)) && t < 9765)
        s++;
    SREG = sreg;
    return s * 1000UL + (uint32_t)t * 1000UL / 19531UL;
}
