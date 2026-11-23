/*
 * uptime.h - time since start-up, from Timer1.
 *
 * On the chip uptime.c provides all three (and, every half second, sets
 * board_beat_due for the heartbeat LED; see board.h). A PC stand-in provides
 * the two clock functions itself.
 */
#ifndef UPTIME_H
#define UPTIME_H

#include <stdint.h>

void     uptime_start(void);        /* the program must also call sei() */
uint32_t platform_seconds(void);    /* whole seconds since start-up */
uint32_t platform_millis(void);     /* milliseconds since start-up */

#endif
