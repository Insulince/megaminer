/*
 * hashrate.h - two kinds of hash rate.
 *
 *   speed     how fast the chip hashes WHILE it is hashing, measured over
 *             windows of about 5 seconds timed to the millisecond. A window
 *             is thrown away and restarted whenever hashing is interrupted
 *             (new job, reconnect), so the figure never includes waiting.
 *
 *   averages  hashes actually done over the last 10 minutes, 1 hour and
 *             1 day of wall-clock time, divided by that whole time. These
 *             include every second spent NOT hashing (key making, handshake,
 *             waiting for the server). Until a window's time has passed
 *             since start-up its figure under-reports, because it is always
 *             divided by the full window.
 *
 * All figures are in tenths of a hash per second (1862 = 186.2 H/s).
 *
 *   best      the hardest hash found since start-up, shown as its first 12
 *             bytes (enough to still show digits for a hash good enough to
 *             be a block: the network asks for about 64 zero bits = 8 zero
 *             bytes) and its count of leading zero bits. With it: the zero
 *             bits one would expect after this many hashes, and "luck", how
 *             many times more hashes a hash this good normally takes than
 *             were done (100% = exactly as expected; the typical value
 *             wanders around 100-150% and drops slowly between new bests).
 *
 *   ever      the hardest hash ever found, kept in EEPROM across resets and
 *             power-offs (and across reflashes once the EESAVE fuse is set).
 *             The console command "resetbest" forgets it.
 *
 *             Zero bits are a property
 *             of the hash alone, so it stays comparable when the pool (and
 *             its target) changes. Next to it: the CURRENT pool's share
 *             difficulty, the zero bits that needs, and how many times
 *             harder that is than the best hash; and the same for the
 *             network, when the pool tells us the network's difficulty.
 *             Difficulty is Bitcoin's: about 1 at 32 zero bits, each
 *             further zero bit doubles it (00000000000fffff... is 4096).
 *
 * The clock comes from uptime.h.
 */
#ifndef HASHRATE_H
#define HASHRATE_H

#include <stdint.h>

#include "uptime.h"                 /* platform_seconds(), platform_millis() */

/* Call after every hash. Returns 1 when a new speed figure is ready
 * (about every 5 seconds of uninterrupted hashing). */
uint8_t hashrate_hashed(void);

/* Call whenever hashing was interrupted: the current speed window is
 * dropped and a new one starts with the next hash. */
void hashrate_interrupted(void);

/* Call often, hashing or not (it only reads the seconds clock). Takes the
 * samples behind the averages. It may be called late, e.g. after a long
 * calculation, provided no hashes were done in between. */
void hashrate_tick(void);

#define HASHRATE_BEST_BYTES 12

/* Call once at start-up: fetches the best hash ever from storage. */
void hashrate_start(void);

/* Would a digest starting with these 8 bytes be this run's best so far?
 * (The fast hashing routine only gives 8 bytes; when this says yes, the
 * caller works out the whole digest and hands it to hashrate_consider.) */
uint8_t hashrate_is_better(const uint8_t *first8);

/* Offer a digest as the best so far. Reads its first 12 bytes. */
void hashrate_consider(const uint8_t *digest);

/* Console line "resetbest" (forget the best ever) or "resetstats" (restart
 * the 10 min / 1 h / 1 day averages and the rate window from now; hash
 * count, best hashes and uptime are kept)? Then deal with it and return 1;
 * otherwise 0. */
uint8_t hashrate_command(const char *line);

/* Storage for the best hash ever; the program provides these two
 * (beststore.c on the chip). Load returns 1 if something was stored. Save
 * with a null pointer erases. */
uint8_t platform_best_load(uint8_t out[HASHRATE_BEST_BYTES]);
void    platform_best_save(const uint8_t *best);

/* Tell it the share target now in force (32 bytes, most significant first,
 * like the digest; the first 8 are read). Only used for printing. */
void hashrate_set_target(const uint8_t *target);

/* Tell it the job's compact "nbits" (first byte = length in bytes, then a
 * 3-byte mantissa). It is shown as the network's difficulty only when it is
 * harder than the share target: some pools (the DATUM gateway) put their
 * share target in this field instead. 0 = unknown. */
void hashrate_set_nbits(uint32_t nbits);

uint16_t hashrate_speed(void);              /* last speed figure, tenths */
uint16_t hashrate_average(uint8_t which);   /* 0 = 10 min, 1 = 1 h, 2 = 1 day; tenths */
uint64_t hashrate_total(void);              /* hashes since start-up */
uint8_t  hashrate_ever_bits(void);          /* leading zero bits of the best hash ever */
uint32_t hashrate_luck_percent(void);       /* see above; 0 = no best yet */
uint8_t  hashrate_expected_bits(void);
uint8_t  hashrate_best_bits(void);          /* leading zero bits of the best hash */
uint64_t hashrate_pool_times(void);         /* how many times harder the pool target is (0 = met) */
uint64_t hashrate_net_difficulty(void);     /* 0 = not known */
uint64_t hashrate_net_times(void);

/* Prints, with no line ending, using uart.h:
 *   rate 983.5 | avg 10m 978.7, 1h 978.9, 1d 39.6 | best
 *   000000521a36ef07c41d9a02 (25 zero bits, 21 expected, luck 1264%) | ever
 *   0000000a1b2c3d4e5f607182 (28 zero bits) | pool 4096 (44 zero bits,
 *   3.36e5x harder) | net 6575188790 (64 zero bits, 5.40e11x harder) | up
 *   1h12m | hashes N
 * "best -" until a digest has been considered; "met" in place of
 * "Nx harder" if the best hash would have satisfied that target. The pool
 * and network comparisons are against this run's best. */
void hashrate_print(void);

/* Leading zero bits of each hash, for a histogram (the web page's luck
 * chart). Call with the count for every hash (0-64; calls with 0 do nothing,
 * those hashes are worked out from the total). The last bin collects that
 * many bits or more. */
#define HASHRATE_HIST_BINS 41
void hashrate_count_bits(uint8_t bits);

/* The same figures as hashrate_print, as JSON members each starting with a
 * comma (the caller writes the braces), using uart.h for output:
 *   ,"rate":983.5,"avg":[978.7,978.9,39.6],"hashes":N,"up":S,
 *   "expectedBits":21,"best":"hex","bestBits":25,"luck":1264,"ever":"hex",
 *   "everBits":28,"poolDiff":4096,"poolBits":44,"poolTimes":N,"netDiff":N,
 *   "netBits":64,"netTimes":N,"hist":[n0,n1,...]
 * best/ever/pool/net members appear only when known; poolTimes 0 = met. */
void hashrate_json(void);

#endif
