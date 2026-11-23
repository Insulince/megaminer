/*
 * sv2_chip_test.c - Stratum V2 stage 4: does the handshake code work on the
 * ATmega, and how long does it take?
 *
 * Build/flash:  make flash TARGET=sv2_chip_test
 * PC side:      PuTTY on the adapter's COM port, 250000 8N1.
 *
 * No network is used. At reset it:
 *   1. times each slow step of the handshake once
 *   2. collects random numbers and makes a real one-time key from them
 *   3. runs the same test vectors as `make test-sv2` on the PC
 *   4. reports the deepest the stack got
 * then blinks the LED slowly. Press reset to run it again: the "random" and
 * "one-time key" lines must be different on every run.
 *
 * To go back to mining afterwards:  make flash TARGET=miner_sv1
 * (flashing may erase the saved settings).
 */

#ifndef F_CPU
#define F_CPU 20000000UL
#endif

#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <util/delay.h>
#include <stdint.h>
#include <string.h>

#include "entropy.h"
#include "sv2crypto.h"
#include "sv2curve.h"
#include "uart.h"

/* Keep the test vectors in flash; the chip has 16 KB of RAM and they are
 * about 15.7 KB. Each one is copied into RAM just before it is used. */
#define SV2_VECTOR_ATTR PROGMEM
#include "sv2crypto_vectors.h"
#include "sv2curve_vectors.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define LOAD(dst, array, i) memcpy_P(&(dst), &(array)[i], sizeof(dst))

#if F_CPU != 20000000UL
#error "the millisecond timer below assumes a 20 MHz clock"
#endif

/* ---- milliseconds since reset: Timer1 counts 20 MHz / 8 = 2.5 MHz and
 * interrupts every 2500 counts ---- */
static volatile uint32_t millis_now;

ISR(TIMER1_COMPA_vect)
{
    millis_now++;
}

static uint32_t millis(void)
{
    uint32_t m;
    uint8_t sreg = SREG;

    cli();
    m = millis_now;
    SREG = sreg;
    return m;
}

/* ---- stack depth: paint the unused RAM, later see how much was overwritten ---- */
extern uint8_t __heap_start;                /* first byte after all the program's variables */
#define __bss_end __heap_start
#define PAINT 0xa5

static void stack_paint(void)
{
    uint8_t *limit = (uint8_t *)SP - 32;    /* SP = where the stack is right now */

    for (uint8_t *p = &__bss_end; p < limit; p++)
        *p = PAINT;
}

static uint16_t stack_untouched(void)
{
    const uint8_t *p = &__bss_end;

    while (*p == PAINT)
        p++;
    return (uint16_t)(p - &__bss_end);
}

/* ---- reporting ---- */
static uint16_t failures;

static void check(const char *what, uint8_t index, uint8_t ok)
{
    if (ok)
        return;
    uart_puts("\r\nFAIL ");
    uart_puts(what);
    uart_puts(" #");
    uart_put_u32(index);
    uart_puts("\r\n");
    failures++;
}

static void report_ms(const char *label, uint32_t ms)
{
    uart_puts(label);
    uart_put_u32(ms);
    uart_puts(" ms\r\n");
}

static void line_hex(const char *label, const uint8_t *bytes, uint8_t len)
{
    uart_puts(label);
    uart_put_hex(bytes, len);
    uart_puts("\r\n");
}

