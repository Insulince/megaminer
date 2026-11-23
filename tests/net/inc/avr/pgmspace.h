#pragma once
#include <string.h>
#include <strings.h>
#include <stdint.h>
extern uint8_t *fake_flash;
#define PSTR(s) (s)
#define PROGMEM
#define strncasecmp_P strncasecmp
#define strlen_P strlen
#define strcpy_P strcpy
#define memcpy_P memcpy
#define strncpy_P strncpy
#define pgm_read_byte_far(a) (fake_flash[(a)])
#define pgm_read_word_far(a) ((uint16_t)(fake_flash[(a)] | fake_flash[(a) + 1] << 8))
#define pgm_read_byte(p) (*(const uint8_t *)(p))
#define strcmp_P strcmp
#define strncmp_P strncmp
#define memcmp_P memcmp
#define strstr_P strstr
