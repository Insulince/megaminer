/*
 * hashrate.c - see hashrate.h.
 */
#include "hashrate.h"

#include <string.h>

#include "uart.h"

#define SPEED_WINDOW_SECONDS 5

static uint64_t total;                  /* hashes since start-up */

/* ---- speed while hashing ---- */

static uint8_t  restart = 1;            /* start a new window at the next hash */
static uint32_t window_hashes;
static uint32_t window_began_s;
static uint32_t window_began_ms;
static uint16_t speed;

void hashrate_interrupted(void)
{
    restart = 1;
}

uint8_t hashrate_hashed(void)
{
    uint32_t now_ms, elapsed;

    total++;
    if (restart) {
        /* The hash just finished began before this window, so it is not
         * counted in it. */
        restart = 0;
        window_hashes = 0;
        window_began_s = platform_seconds();
        window_began_ms = platform_millis();
        return 0;
    }
    window_hashes++;
    /* The cheap whole-second clock decides when to look at the fine one. */
    if (platform_seconds() - window_began_s < SPEED_WINDOW_SECONDS)
        return 0;

    now_ms = platform_millis();
    elapsed = now_ms - window_began_ms;
    if (elapsed)
        speed = (uint16_t)(window_hashes * 10000UL / elapsed);
    window_hashes = 0;
    window_began_s = platform_seconds();
    window_began_ms = now_ms;
    return 1;
}

/* ---- averages over wall-clock time ---- */

/* Each ring holds the running total as it stood at regular moments. When a
 * sample is due, the slot about to be overwritten is the total from exactly
 * one whole window ago, so (total - slot) is the hashes done in the window.
 * The rings start as zeros, the total at start-up, which is what makes a
 * window that has not yet filled under-report instead of misbehave. */
#define SLOTS_10M 60    /* every 10 s  */
#define SLOTS_1H  60    /* every 60 s  */
#define SLOTS_1D  96    /* every 900 s */

static uint32_t ring_10m[SLOTS_10M], ring_1h[SLOTS_1H], ring_1d[SLOTS_1D];

struct average {
    uint32_t *ring;
    uint8_t   slots;
    uint16_t  period;       /* seconds between samples */
    uint8_t   next_slot;
    uint32_t  due;          /* seconds clock value of the next sample */
    uint16_t  value;        /* tenths of a hash per second */
};

static struct average averages[3] = {
    { ring_10m, SLOTS_10M, 10,  0, 10,  0 },
    { ring_1h,  SLOTS_1H,  60,  0, 60,  0 },
    { ring_1d,  SLOTS_1D,  900, 0, 900, 0 },
};

void hashrate_tick(void)
{
    uint32_t now = platform_seconds();
    uint8_t i;

    for (i = 0; i < 3; i++) {
        struct average *a = &averages[i];

        while ((int32_t)(now - a->due) >= 0) {
            uint32_t window_seconds = (uint32_t)a->slots * a->period;
            uint32_t done = (uint32_t)total - a->ring[a->next_slot];

            /* done * 10 fits: a day at 10,000 H/s is under 2^32 / 10. */
            a->value = (uint16_t)(done * 10 / window_seconds);
            a->ring[a->next_slot] = (uint32_t)total;
            if (++a->next_slot == a->slots)
                a->next_slot = 0;
            a->due += a->period;
        }
    }
}

/* ---- the best hash so far, against the pool and the network ---- */

#define NONE 0xffffffffffffffffULL

#define BEST HASHRATE_BEST_BYTES

static uint64_t best_top = NONE;        /* first 8 bytes of the best digest, as a number */
static uint8_t  best[BEST];             /* its first 12 bytes, for printing */
static uint64_t ever_top = NONE;        /* the same for the best ever (kept in EEPROM) */
static uint8_t  ever[BEST];
static uint64_t target_top;             /* first 8 bytes of the share target; 0 = none yet */
static uint32_t nbits;                  /* 0 = unknown */

/* Worked out again only when one of the three above changes: 64-bit
 * division is slow on the chip and the report line is printed often. */
static uint8_t  best_bits, pool_bits, net_bits;
static uint32_t pool_difficulty;
static uint64_t pool_times;             /* 0 = met */
static uint64_t net_difficulty;         /* 0 = not known */
static uint64_t net_times;

/* A "times harder" figure rounded to three digits for printing:
 * digits * 10^tens, e.g. 356 and 8 for 3.56e10. */
