#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

// TODO: generador de sonido y música por PWM (salida en GP4)

void sound_init(void);
void sound_play_tone(uint16_t frequency_hz, uint16_t duration_ms);

#endif
