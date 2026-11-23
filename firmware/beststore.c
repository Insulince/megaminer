/*
 * beststore.c - keeps the best hash ever found in the chip's EEPROM, for
 * hashrate.c. It sits well away from the settings (which start at address 0
 * and take about 100 bytes): a marker, the 12 bytes, and a check byte.
 *
 * It is written only when a new record is found, which gets rarer as the
 * record improves, so wear on the EEPROM is no concern.
 */
#include <avr/eeprom.h>
#include <string.h>

#include "hashrate.h"

#define ADDRESS ((void *)0x200)
#define MARK_0  0xBE
#define MARK_1  0x57

struct stored {
    uint8_t mark[2];
    uint8_t best[HASHRATE_BEST_BYTES];
    uint8_t check;
};

static uint8_t check_of(const uint8_t *best)
{
    uint8_t sum = 0x5a, i;

    for (i = 0; i < HASHRATE_BEST_BYTES; i++)
        sum = (uint8_t)(sum * 31 + best[i]);
    return sum;
}

uint8_t platform_best_load(uint8_t out[HASHRATE_BEST_BYTES])
{
    struct stored st;

    eeprom_read_block(&st, ADDRESS, sizeof st);
    if (st.mark[0] != MARK_0 || st.mark[1] != MARK_1 || st.check != check_of(st.best))
        return 0;
    memcpy(out, st.best, HASHRATE_BEST_BYTES);
    return 1;
}

void platform_best_save(const uint8_t *best)
{
    struct stored st;

    memset(&st, 0xff, sizeof st);           /* all 0xff = erased = nothing stored */
    if (best) {
        st.mark[0] = MARK_0;
        st.mark[1] = MARK_1;
        memcpy(st.best, best, HASHRATE_BEST_BYTES);
        st.check = check_of(best);
    }
    eeprom_update_block(&st, ADDRESS, sizeof st);
}
