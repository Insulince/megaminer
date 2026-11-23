/*
 * miner.h - everything a megaminer program does that is not the pool's
 * protocol. miner.c owns main(): start-up (lamp test, button menu, WiFi
 * setup, network), the console, the clock, the lights, the watchdog and
 * its stuck rule, the web page's /status, and the hashing loop.
 *
 * A protocol program (miner_sv2.c, miner_sv1.c) is only the pool side:
 * it fills in `job`, calls miner_job() when a job arrives, and supplies
 * the pool_* functions below, which miner.c calls.
 *
 *   miner.c                         miner_sv2.c / miner_sv1.c
 *   -------                         -------------------------
 *   main(): start-up      -------->  pool_start()
 *   every ~65 ms          -------->  pool_service()     connect, read, reply
 *   a console line        -------->  pool_command()     e.g. sv2.authority_key=
 *   settings saved        -------->  pool_reset()       start the connection afresh
 *   a share found         -------->  pool_submit()      job.work holds the nonce
 *                         <--------  miner_job()        a new job is in `job`
 *                         <--------  miner_stop()       not hashing any more
 *                         <--------  miner_failed()     an attempt failed
 *                         <--------  miner_needs_you()  waiting on a person
 *
 * The traffic light follows from those: green = hashing; yellow = not
 * hashing but on the way; red = it needs you (no WiFi link, a setting only
 * a person can give, or MINER_FAILURES_RED failed attempts in a row).
 */
#ifndef MINER_H
#define MINER_H

#include <stdint.h>

#define MINER_WORK_BYTES    80
#define MINER_DIGEST_BYTES  32
#define MINER_NONCE_OFFSET  32      /* 8 nonce bytes at 32-39; we count in the first 4 */
#define MINER_NTIME_OFFSET  40
#define MINER_ROOT_OFFSET   48
#define MINER_FAILURES_RED  3
#define MINER_RETRY_SECONDS 10      /* pause after a failure that waiting may cure */

/* The job being hashed. The protocol fills work (nonce bytes zero) and
 * target (most significant byte first, like the digest), then calls
 * miner_job(). */
struct miner_job {
    uint8_t  work[MINER_WORK_BYTES];
    uint8_t  target[MINER_DIGEST_BYTES];
    uint32_t nbits;                 /* the job's compact target field, for the report */
    uint32_t nonce;                 /* the one being tried now */
};
extern struct miner_job job;

/* ---- for the protocol to call ---- */

/* A job is in `job`. new_job = 1 starts its nonces from 0 (a new job), 0
 * keeps counting (the same job, e.g. only the target changed). */
void miner_job(uint8_t new_job);
void miner_stop(void);              /* stop hashing (connection lost, ...) */
void miner_failed(void);            /* an attempt failed: counts towards red */
void miner_needs_you(uint8_t yes);  /* 1 = red: waiting on a person; cleared by a job */
uint8_t miner_hashing(void);

/* Call from inside long calculations (curve maths, waiting for randomness)
 * so the console keeps answering. A "save" typed then is applied when the
 * calculation is over. */
void miner_while_busy(void);

/* The full 32-byte BLAKE2b digest. */
void miner_hash(const uint8_t *in, uint8_t len, uint8_t *digest);

/* ---- the clock (SNTP, RFC 4330) ---- */
uint8_t  clock_fetch(void);         /* ask the time server; 1 = got an answer */
uint8_t  clock_known(void);         /* a reading has been taken */
uint32_t clock_age(void);           /* seconds since that reading */
uint32_t clock_now(void);           /* seconds since 1970 */
void     clock_forget(void);        /* the reading may be wrong: take a new one */

/* ---- console helpers: text in flash, PSTR("...") ---- */
void put_hex_line(const char *label, const uint8_t *bytes, uint8_t len);

/* ---- supplied by the protocol program ---- */
void    pool_start(void);
void    pool_service(void);
uint8_t pool_command(const char *line);     /* 1 = it was one of the protocol's */
void    pool_reset(uint8_t hang_up);        /* settings changed: start afresh; hang_up = 0 when
                                             * a network restart already dropped the connection */
void    pool_submit(void);

#endif
