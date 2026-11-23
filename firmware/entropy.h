/*
 * entropy.h - random numbers for a chip that has no random number generator.
 *
 * The ATmega has two independent clocks: the 20 MHz crystal, and a separate
 * low-quality ~128 kHz oscillator inside the chip that drives the watchdog
 * timer. They drift against each other in a way nobody can predict. Every
 * watchdog tick (about 16 ms) we look at a counter running off the crystal
 * and keep its value; the low bits of that value are the randomness.
 *
 * Uses the watchdog timer (interrupt only, it never resets the chip) and
 * Timer0. The program must call sei() for samples to be collected.
 *
 * How unpredictable the samples really are has NOT been measured on this
 * board. entropy_bytes() therefore mixes 256 samples (about 4 seconds) into
 * every 32 bytes it hands out.
 */

#ifndef ENTROPY_H
#define ENTROPY_H

#include <stdint.h>

#define ENTROPY_RAW_KEPT 32

/* Start collecting. */
void entropy_start(void);

/* Fill `out` with 32 random bytes. Waits until 256 fresh samples have been
 * collected since the previous call (up to ~4 seconds). */
void entropy_bytes(uint8_t out[32]);

/* If set, called over and over while entropy_bytes() waits. */
extern void (*entropy_idle)(void);

/* For looking at the source: the most recent raw samples, oldest first. */
void entropy_raw(uint8_t out[ENTROPY_RAW_KEPT]);

#endif