struct rounded {
    uint16_t digits;
    uint8_t  tens;
};
static struct rounded pool_rounded, net_rounded;

static struct rounded round_off(uint64_t v)
{
    struct rounded r;

    r.tens = 0;
    while (v >= 10000) {
        v /= 10;
        r.tens++;
    }
    if (v >= 1000) {                    /* the last digit to go is rounded */
        v = (v + 5) / 10;
        r.tens++;
        if (v == 1000) {
            v = 100;
            r.tens++;
        }
    }
    r.digits = (uint16_t)v;
    return r;
}

static uint8_t zero_bits(uint64_t v)
{
    uint8_t n = 0;

    while (n < 64 && !(v >> 63)) {
        v <<= 1;
        n++;
    }
    return n;
}

static uint64_t shifted(uint64_t v, int16_t left)
{
    if (left <= 0)
        return left < -63 ? 0 : v >> -left;
    if (left > 63 || (v >> (64 - left)))
        return NONE;                    /* too big to hold */
    return v << left;
}

static void refresh(void)
{
    uint8_t size = (uint8_t)(nbits >> 24);          /* the target is this many bytes long */
    uint32_t mantissa = nbits & 0xffffffUL;         /* and these are its top three */

    best_bits = zero_bits(best_top);

    /* A digest or target is roughly (its first 64 bits) * 2^192, so the
     * ratio of two of them is the ratio of their first 64 bits. Difficulty
     * is (0xffff * 2^208) / target, Bitcoin's definition (the node reports
     * the same number), so (0xffff * 2^16) / (first 64 bits); rounded to
     * nearest because pools send a target a hair above the exact one. */
    if (target_top) {
        pool_bits = zero_bits(target_top);
        pool_difficulty = (uint32_t)((0xffff0000ULL + target_top / 2) / target_top);
        pool_times = best_top <= target_top ? 0 : best_top / target_top;
        pool_rounded = round_off(pool_times);
    }

    /* The network target is mantissa * 2^(8 * (size - 3)); its first 64
     * bits alone would be too coarse (often zero), so use the mantissa. */
    net_difficulty = 0;
    if (mantissa && size >= 3 && size <= 32) {
        int16_t exponent = (int16_t)(8 * (size - 3));       /* target = mantissa * 2^exponent */
        uint64_t difficulty = shifted(0xffff, 208 - exponent) == NONE
                                  ? NONE : shifted(0xffff, 208 - exponent) / mantissa;

        if (difficulty != NONE && difficulty > pool_difficulty) {
            uint8_t mantissa_zeros = (uint8_t)(zero_bits(mantissa) - 40);   /* within its 24 bits */

            net_difficulty = difficulty;
            net_bits = (uint8_t)(256 - 8 * size + mantissa_zeros);
            /* best / target = (best_top * 2^192) / (mantissa * 2^exponent) */
            net_times = shifted(best_top / mantissa, 192 - exponent);
            net_rounded = round_off(net_times);
        }
    }
}

static uint64_t top_of(const uint8_t *digest)
{
    uint64_t top = 0;
    uint8_t i;

    for (i = 0; i < 8; i++)
        top = (top << 8) | digest[i];
    return top;
}

void hashrate_start(void)
{
    if (platform_best_load(ever))
        ever_top = top_of(ever);
}

uint8_t hashrate_is_better(const uint8_t *first8)
{
    return top_of(first8) < best_top;
}

void hashrate_consider(const uint8_t *digest)
{
    uint64_t top = top_of(digest);

    if (top >= best_top)
        return;
    best_top = top;
    memcpy(best, digest, BEST);
    if (top < ever_top) {
        ever_top = top;
        memcpy(ever, digest, BEST);
        platform_best_save(ever);       /* rare: each new record is harder to beat */
    }
    refresh();
}

/* "resetstats": the averages start over from now, as at start-up (each reads
 * low until its window has filled again). The hash count, the best hashes
 * and the uptime are left alone. */
static void reset_averages(void)
{
    uint32_t now = platform_seconds();

    for (uint8_t i = 0; i < 3; i++) {
        struct average *a = &averages[i];
        for (uint8_t s = 0; s < a->slots; s++)
            a->ring[s] = (uint32_t)total;
        a->next_slot = 0;
        a->due = now + a->period;
        a->value = 0;
    }
    restart = 1;                        /* and the "rate" window */
}

