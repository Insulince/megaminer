#pragma once
void fake_restart(void);
#define WDTO_15MS 0
#define wdt_enable(x) fake_restart()
#define wdt_disable() (void)0