/* ---- 1. timings ---- */
static uint32_t time_handshake_steps(void)
{
    static struct key_vector key;
    static struct ecdh_vector ecdh;
    static struct schnorr_vector schnorr;
    uint8_t seed[32], ell[64], out[32], buf[116];
    uint32_t start, t_create, t_decode, t_ecdh, t_verify, t;
    uint8_t ok;

    LOAD(key, key_vectors, 0);
    LOAD(ecdh, ecdh_vectors, 0);
    LOAD(schnorr, schnorr_vectors, 0);
    memset(seed, 0x42, sizeof seed);

    uart_puts("\r\n-- timings --\r\n");

    start = millis();
    ok = sv2_ellswift_create(key.priv, seed, ell);
    t_create = millis() - start;
    report_ms("make one-time key (1 curve multiplication + encode): ", t_create);

    start = millis();
    sv2_ellswift_decode(ell, out);
    t_decode = millis() - start;
    report_ms("decode a key:                                        ", t_decode);
    check("timing: created key decodes to the right public key", 0,
          ok && memcmp(out, key.x, 32) == 0);

    start = millis();
    ok = sv2_ecdh(ecdh.priv, ecdh.theirs, ecdh.ours, ecdh.initiating, out);
    t_ecdh = millis() - start;
    report_ms("one key agreement (decode + 1 multiplication):       ", t_ecdh);
    check("timing: key agreement result", 0, ok && memcmp(out, ecdh.secret, 32) == 0);

    start = millis();
    ok = sv2_schnorr_verify(schnorr.pubkey, schnorr.msg, schnorr.sig);
    t_verify = millis() - start;
    report_ms("check the certificate signature (2 multiplications): ", t_verify);
    check("timing: signature accepted", 0, ok);

    /* The per-message cost once mining: seal a 100-byte message, 50 times. */
    memset(buf, 0x17, sizeof buf);
    start = millis();
    for (uint8_t i = 0; i < 50; i++)
        aead_seal(seed, i, 0, 0, buf, 100, buf);
    t = millis() - start;
    report_ms("encrypt 50 messages of 100 bytes:                    ", t);

    t = t_ecdh * 2 + t_verify;
    report_ms("=> before connecting (make the key):                 ", t_create);
    report_ms("=> after the server's reply (2 agreements + check):  ", t);
    return t;
}

/* ---- 2. randomness ---- */
static void try_randomness(void)
{
    uint8_t raw[ENTROPY_RAW_KEPT], priv[32], seed[32], ell[64], x[32];
    uint8_t seen[32] = {0}, distinct = 0, tries = 0;
    uint32_t start;

    uart_puts("\r\n-- randomness (watchdog clock against the crystal) --\r\n");

    start = millis();
    entropy_bytes(priv);
    report_ms("256 samples collected in ", millis() - start);

    entropy_raw(raw);
    line_hex("last 32 raw samples: ", raw, sizeof raw);
    for (uint8_t i = 0; i < sizeof raw; i++) {
        uint8_t mask = (uint8_t)(1u << (raw[i] & 7));
        if (!(seen[raw[i] >> 3] & mask)) {
            seen[raw[i] >> 3] |= mask;
            distinct++;
        }
    }
    uart_puts("different values among them: ");
    uart_put_u32(distinct);
    uart_puts(" of 32 (a stuck source would show 1 or 2)\r\n");

    /* Exactly what the miner will do before each connection. */
    for (;;) {
        uint8_t ok;

        if (tries++)
            entropy_bytes(priv);
        start = millis();
        entropy_bytes(seed);
        report_ms("waited for 256 fresh samples: ", millis() - start);
        start = millis();
        ok = sv2_ellswift_create(priv, seed, ell);
        report_ms("made a one-time key from them: ", millis() - start);
        if (ok)
            break;
        uart_puts("(that key was out of range; trying again)\r\n");
    }
    line_hex("random  ", seed, 32);
    line_hex("one-time key  ", ell, 64);
    sv2_ellswift_decode(ell, x);
    line_hex("  its public key x  ", x, 32);
    uart_puts("(press reset later: these three lines must come out different)\r\n");
}

