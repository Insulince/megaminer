/*
 * board.c - see board.h. ATmega1284P at 20 MHz.
 */
#ifndef F_CPU
#define F_CPU 20000000UL
#endif

#include <avr/interrupt.h>
#include <avr/io.h>
#include <util/delay.h>

#include "board.h"
#include "bootshare.h"
#include "settings.h"
#include "uart.h"

/* From uptime.c. Weak: miner_net has no clock; there the luck lights simply
 * never light (it does not call board_luck anyway). */
uint32_t platform_millis(void) __attribute__((weak));

#define HEARTBEAT_PIN _BV(PB0)

/* ---- the heartbeat: a breathing light ----
 *
 * PB0 has no hardware PWM, so Timer2 makes it: the overflow interrupt
 * switches the LED on, the compare-match interrupt (at OCR2A, 0-255) off.
 * 20 MHz / 128 / 256 = 610 times a second, too fast to see flicker; the
 * two short interrupts cost about 0.2% of the hashing time.
 *
 * Every BEAT_OVERFLOWS overflows (~10 ms) the interrupt sets board_beat_due,
 * and the main program's next board_beat() moves the brightness one step
 * along the breath (256 steps, ~2.5 s per breath). So the LED breathes only
 * while the program is making its rounds: stuck, it holds one brightness.
 * That is the dead man's switch; it also feeds the watchdog (watchdog.h).
 *
 * Brightness: a triangle wave, raised to the power HEARTBEAT_GAMMA, because
 * the eye sees brightness roughly as a power of the light: linear steps in
 * duty look like a fast rise and a long plateau, squared ones like an even
 * breath. HEARTBEAT_FLOOR keeps a faint glow at the bottom, so a breath is
 * never mistaken for "off". Both to be tuned by eye. */
#ifndef HEARTBEAT_GAMMA
#define HEARTBEAT_GAMMA 2               /* 1 = linear, 2 = squared, 3 = cubed */
#endif
#ifndef HEARTBEAT_FLOOR
#define HEARTBEAT_FLOOR 2               /* lowest duty, of 255 */
#endif
#define BEAT_OVERFLOWS 6                /* x 1.64 ms = ~9.8 ms per step: 2.5 s a breath */

volatile uint8_t board_beat_due;
static volatile uint8_t pwm_on;         /* 0: the lamp test drives PB0 by hand */
static volatile uint8_t pwm_duty;
static uint8_t overflows;               /* (interrupt only) */
static uint8_t breath;                  /* where in the breath: 0-255 */

ISR(TIMER2_OVF_vect)
{
    if (pwm_on && pwm_duty)
        PORTB |= HEARTBEAT_PIN;
    if (++overflows == BEAT_OVERFLOWS) {
        overflows = 0;
        board_beat_due = 1;
    }
}

ISR(TIMER2_COMPA_vect)
{
    if (pwm_on && pwm_duty != 255)
        PORTB &= (uint8_t)~HEARTBEAT_PIN;
}

static uint8_t breath_duty(uint8_t step)
{
    uint8_t tri = step < 128 ? (uint8_t)(step * 2) : (uint8_t)((255 - step) * 2);  /* 0..254..0 */
    uint16_t v = tri;

#if HEARTBEAT_GAMMA >= 2
    v = (uint16_t)(v * tri / 255);
#endif
#if HEARTBEAT_GAMMA >= 3
    v = (uint16_t)(v * tri / 255);
#endif
    return v < HEARTBEAT_FLOOR ? HEARTBEAT_FLOOR : (uint8_t)v;
}

/* watchdog.c (weak where absent): fed with the heartbeat, so exactly what
 * keeps the LED breathing keeps the chip from being restarted. */
void watchdog_feed(void) __attribute__((weak));

void board_beat_now(void)
{
    board_beat_due = 0;
    pwm_duty = breath_duty(++breath);
    OCR2A = pwm_duty;
    if (watchdog_feed)
        watchdog_feed();
}

