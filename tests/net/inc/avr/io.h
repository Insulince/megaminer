#pragma once
#include <stdint.h>
extern uint8_t PORTA, DDRA, PORTB, DDRB, PINB, PORTD, PIND;
extern uint16_t SP;
#define _BV(b) (1u << (b))