/* ---- 3. the PC's test vectors ---- */
static void test_crypto(void)
{
    static struct sha_vector sha;
    static struct hmac_vector hm;
    static struct hkdf_vector hk;
    static struct aead_vector ae;
    static uint8_t buf[216];
    uint8_t out[32], out2[32];

    uart_puts("\r\n-- sv2crypto vectors --\r\n");

    for (uint8_t i = 0; i < COUNT(sha_vectors); i++) {
        sha256_ctx ctx;

        LOAD(sha, sha_vectors, i);
        sha256_init(&ctx);
        sha256_update(&ctx, sha.data, sha.len);
        sha256_final(&ctx, out);
        check("sha256", i, memcmp(out, sha.want, 32) == 0);

        sha256_init(&ctx);
        for (uint16_t j = 0; j < sha.len; j++)
            sha256_update(&ctx, sha.data + j, 1);
        sha256_final(&ctx, out);
        check("sha256 bytewise", i, memcmp(out, sha.want, 32) == 0);
    }
    uart_puts("sha256 done\r\n");

    for (uint8_t i = 0; i < COUNT(hmac_vectors); i++) {
        LOAD(hm, hmac_vectors, i);
        hmac_sha256(hm.key, hm.key_len, hm.data, hm.len, out);
        check("hmac", i, memcmp(out, hm.want, 32) == 0);
    }
    uart_puts("hmac done\r\n");

    for (uint8_t i = 0; i < COUNT(hkdf_vectors); i++) {
        LOAD(hk, hkdf_vectors, i);
        noise_hkdf2(hk.ck, hk.material, hk.len, out, out2);
        check("hkdf", i, memcmp(out, hk.want1, 32) == 0 && memcmp(out2, hk.want2, 32) == 0);

        memcpy(out, hk.ck, 32);
        noise_hkdf2(out, hk.material, hk.len, out, out2);
        check("hkdf in place", i,
              memcmp(out, hk.want1, 32) == 0 && memcmp(out2, hk.want2, 32) == 0);
    }
    uart_puts("hkdf done\r\n");

    for (uint8_t i = 0; i < COUNT(aead_vectors); i++) {
        LOAD(ae, aead_vectors, i);

        aead_seal(ae.key, ae.counter, ae.ad, ae.ad_len, ae.plain, ae.len, buf);
        check("aead seal", i, memcmp(buf, ae.sealed, ae.len + 16u) == 0);

        memcpy(buf, ae.sealed, ae.len + 16u);
        check("aead open", i,
              aead_open(ae.key, ae.counter, ae.ad, ae.ad_len, buf, ae.len, buf) &&
              memcmp(buf, ae.plain, ae.len) == 0);

        memcpy(buf, ae.sealed, ae.len + 16u);
        buf[ae.len + 5] ^= 0x01;
        check("aead bad tag", i,
              !aead_open(ae.key, ae.counter, ae.ad, ae.ad_len, buf, ae.len, buf));
        if (ae.len) {
            memcpy(buf, ae.sealed, ae.len + 16u);
            buf[ae.len / 2] ^= 0x80;
            check("aead bad ciphertext", i,
                  !aead_open(ae.key, ae.counter, ae.ad, ae.ad_len, buf, ae.len, buf));
        }
        memcpy(buf, ae.sealed, ae.len + 16u);
        check("aead wrong counter", i,
              !aead_open(ae.key, ae.counter + 1, ae.ad, ae.ad_len, buf, ae.len, buf));
    }
    uart_puts("aead done\r\n");
}

/* The PC runs more failure cases per vector; here the slow ones are trimmed
 * so the whole run stays within a few minutes. One dot per vector. */
