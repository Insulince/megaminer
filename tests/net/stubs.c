#include <stdint.h>
#include <stdio.h>
void (*uart_redirect)(char c);
uint8_t settings_request_update(void) { return 0; }
char *ultoa(unsigned long v, char *s, int base) { sprintf(s, base == 16 ? "%lx" : "%lu", v); return s; }