static void heartbeat_start(void)
{
    pwm_duty = breath_duty(0);
    OCR2A  = pwm_duty;
    TCCR2A = 0;                         /* normal mode: counts 0-255, then overflows */
    TCCR2B = _BV(CS22) | _BV(CS20);     /* clock / 128 */
    TIMSK2 = _BV(TOIE2) | _BV(OCIE2A);
    pwm_on = 1;
}

#define TELNET_PIN   _BV(PA0)
#define T_GREEN      _BV(PA1)
#define T_YELLOW     _BV(PA2)
#define T_RED        _BV(PA3)
#define TRAFFIC_PINS (T_GREEN | T_YELLOW | T_RED)
#define LUCK_PIN(i)  _BV(PA4 + (i))     /* i = 0, 1, 2 */
#define LUCK_PINS    (LUCK_PIN(0) | LUCK_PIN(1) | LUCK_PIN(2))
#define SPARE_PIN    _BV(PA7)
#define LED_PINS     (LUCK_PINS | TRAFFIC_PINS | TELNET_PIN)

#define BUTTON_PIN   _BV(PD2)
#define BUTTON_DOWN  (!(PIND & BUTTON_PIN))

static const uint16_t luck_hold_ms[3] = { 100, 2000, 60000 };
static uint32_t luck_until[3];          /* ms clock; each LED off after its own time */

void board_init(void)
{
    PORTA = SPARE_PIN;                  /* LEDs off; the unused pin pulled up */
    DDRA  = LED_PINS;
    DDRB |= HEARTBEAT_PIN;
    DDRD  &= (uint8_t)~BUTTON_PIN;
    PORTD |= BUTTON_PIN;                /* button pulls the pin to GND; pull-up otherwise */

    /* ADC: reference = AVCC (the supply), input = the internal 1.1 V
     * bandgap (MUX 11110), clock 20 MHz / 128 = 156 kHz (50-200 kHz wanted
     * for full accuracy). Left on and pointed there: nothing else uses the
     * ADC, and the bandgap needs time to settle after being selected. */
    ADMUX  = _BV(REFS0) | 0x1e;
    ADCSRA = _BV(ADEN) | _BV(ADPS2) | _BV(ADPS1) | _BV(ADPS0);

    heartbeat_start();
}

void board_lamp_test(void)
{
    /* Telnet, then the heartbeat (0 = PB0, on port B), then traffic and luck. */
    static const uint8_t order[] = { TELNET_PIN, 0, T_GREEN, T_YELLOW, T_RED,
                                     LUCK_PIN(0), LUCK_PIN(1), LUCK_PIN(2) };

    pwm_on = 0;                         /* the breathing pauses meanwhile */
    PORTB &= (uint8_t)~HEARTBEAT_PIN;
    for (uint8_t i = 0; i < sizeof order; i++) {
        if (order[i])
            PORTA |= order[i];
        else
            PORTB |= HEARTBEAT_PIN;
        _delay_ms(150);
        PORTA &= (uint8_t)~order[i];
        PORTB &= (uint8_t)~HEARTBEAT_PIN;
    }
    pwm_on = 1;
}

static void clear_console_password(void)
{
    settings_command("console.password=");
    settings_command("save");
    uart_puts_P(PSTR("recovery: console password CLEARED\r\n"));
    for (uint8_t i = 0; i < 3; i++) {   /* all three traffic lights flash: done */
        PORTA |= TRAFFIC_PINS;
        _delay_ms(200);
        PORTA &= (uint8_t)~TRAFFIC_PINS;
        _delay_ms(200);
        board_beat();
    }
}

