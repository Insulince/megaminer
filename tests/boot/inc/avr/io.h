#pragma once
#include <stdint.h>
extern uint8_t PORTA, DDRA, PORTB, DDRB, PINB, PORTD, PIND, GPIOR0, GPIOR1, MCUSR;
#define _BV(b) (1u << (b))
#define PA0 0
#define PA1 1
#define PA2 2
#define PA3 3
#define PA4 4
#define PB0 0
#define PD2 2
#define SPM_PAGESIZE 256
