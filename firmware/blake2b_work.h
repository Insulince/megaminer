/*
 * blake2b_work.h - the fast path for the mining loop.
 *
 * It computes the first 8 bytes of BLAKE2b-256 over an 80-byte work block
 * whose nonce (bytes 32..35, low byte first) is the only thing that changes
 * between calls. The digests are exactly BLAKE2b's; only the route is
 * arranged for this one shape of input. See gen_blake2b_avr.py for how, and
 * blake2b.c for the general algorithm (still used for everything else,
 * including the full digest of any hash that looks like a share).
 *
 *     bw_setup(work);              once per job
 *     bw_hash(nonce);              per attempt; the result is in bw_out
 *
 * On the chip bw_hash() runs generated assembly (blake2b_work_avr.S). Call
 * bw_selftest() once at start-up: it compares that against blake2b.c and,
 * if they ever disagree, switches bw_hash() to plain blake2b.c for good.
 * On the PC bw_hash() runs a C model of the same shortcut route.
 */
#ifndef BLAKE2B_WORK_H
#define BLAKE2B_WORK_H

#include <stdint.h>

extern uint8_t bw_out[8];           /* digest bytes 0..7 of the last bw_hash() */

/* Bytes 32..39 of `work` (the nonce field) are ignored: bw_hash() supplies
 * the low four and the high four are taken as zero. */
void bw_setup(const uint8_t work[80]);
void bw_hash(uint32_t nonce);

/* 1 = the fast path agrees with blake2b.c; 0 = it does not and has been
 * switched off (mining continues at the old speed). Disturbs the current
 * job: call bw_setup() again afterwards. */
uint8_t bw_selftest(void);
uint8_t bw_is_fast(void);

#endif
