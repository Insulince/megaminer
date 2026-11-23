/*
 * hashrate_test.c - PC test of hashrate.c with a pretend clock.
 *
 *   gcc -O2 -Wall -Wextra -o hashrate_test.exe hashrate_test.c hashrate.c && ./hashrate_test.exe
 *
 * A pretend chip hashes at exactly 186 H/s, in cycles of 100 s hashing and
 * 20 s doing something else, with a new job (an interruption of 30 ms)
 * every 20 s at an awkward moment. Expected: speed always 186.0 give or
 * take 0.2, and the averages settle at 186 * 100/120 = 155.0, each one
 * only once its window has passed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hashrate.h"
#include "uart.h"

static uint64_t now_us;

uint32_t platform_seconds(void) { return (uint32_t)(now_us / 1000000); }
uint32_t platform_millis(void)  { return (uint32_t)(now_us / 1000); }

void uart_putc(char c) { putchar(c); }
void uart_puts(const char *s) { fputs(s, stdout); }
void uart_put_u32(uint32_t n) { printf("%lu", (unsigned long)n); }
void uart_put_hex(const uint8_t *b, uint8_t len) { while (len--) printf("%02x", *b++); }

/* A pretend EEPROM for the best hash ever. */
static uint8_t stored[HASHRATE_BEST_BYTES];
static int have_stored, saves;

uint8_t platform_best_load(uint8_t out[HASHRATE_BEST_BYTES])
{
    if (have_stored)
        memcpy(out, stored, HASHRATE_BEST_BYTES);
    return (uint8_t)have_stored;
}

void platform_best_save(const uint8_t *best)
{
    saves++;
    have_stored = best != 0;
    if (best)
        memcpy(stored, best, HASHRATE_BEST_BYTES);
}

static int failures;

static void check_is_better(const uint8_t *digest, int want)
{
    if (hashrate_is_better(digest) != want) {
        printf("FAIL is_better\n");
        failures++;
    }
}

static void expect(const char *what, long got, long want, long slack)
{
    if (labs(got - want) > slack) {
        printf("FAIL %s: got %ld, want %ld +/- %ld\n", what, got, want, slack);
        failures++;
    }
}