uint8_t board_recovery(void)
{
    /* With the bootloader (boot.c) installed, it runs the button menu and
     * leaves its verdict in GPIOR0 (bootshare.h). */
    uint8_t handoff = GPIOR0;

    GPIOR0 = 0;
    if ((handoff & 0xf0) == BOOT_HANDOFF) {
        switch (handoff & 0x0f) {
        case BOOT_CHOICE_CLEAR_PASS:
            uart_puts_P(PSTR("button menu: 1 = clear the console password\r\n"));
            clear_console_password();
            return BOOT_CHOICE_CLEAR_PASS;
        case BOOT_CHOICE_WIFI_SETUP:
            uart_puts_P(PSTR("button menu: 2 = WiFi setup\r\n"));
            return BOOT_CHOICE_WIFI_SETUP;
        default:
            return BOOT_CHOICE_NONE;
        }
    }

    /* No bootloader: the original hold-for-5-s gesture, handled here. */
    _delay_ms(5);                       /* let the pull-up charge the pin */
    if (!BUTTON_DOWN)
        return BOOT_CHOICE_NONE;

    uart_puts_P(PSTR("recovery button held: keep holding for 5 s to clear the console password\r\n"));
    for (uint8_t i = 0; i < 50; i++) {
        if (i % 2 == 0)
            PORTA ^= T_RED;             /* red blinks while you hold */
        _delay_ms(100);
        board_beat();
        if (!BUTTON_DOWN) {
            PORTA &= (uint8_t)~T_RED;
            uart_puts_P(PSTR("recovery button released early; nothing changed\r\n"));
            return BOOT_CHOICE_NONE;
        }
    }
    PORTA &= (uint8_t)~T_RED;
    clear_console_password();
    for (uint8_t i = 0; i < 100 && BUTTON_DOWN; i++) {
        _delay_ms(50);                  /* wait (up to 5 s) for the button to be let go */
        board_beat();
    }
    return BOOT_CHOICE_CLEAR_PASS;
}

void board_setup_light(uint8_t state, uint8_t on)
{
    uint8_t a = LUCK_PIN(0) | LUCK_PIN(1);

    if (state == BOARD_SETUP_DONE) {
        for (uint8_t i = 0; i < 6; i++) {   /* as the bootloader's "written": all six flash 3 times */
            PORTA = (uint8_t)((PORTA & ~(TRAFFIC_PINS | LUCK_PINS)) | (i & 1 ? 0 : TRAFFIC_PINS | LUCK_PINS));
            _delay_ms(200);
            board_beat();
        }
        return;
    }
    if (state == BOARD_SETUP_FAILED)
        a |= T_RED;
    else if (state == BOARD_SETUP_TRYING || on)
        a |= T_YELLOW;
    PORTA = (uint8_t)((PORTA & ~(TRAFFIC_PINS | LUCK_PINS)) | a);
}

void board_luck(uint8_t zero_bits)
{
    uint8_t level = zero_bits >= 24 ? 3 : zero_bits >= 16 ? 2 : zero_bits >= 8 ? 1 : 0;
    uint32_t until;

    if (!level || !platform_millis)
        return;
    until = platform_millis() + luck_hold_ms[level - 1];
    for (uint8_t i = 0; i < level; i++) {
        if (!(PORTA & LUCK_PIN(i)) || (int32_t)(until - luck_until[i]) > 0)
            luck_until[i] = until;
        PORTA |= LUCK_PIN(i);
    }
}

void board_tick(void)
{
    uint32_t now;

    board_beat();
    if (!(PORTA & LUCK_PINS) || !platform_millis)
        return;
    now = platform_millis();
    for (uint8_t i = 0; i < 3; i++)
        if ((PORTA & LUCK_PIN(i)) && (int32_t)(now - luck_until[i]) >= 0)
            PORTA &= (uint8_t)~LUCK_PIN(i);
}

void board_traffic(uint8_t colour)
{
    uint8_t pin = colour == BOARD_GREEN ? T_GREEN : colour == BOARD_YELLOW ? T_YELLOW : T_RED;

    PORTA = (uint8_t)((PORTA & ~TRAFFIC_PINS) | pin);
}

void board_telnet(uint8_t on)
{
    if (on)
        PORTA |= TELNET_PIN;
    else
        PORTA &= (uint8_t)~TELNET_PIN;
}

uint16_t board_vcc_mv(void)
{
    uint32_t sum = 0;

    for (uint8_t i = 0; i < 9; i++) {
        ADCSRA |= _BV(ADSC);
        while (ADCSRA & _BV(ADSC)) {
        }
        if (i)                          /* the first conversion is thrown away */
            sum += ADC;
    }
    if (!sum)
        return 0;
    return (uint16_t)(1100UL * 1024UL * 8 / sum);
}
