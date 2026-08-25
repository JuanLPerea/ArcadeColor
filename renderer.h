#ifndef RENDERER_H
#define RENDERER_H

#include "st7789.h"

// Capa fina sobre el driver st7789: los juegos y el menú dibujan a
// través de esta interfaz en vez de llamar directo al driver, para
// poder cambiar de driver/pantalla en el futuro sin tocar el resto
// del proyecto.

void renderer_init(void);
void renderer_clear(uint16_t color);
void renderer_draw_text(uint16_t x, uint16_t y, const char *text, uint16_t color, uint16_t bg, uint8_t scale);
void renderer_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void renderer_draw_pixel(uint16_t x, uint16_t y, uint16_t color);

#endif
