#include <stdint.h>
#include <stdio.h>
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
uint8_t platform_best_load(uint8_t out[HASHRATE_BEST_BYTES]) { (void)out; return 0; }
void platform_best_save(const uint8_t *best) { (void)best; }
static uint32_t rng = 12345;
static uint8_t rnd(void) { rng = rng * 1103515245u + 12345u; return (uint8_t)(rng >> 16); }
int main(int argc, char **argv)
{
    int n = argc > 1 ? 200000 : 0;
    hashrate_start();
    printf("{\"empty\":0");  hashrate_json(); printf("}\n");
    uint8_t t[32] = {0,0,0,0,0,0x0f,0xff,0xff}; memset(t + 8, 0xff, 24);
    hashrate_set_target(t); hashrate_set_nbits(0x1900a738);
    for (int i = 0; i < n; i++) {
        uint8_t d[12]; for (int k = 0; k < 12; k++) d[k] = rnd();
        now_us += 1020;
        hashrate_hashed(); hashrate_tick();
        uint8_t bits = 0, j = 0; while (j < 8 && d[j] == 0) { bits += 8; j++; }
        if (j < 8) { uint8_t b = d[j]; while (!(b & 0x80)) { b <<= 1; bits++; } } else bits = 64;
        hashrate_count_bits(bits);
        if (hashrate_is_better(d)) hashrate_consider(d);
    }
    printf("{\"full\":1"); hashrate_json(); printf("}\n");
    return 0;
}
