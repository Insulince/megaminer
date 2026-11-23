/*
 * watchdog.h - restart the chip by itself if the program stops (miner_sv2,
 * so that it can run unattended).
 *
 * The chip's watchdog timer is already busy: entropy.c samples its ~16 ms
 * ticks for randomness (and keeps doing so). So it runs in the datasheet's
 * "interrupt and system reset" mode: every tick runs the interrupt, and the
 * hardware disarms itself while doing so. The interrupt re-arms it only
 * while the program has fed it within the last WATCHDOG_TICKS ticks (~8-9 s);
 * after that the next tick resets the whole chip. If the program stops with
 * interrupts off, the interrupt cannot run, and the hardware resets on the
 * second missed tick by itself.
 *
 * Fed by board.c's heartbeat (board_beat_now, twice a second while the
 * program calls board_beat: the same discipline as the heartbeat LED) and by
 * winc.c's bounded start-up waits (up to 10 s). Interrupts must never be off
 * for more than ~16 ms (none in this program are).
 *
 * Restart reasons survive a reset in .noinit RAM (cleared by a power cut):
 *   FROZEN  the interrupt saw no feeding for ~8 s and let the reset happen
 *   STUCK   the miner was alive but not hashing for too long (miner_sv2)
 *   ASKED   reboot / update command
 * Freezes and stuck-rule restarts are counted in EEPROM (0x280), so they
 * show on the web page however long ago they happened.
 */
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdint.h>

#define WATCHDOG_TICKS 500          /* ~16-18 ms each: about 8-9 s without feeding */

#define WATCHDOG_NONE   0
#define WATCHDOG_FROZEN 1
#define WATCHDOG_STUCK  2
#define WATCHDOG_ASKED  3

/* Switch it on (after entropy_start(), with interrupts on). Until then the
 * other functions do nothing harmful. */
void watchdog_start(void);

/* "Still alive." Cheap: sets a flag the next tick looks at. */
void watchdog_feed(void);

/* Called by entropy.c's interrupt on every watchdog tick. */
void watchdog_tick(void);

/* Restart the chip now, remembering why (never returns). */
void watchdog_restart(uint8_t why) __attribute__((noreturn));

/* At start-up, before anything else prints much: why the last reset
 * happened (the bootloader keeps the reset flags in GPIOR1), counted and
 * printed. Then watchdog_json() for /status. */
void watchdog_report(void);
void watchdog_json(void);

/* "resetrestarts": the counts back to zero (e.g. after testing). */
void watchdog_forget(void);

#endif
