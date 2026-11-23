/*
 * board.h - the miner's lights, recovery button and supply-voltage reading.
 *
 * Pins (all outputs through a 220 ohm resistor to an LED to GND); listed
 * top of the board to the bottom, as wired:
 *   PA0 (pin 40)  telnet   yellow  someone is connected to the console
 *                                  (rows 7-8 R, beside the heartbeat)
 *   PA1 (pin 39)  traffic  green   hashing a live job
 *   PA2 (pin 38)  traffic  yellow  not hashing, on the way (reconnect, key,
 *                                  handshake, retrying after a failure)
 *   PA3 (pin 37)  traffic  red     not hashing, needs you: no WiFi, no
 *                                  authority key, or 3 failures in a row
 *   PA4 (pin 36)  luck 1   green  \
 *   PA5 (pin 35)  luck 2   green   > a bar: 1, 2 or 3 lit for a rare hash
 *   PA6 (pin 34)  luck 3   green  /
 *   PA7 (pin 33)  -        unused (input with pull-up)
 *   PD2 (pin 16)  recovery button to GND (internal pull-up, no resistor)
 *   PB0 (pin 1)   heartbeat yellow, breathing (~2.5 s a breath) while the
 *                                  main program runs (see board_beat below)
 *
 * Luck levels, by the hash's leading zero bits (one hash in 2^bits has them):
 *   8+  -> luck 1 lit for 0.1 s   (about 4 a second at 980 H/s)
 *   16+ -> luck 1-2 lit for 2 s   (about one a minute)
 *   24+ -> luck 1-3 lit for 60 s  (about one every 4.75 hours)
 * A weaker hit never shortens a stronger one's time.
 *
 * Supply voltage: the ADC measures the chip's internal 1.1 V reference
 * against its own supply (AVCC), so VCC = 1.1 V * 1024 / reading. No wiring
 * needed, but the reference is only specified to about +-10%, so compare
 * with a multimeter once.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

#define BOARD_GREEN  0
#define BOARD_YELLOW 1
#define BOARD_RED    2

void     board_init(void);          /* pins and ADC; all LEDs off */
void     board_lamp_test(void);     /* each LED in turn (~1.2 s), heartbeat second, for checking the wiring */

/* The start-up button menu (run by the bootloader, bootshare.h): carries
 * out choice 1 (clears and saves the console password) itself and returns
 * the choice (BOOT_CHOICE_*), so that the program can act on 2 (WiFi setup:
 * net_wifi_setup()). Without a bootloader: the old hold-for-5-s gesture for
 * choice 1. Call once after settings_load(). */
uint8_t  board_recovery(void);

/* Lights for WiFi setup mode (net_winc.c): luck 1-2 lit throughout (the
 * menu choice that got you here) and the traffic light says where it is. */
#define BOARD_SETUP_WAITING   0     /* yellow, blinking: the caller toggles `on` */
#define BOARD_SETUP_TRYING    1     /* yellow steady: joining with the new details */
#define BOARD_SETUP_FAILED    2     /* red: they did not work (shown ~3 s) */
#define BOARD_SETUP_DONE      3     /* all six flash 3 times, then off: saved */
void     board_setup_light(uint8_t state, uint8_t on);

void     board_luck(uint8_t zero_bits);  /* a hash with this many leading zero bits */
void     board_traffic(uint8_t colour);
void     board_telnet(uint8_t on);
void     board_tick(void);          /* call often (every ~0.1 s): luck lights off, heartbeat */

/* The heartbeat, a dead man's switch: a breathing light (board.c, Timer2).
 * The timer sets board_beat_due every ~10 ms, and the brightness moves one
 * step only when the main program calls board_beat(). If the program gets
 * stuck anywhere, the light stops breathing (it holds one brightness).
 * Cheap enough to call for every hash. Every long wait must call it (or
 * board_tick): net_winc.c does in service(), miner.c in miner_while_busy.
 * The same call also feeds the watchdog (watchdog.h): ~8 s without it and
 * the chip restarts itself. */
extern volatile uint8_t board_beat_due;
void board_beat_now(void);
static inline void board_beat(void)
{
    if (board_beat_due)
        board_beat_now();
}

uint16_t board_vcc_mv(void);        /* supply voltage in millivolts */

#endif