int main(void)
{
    const uint64_t hash_us = 1000000 / 186;         /* 5376 us */
    const uint64_t day = 86400ULL * 1000000;
    uint64_t next_job = 20300000;
    long speeds = 0;
    int shown = 0;

    {   /* a record left by an earlier run: 30 zero bits */
        static const uint8_t old[12] = {0, 0, 0, 0x03, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

        platform_best_save(old);
        saves = 0;
        hashrate_start();
        expect("record loaded", hashrate_ever_bits(), 30, 0);
        expect("no luck figure before a best", (long)hashrate_luck_percent(), 0, 0);
    }

    while (now_us < 2 * day) {
        uint64_t in_cycle = now_us % 120000000;

        hashrate_tick();
        if (in_cycle >= 100000000) {                /* 20 s of not hashing, in one lump */
            hashrate_interrupted();
            now_us += 120000000 - in_cycle;
            continue;
        }
        if (now_us >= next_job) {                   /* a job arrives */
            hashrate_interrupted();
            now_us += 30000;
            next_job += 20300000;
        }
        now_us += hash_us;
        if (hashrate_hashed()) {
            speeds++;
            expect("speed", hashrate_speed(), 1860, 2);
            if (shown < 3 || now_us > 2 * day - 30000000) {
                hashrate_print();
                putchar('\n');
                shown++;
            }
        }
        /* Partly filled windows under-report, in proportion. */
        if (now_us > 3 * 3600ULL * 1000000 && now_us < 3 * 3600ULL * 1000000 + hash_us * 2) {
            expect("10m at 3 h", hashrate_average(0), 1550, 10);
            expect("1h at 3 h", hashrate_average(1), 1550, 10);
            expect("1d at 3 h", hashrate_average(2), 1550 / 8, 5);
        }
    }
    {   /* best share: a worse digest must not replace a better one */
        static const uint8_t d4096[12] = {0, 0, 0, 0, 0, 0x0f, 0xff, 0xff, 0xa1, 0xb2, 0xc3, 0xd4};
        static const uint8_t small[12] = {0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 1, 2, 3, 4};
        static const uint8_t worse[12] = {0, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        /* 2 days at 186 H/s, 100 s in every 120: about 26.8 million hashes,
         * so 24 zero bits are expected (2^24 = 16.8 million). */
        double hashes = (double)hashrate_total();

        expect("hashes (millions)", (long)(hashes / 1e6), 26, 1);
        expect("expected bits", hashrate_expected_bits(), 24, 0);
        check_is_better(small, 1);

        hashrate_set_target(d4096);     /* the pool target is difficulty 4096 */
        hashrate_set_nbits(0x1b0fffff); /* nbits = that same target: not the network's */
        hashrate_consider(small);       /* 24 zero bits, 2^20 times easier than the pool's 44 */
        expect("best bits", hashrate_best_bits(), 24, 0);
        /* a hash with 24 zero bits (just under 2^40) comes once in 2^24 hashes */
        expect("luck", (long)hashrate_luck_percent(), (long)(16777216.0 / hashes * 100 + 0.5), 1);
        expect("record untouched by a lesser hash", hashrate_ever_bits(), 30, 0);
        expect("and not written", saves, 0, 0);
        check_is_better(small, 0);
        check_is_better(worse, 0);
        check_is_better(d4096, 1);
        expect("pool times", (long)hashrate_pool_times(), 1048577, 1);
        expect("no network figure", (long)hashrate_net_difficulty(), 0, 0);
        hashrate_consider(worse);
        expect("best kept", hashrate_best_bits(), 24, 0);
        hashrate_print();
        putchar('\n');

        /* Seen from a real pool: 1900a738 = 0xa738 * 2^176, 64 zero
         * bits, difficulty 0xffff * 2^32 / 0xa738 = 6575188790 (the node agrees),
         * and (2^40 * 2^16) / 0xa738 = 1.68e12
         * times harder than a hash with 24 zero bits. */
        hashrate_set_nbits(0x1900a738);
        expect("net difficulty", (long)(hashrate_net_difficulty() / 1000), 6575188, 0);
        expect("net times (millions)", (long)(hashrate_net_times() / 1000000), 1683274, 1);
        hashrate_print();
        putchar('\n');

        hashrate_consider(d4096);
        expect("pool met", (long)hashrate_pool_times(), 0, 0);
        expect("new record", hashrate_ever_bits(), 44, 0);
        expect("written once", saves, 1, 0);
        expect("stored bytes", memcmp(stored, d4096, 12), 0, 0);
        hashrate_print();
        putchar('\n');

        expect("other lines are not ours", hashrate_command("show"), 0, 0);
        expect("resetbest is ours", hashrate_command("resetbest"), 1, 0);
        expect("record is now this run's best", hashrate_ever_bits(), 44, 0);
    }
    expect("10m", hashrate_average(0), 1550, 10);
    expect("1h", hashrate_average(1), 1550, 10);
    expect("1d", hashrate_average(2), 1550, 10);

    {   /* resetstats: averages start over; count and best hashes stay */
        uint64_t before = hashrate_total();
        long best_bits = hashrate_best_bits(), ever_bits = hashrate_ever_bits();
        uint64_t end;

        expect("resetstats is ours", hashrate_command("resetstats"), 1, 0);
        expect("averages zero after resetstats",
               hashrate_average(0) + hashrate_average(1) + hashrate_average(2), 0, 0);
        expect("hash count kept", hashrate_total() == before, 1, 0);
        expect("best kept by resetstats", hashrate_best_bits(), best_bits, 0);
        expect("record kept by resetstats", hashrate_ever_bits(), ever_bits, 0);
        /* 11 minutes of steady hashing: the 10 min average is full again,
         * the 1 h one 11/60 full, the 1 day one not sampled yet. */
        for (end = now_us + 660ULL * 1000000; now_us < end; now_us += hash_us) {
            hashrate_tick();
            hashrate_hashed();
        }
        hashrate_tick();
        expect("10m refilled after resetstats", hashrate_average(0), 1860, 10);
        expect("1h partly refilled after resetstats", hashrate_average(1), 1860 * 11 / 60, 35);
        expect("1d still empty after resetstats", hashrate_average(2), 0, 0);
    }
    printf("%ld speed figures checked\n", speeds);
    printf(failures ? "RESULT: FAILED\n" : "RESULT: all tests passed\n");
    return failures != 0;
}
