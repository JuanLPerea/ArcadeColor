#ifndef ST7789_H
#define ST7789_H

#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------
// Definición de pines (ajusta si cambias el cableado)
// ---------------------------------------------------------
#define PIN_CS   17
#define PIN_DC   14
#define PIN_RST  15
#define PIN_SCK  18
#define PIN_MOSI 19

// SPI usado (GP18/GP19 corresponden a SPI0 en la Pico)
#define TFT_SPI      spi0
#define TFT_SPI_FREQ (20 * 1000 * 1000)  // 20 MHz; sube a 32 MHz si tu cableado lo aguanta limpio

// Resolución efectiva actual (se ajusta con st7789_set_rotation).
// TFT_WIDTH/TFT_HEIGHT siguen usándose igual que antes en todo el
// proyecto, pero ahora reflejan la orientación activa.
extern uint16_t st7789_screen_w;
extern uint16_t st7789_screen_h;
#define TFT_WIDTH  st7789_screen_w
#define TFT_HEIGHT st7789_screen_h

// Offsets de fila/columna. En la mayoría de módulos de 2.0" son 0,
// pero en algunos (según el recorte del panel) hace falta 0/80 etc.
#define TFT_COL_OFFSET 0
#define TFT_ROW_OFFSET 0

// Colores básicos en formato RGB565
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F

void st7789_init(void);
void st7789_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void st7789_fill_screen(uint16_t color);
void st7789_draw_pixel(uint16_t x, uint16_t y, uint16_t color);
void st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

// ---------------------------------------------------------
// Texto (fuente bitmap 5x7). Soporta may\u00fasculas, d\u00edgitos,
// espacio, ':' , '.' y '-'. Cualquier otro car\u00e1cter se
// dibuja en blanco (hueco).
// ---------------------------------------------------------
#define FONT_WIDTH  5
#define FONT_HEIGHT 7

void st7789_draw_char(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg, uint8_t scale);
void st7789_draw_text(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t scale);
uint16_t st7789_text_width(const char *str, uint8_t scale);

// ---------------------------------------------------------
// Volcado rápido de un buffer de píxeles completo (framebuffer).
// A diferencia de draw_pixel/fill_rect, fija la ventana UNA sola
// vez y transmite todo el bloque de datos seguido, sin reabrir
// la ventana por cada píxel. Imprescindible para animaciones
// fluidas a pantalla completa.
// ---------------------------------------------------------
void st7789_blit(uint16_t x0, uint16_t y0, uint16_t w, uint16_t h, const uint16_t *buf);

// ---------------------------------------------------------
// Rotación de pantalla: 0=0°, 1=90°, 2=180°, 3=270°.
// Ajusta MADCTL y actualiza TFT_WIDTH/TFT_HEIGHT en consecuencia.
// Llamar después de st7789_init(). Si al probarla la imagen no
// queda como se espera, prueba con los otros valores (0-3):
// alguno de ellos será el correcto para tu panel concreto.
// ---------------------------------------------------------
void st7789_set_rotation(uint8_t rotation);

#endif
