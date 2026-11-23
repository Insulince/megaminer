#pragma once
#include <stdint.h>
void fake_erase(uint32_t a); void fake_fill(uint32_t a, uint16_t w); void fake_write(uint32_t a);
#define boot_page_erase_safe(a) fake_erase(a)
#define boot_page_fill_safe(a, w) fake_fill((a), (w))
#define boot_page_write_safe(a) fake_write(a)
#define boot_spm_busy_wait() (void)0
#define boot_rww_enable_safe() (void)0