uint8_t hashrate_command(const char *line)
{
    if (strcmp(line, "resetstats") == 0) {
        reset_averages();
        uart_puts_P(PSTR("cmd: resetstats\r\naverages: reset; they read low until their windows fill "
                  "(10 min, 1 h, 1 day). Hash count and best hashes kept.\r\n"));
        return 1;
    }
    if (strcmp(line, "resetbest") != 0)
        return 0;
    /* The best of this run is still a fact, so it becomes the new record. */
    ever_top = best_top;
    memcpy(ever, best, BEST);
    platform_best_save(best_top == NONE ? 0 : ever);
    uart_puts_P(PSTR("cmd: resetbest\r\nbest ever: forgotten; starting again from this run's best\r\n"));
    return 1;
}

void hashrate_set_target(const uint8_t *target)
{
    uint64_t top = top_of(target);

    if (top == target_top)
        return;
    target_top = top;
    refresh();
}

void hashrate_set_nbits(uint32_t bits)
{
    if (bits == nbits)
        return;
    nbits = bits;
    refresh();
}

/* ---- leading zero bits of every hash (the web page's luck chart) ---- */

/* hist[k] = hashes with exactly k leading zero bits (the last bin: that many
 * or more). hist[0] is not kept: it is the total minus all the others. Each
 * bin should come out about half the one before it. */
static uint32_t hist[HASHRATE_HIST_BINS];

void hashrate_count_bits(uint8_t bits)
{
    if (!bits)
        return;
    if (bits >= HASHRATE_HIST_BINS)
        bits = HASHRATE_HIST_BINS - 1;
    hist[bits]++;
}

/* ---- reading and printing ---- */

uint16_t hashrate_speed(void)              { return speed; }
uint16_t hashrate_average(uint8_t which)   { return averages[which].value; }
uint64_t hashrate_total(void)              { return total; }
uint8_t  hashrate_ever_bits(void)          { return zero_bits(ever_top); }
uint8_t  hashrate_best_bits(void)          { return best_bits; }
uint64_t hashrate_pool_times(void)         { return pool_times; }
uint64_t hashrate_net_difficulty(void)     { return net_difficulty; }
uint64_t hashrate_net_times(void)          { return net_times; }

/* A hash as good as the best one turns up once in (2^64 / best_top) hashes
 * on average. Luck is that figure over the hashes actually done: 100% means
 * the best hash is exactly as good as the work done would lead one to
 * expect; 400% means it would normally have taken four times as long. */
uint32_t hashrate_luck_percent(void)
{
    float luck;

    if (best_top == NONE || total == 0)
        return 0;
    luck = 1.8446744e19f / (float)(best_top ? best_top : 1) / (float)total * 100.0f;
    return luck >= 999999999.0f ? 999999999UL : (uint32_t)(luck + 0.5f);
}

/* The zero bits a hash at exactly 100% luck would have: one hash in N has
 * about log2(N) of them. */
uint8_t hashrate_expected_bits(void)
{
    uint64_t n = total;
    uint8_t bits = 0;

    while (n >>= 1)
        bits++;
    return bits;
}

static void put_tenths(uint16_t v)
{
    uart_put_u32(v / 10);
    uart_putc('.');
    uart_putc((char)('0' + v % 10));
}

/* uart.h only prints 32-bit numbers. Split at 10^9: one slow division. */
static void put_u64(uint64_t v)
{
    uint32_t low, place;

    if (v <= 0xffffffffUL) {
        uart_put_u32((uint32_t)v);
        return;
    }
    low = (uint32_t)(v % 1000000000UL);
    put_u64(v / 1000000000UL);
    for (place = 100000000UL; place; place /= 10)
        uart_putc((char)('0' + low / place % 10));
}

/* " (44 zero bits, 3.56e10x harder)"; small figures are printed whole. */
static void put_comparison(uint8_t bits, uint64_t times, struct rounded r)
{
    uart_puts_P(PSTR(" ("));
    uart_put_u32(bits);
    uart_puts_P(PSTR(" zero bits, "));
    if (times == 0) {
        uart_puts_P(PSTR("met)"));
        return;
    }
    if (times < 1000) {
        uart_put_u32((uint32_t)times);
    } else {
        uart_putc((char)('0' + r.digits / 100));
        uart_putc('.');
        uart_putc((char)('0' + r.digits / 10 % 10));
        uart_putc((char)('0' + r.digits % 10));
        uart_putc('e');
        uart_put_u32((uint32_t)r.tens + 2);
    }
    uart_puts_P(PSTR("x harder)"));
}

