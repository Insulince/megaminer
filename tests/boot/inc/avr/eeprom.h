#pragma once
#include <stdint.h>
#include <string.h>
extern uint8_t fake_ee[4096];
#define eeprom_read_byte(p) (fake_ee[(uintptr_t)(p)])
#define eeprom_update_byte(p, v) (fake_ee[(uintptr_t)(p)] = (v))
#define eeprom_read_block(dst, p, n) memcpy((dst), fake_ee + (uintptr_t)(p), (n))
