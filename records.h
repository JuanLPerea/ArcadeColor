#ifndef RECORDS_H
#define RECORDS_H

#include <stdint.h>

// TODO: sistema de guardado de récords, uno independiente por juego
// (previsiblemente en memoria flash, ya que no hay tarjeta SD)

void records_init(void);
uint32_t records_get_high_score(const char *game_name);
void records_set_high_score(const char *game_name, uint32_t score);

#endif