void hashrate_print(void)
{
    uint32_t minutes = platform_seconds() / 60;

    uart_puts_P(PSTR("rate "));
    put_tenths(speed);
    uart_puts_P(PSTR(" | avg 10m "));
    put_tenths(averages[0].value);
    uart_puts_P(PSTR(", 1h "));
    put_tenths(averages[1].value);
    uart_puts_P(PSTR(", 1d "));
    put_tenths(averages[2].value);

    uart_puts_P(PSTR(" | best "));
    if (best_top == NONE) {
        uart_putc('-');
    } else {
        uart_put_hex(best, BEST);
        uart_puts_P(PSTR(" ("));
        uart_put_u32(best_bits);
        uart_puts_P(PSTR(" zero bits, "));
        uart_put_u32(hashrate_expected_bits());
        uart_puts_P(PSTR(" expected, luck "));
        uart_put_u32(hashrate_luck_percent());
        uart_puts_P(PSTR("%)"));
    }
    if (ever_top != NONE) {
        uart_puts_P(PSTR(" | ever "));
        uart_put_hex(ever, BEST);
        uart_puts_P(PSTR(" ("));
        uart_put_u32(zero_bits(ever_top));
        uart_puts_P(PSTR(" zero bits)"));
    }
    if (best_top != NONE) {
        if (target_top) {
            uart_puts_P(PSTR(" | pool "));
            uart_put_u32(pool_difficulty);
            put_comparison(pool_bits, pool_times, pool_rounded);
        }
        if (net_difficulty) {
            uart_puts_P(PSTR(" | net "));
            put_u64(net_difficulty);
            put_comparison(net_bits, net_times, net_rounded);
        }
    }

    uart_puts_P(PSTR(" | up "));
    uart_put_u32(minutes / 60);
    uart_putc('h');
    uart_putc((char)('0' + minutes % 60 / 10));
    uart_putc((char)('0' + minutes % 60 % 10));
    uart_puts_P(PSTR("m | hashes "));
    put_u64(total);
}

static void json_tenths(const char *name, uint16_t v)
{
    uart_puts_P(name);
    put_tenths(v);
}

static void json_u64(const char *name, uint64_t v)
{
    uart_puts_P(name);
    put_u64(v);
}

void hashrate_json(void)
{
    uint8_t last, i;
    uint64_t rest = total;

    json_tenths(PSTR(",\"rate\":"), speed);
    json_tenths(PSTR(",\"avg\":["), averages[0].value);
    json_tenths(PSTR(","), averages[1].value);
    json_tenths(PSTR(","), averages[2].value);
    json_u64(PSTR("],\"hashes\":"), total);
    json_u64(PSTR(",\"up\":"), platform_seconds());
    json_u64(PSTR(",\"expectedBits\":"), hashrate_expected_bits());
    if (best_top != NONE) {
        uart_puts_P(PSTR(",\"best\":\""));
        uart_put_hex(best, BEST);
        json_u64(PSTR("\",\"bestBits\":"), best_bits);
        json_u64(PSTR(",\"luck\":"), hashrate_luck_percent());
    }
    if (ever_top != NONE) {
        uart_puts_P(PSTR(",\"ever\":\""));
        uart_put_hex(ever, BEST);
        json_u64(PSTR("\",\"everBits\":"), zero_bits(ever_top));
    }
    if (target_top) {
        json_u64(PSTR(",\"poolDiff\":"), pool_difficulty);
        json_u64(PSTR(",\"poolBits\":"), pool_bits);
        if (best_top != NONE)
            json_u64(PSTR(",\"poolTimes\":"), pool_times);
    }
    if (net_difficulty) {
        json_u64(PSTR(",\"netDiff\":"), net_difficulty);
        json_u64(PSTR(",\"netBits\":"), net_bits);
        if (best_top != NONE)
            json_u64(PSTR(",\"netTimes\":"), net_times);
    }
    /* hist: bin 0 worked out, trailing empty bins left off */
    for (i = 1; i < HASHRATE_HIST_BINS; i++)
        rest -= hist[i];
    for (last = HASHRATE_HIST_BINS - 1; last > 0 && !hist[last]; last--) {
    }
    json_u64(PSTR(",\"hist\":["), rest);
    for (i = 1; i <= last; i++)
        json_u64(PSTR(","), hist[i]);
    uart_putc(']');
}