static void test_curve(void)
{
    static struct decode_vector dec;
    static struct key_vector key;
    static struct ecdh_vector ecdh;
    static struct schnorr_vector sch, other;
    uint8_t out[32], ell[64], ell2[64], seed[32], bad[64];

    uart_puts("\r\n-- sv2curve vectors (slow; one dot each) --\r\ndecode ");
    for (uint8_t i = 0; i < COUNT(decode_vectors); i++) {
        LOAD(dec, decode_vectors, i);
        sv2_ellswift_decode(dec.ell, out);
        check("ellswift decode", i, memcmp(out, dec.x, 32) == 0);
        uart_putc('.');
    }

    uart_puts("\r\ncreate ");
    for (uint8_t i = 0; i < COUNT(key_vectors); i++) {
        LOAD(key, key_vectors, i);
        for (uint8_t round = 0; round < 2; round++) {
            memset(seed, (uint8_t)(i * 16 + round), sizeof seed);
            check("ellswift create", i, sv2_ellswift_create(key.priv, seed, ell));
            sv2_ellswift_decode(ell, out);
            check("ellswift create round trip", i, memcmp(out, key.x, 32) == 0);
            if (round == 0)
                memcpy(ell2, ell, 64);
            else
                check("ellswift encodings differ", i, memcmp(ell, ell2, 64) != 0);
        }
        uart_putc('.');
    }
    memset(seed, 0, sizeof seed);
    check("create refuses zero key", 0, !sv2_ellswift_create(seed, seed, ell));
    memset(bad, 0xff, 32);
    check("create refuses big key", 0, !sv2_ellswift_create(bad, seed, ell));

    uart_puts("\r\necdh ");
    for (uint8_t i = 0; i < COUNT(ecdh_vectors); i++) {
        LOAD(ecdh, ecdh_vectors, i);
        check("ecdh", i,
              sv2_ecdh(ecdh.priv, ecdh.theirs, ecdh.ours, ecdh.initiating, out) &&
              memcmp(out, ecdh.secret, 32) == 0);
        uart_putc('.');
    }

    uart_puts("\r\nschnorr ");
    for (uint8_t i = 0; i < COUNT(schnorr_vectors); i++) {
        LOAD(sch, schnorr_vectors, i);
        check("schnorr valid", i, sv2_schnorr_verify(sch.pubkey, sch.msg, sch.sig));

        if (i < 2) {                        /* the slow failure cases, twice only */
            memcpy(bad, sch.sig, 64);
            bad[3] ^= 0x10;
            check("schnorr bad r", i, !sv2_schnorr_verify(sch.pubkey, sch.msg, bad));
            memcpy(bad, sch.sig, 64);
            bad[60] ^= 0x01;
            check("schnorr bad s", i, !sv2_schnorr_verify(sch.pubkey, sch.msg, bad));
            memcpy(bad, sch.msg, 32);
            bad[0] ^= 0x80;
            check("schnorr bad msg", i, !sv2_schnorr_verify(sch.pubkey, bad, sch.sig));
            LOAD(other, schnorr_vectors, i + 1);
            check("schnorr wrong key", i, !sv2_schnorr_verify(other.pubkey, sch.msg, sch.sig));
        }

        /* Out-of-range values are refused before any slow arithmetic. */
        memcpy(bad, sch.sig, 64);
        memset(bad + 32, 0xff, 32);
        check("schnorr s too big", i, !sv2_schnorr_verify(sch.pubkey, sch.msg, bad));
        memcpy(bad, sch.sig, 64);
        memset(bad, 0xff, 32);
        check("schnorr r too big", i, !sv2_schnorr_verify(sch.pubkey, sch.msg, bad));
        check("schnorr key too big", i, !sv2_schnorr_verify(bad, sch.msg, sch.sig));
        uart_putc('.');
    }
    uart_puts("\r\n");
}

/* ---- crash diagnosis ----
 * A crash on a bare chip just restarts the program. These two variables live
 * in a part of RAM that start-up does not clear, so after a restart we can
 * still read which step was running, report it, and stop instead of looping.
 * The reset button (or a power cycle) starts a fresh run. */
#define STEP_MAGIC 0x5a3c
static uint16_t step_magic __attribute__((section(".noinit")));
static uint8_t  step_now   __attribute__((section(".noinit")));

static void step(uint8_t n, const char *what)
{
    step_now = n;
    step_magic = STEP_MAGIC;
    uart_puts("step ");
    uart_put_u32(n);
    uart_puts(": ");
    uart_puts(what);
    uart_puts("\r\n");
}

/* Steps 1-7: the same operations three times over: with no interrupts at
 * all, then with the millisecond timer running, then with the watchdog
 * randomness source running too. Where it dies says what is to blame. */
