#pragma once
/* the test watches the lights through this */
void fake_delay(double ms);
#define _delay_ms(x) fake_delay(x)