static void isolation_steps(void)
{
    static struct decode_vector dec;
    static struct key_vector key;
    static struct ecdh_vector ecdh;
    static struct schnorr_vector sch;
    uint8_t seed[32], ell[64], out[32];
    uint8_t ok;

    LOAD(dec, decode_vectors, 0);
    LOAD(key, key_vectors, 0);
    LOAD(ecdh, ecdh_vectors, 0);
    LOAD(sch, schnorr_vectors, 0);
    memset(seed, 0x42, sizeof seed);

    step(1, "hashes and encryption, interrupts off");
    test_crypto();

    step(2, "decode a key, interrupts off");
    sv2_ellswift_decode(dec.ell, out);
    check("decode", 0, memcmp(out, dec.x, 32) == 0);

    step(3, "make a key (curve multiplication), interrupts off");
    ok = sv2_ellswift_create(key.priv, seed, ell);
    sv2_ellswift_decode(ell, out);
    check("create", 0, ok && memcmp(out, key.x, 32) == 0);

    step(4, "key agreement, interrupts off");
    ok = sv2_ecdh(ecdh.priv, ecdh.theirs, ecdh.ours, ecdh.initiating, out);
    check("ecdh", 0, ok && memcmp(out, ecdh.secret, 32) == 0);

    step(5, "signature check, interrupts off");
    check("schnorr", 0, sv2_schnorr_verify(sch.pubkey, sch.msg, sch.sig));

    step(6, "make a key with the millisecond timer interrupt running");
    TCCR1A = 0;
    TCCR1B = _BV(WGM12) | _BV(CS11);        /* reset-on-match, clock / 8 */
    OCR1A  = 2499;                          /* 2500 counts = 1 ms */
    TIMSK1 = _BV(OCIE1A);
    sei();
    ok = sv2_ellswift_create(key.priv, seed, ell);
    sv2_ellswift_decode(ell, out);
    check("create with timer", 0, ok && memcmp(out, key.x, 32) == 0);

    step(7, "make a key with the watchdog randomness source running too");
    entropy_start();
    ok = sv2_ellswift_create(key.priv, seed, ell);
    sv2_ellswift_decode(ell, out);
    check("create with watchdog", 0, ok && memcmp(out, key.x, 32) == 0);
}

int main(void)
{
    uint32_t start;
    uint8_t cause = MCUSR;                  /* why the chip last reset */

    MCUSR = 0;
    wdt_disable();                          /* a watchdog reset leaves it running */
    stack_paint();

    DDRB |= _BV(DDB0);                      /* LED on PB0 */
    uart_init();

    uart_puts("\r\n\r\nsv2_chip_test: Stratum V2 handshake code on the ATmega1284P\r\n");
    uart_puts("reset cause:");
    if (cause & _BV(PORF))  uart_puts(" power-on");
    if (cause & _BV(EXTRF)) uart_puts(" reset-pin");
    if (cause & _BV(BORF))  uart_puts(" brown-out");
    if (cause & _BV(WDRF))  uart_puts(" WATCHDOG");
    if (cause & _BV(JTRF))  uart_puts(" jtag");
    if (!cause)             uart_puts(" NONE (the program jumped back to its start: a crash)");
    uart_puts("\r\n");

    if (step_magic == STEP_MAGIC && !(cause & (_BV(PORF) | _BV(EXTRF)))) {
        uart_puts("THE PREVIOUS RUN DIED DURING STEP ");
        uart_put_u32(step_now);
        uart_puts("\r\nStopped. Press the reset button for a fresh run.\r\n");
        step_magic = 0;
        for (;;) {
            PINB = _BV(PINB0);
            _delay_ms(100);
        }
    }

    isolation_steps();                      /* leaves interrupts and both sources on */

    step(8, "timings");
    time_handshake_steps();
    step(9, "randomness");
    try_randomness();

    step(10, "all vectors");
    start = millis();
    test_crypto();
    test_curve();
    report_ms("vectors took ", millis() - start);

    uart_puts("\r\nRAM never touched between variables and stack: ");
    uart_put_u32(stack_untouched());
    uart_puts(" bytes\r\n");

    if (failures) {
        uart_puts("RESULT: ");
        uart_put_u32(failures);
        uart_puts(" FAILED\r\n");
    } else {
        uart_puts("RESULT: all tests passed\r\n");
    }
    step_magic = 0;

    for (;;) {
        PINB = _BV(PINB0);                  /* slow blink = finished */
        if (failures)
            _delay_ms(100);                 /* fast blink = something failed */
        else
            _delay_ms(1000);
    }
}
