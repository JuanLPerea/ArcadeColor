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

// ---------------------------------------------------------
// DOBLE BUFFER (framebuffer en RAM)
// ---------------------------------------------------------
// A partir de ahora, TODAS las funciones de dibujo de más abajo
// (draw_pixel, fill_rect, fill_screen, draw_char, draw_text)
// escriben en un framebuffer que vive en RAM -- no tocan el SPI
// para nada. La pantalla física no cambia hasta que llamas a
// st7789_flush().
//
// Esto tiene dos efectos importantes a tener en cuenta:
//
//  1) Nada se ve en pantalla hasta que llamas a st7789_flush().
//     Puedes hacer todos los draw_*/fill_* que quieras entre dos
//     flush -- se acumulan en RAM sin coste de SPI -- pero como
//     mínimo necesitas un flush al final de cada "frame" visible.
//
//  2) st7789_flush() solo transmite la zona que realmente ha
//     cambiado desde el último flush (rectángulo "sucio"), no la
//     pantalla entera. Eso es importante por el tipo de conexión:
//     a 20 MHz por SPI, una pantalla completa (320x240x2 bytes)
//     tarda ~60 ms en transmitirse; una sola fila de texto que
//     cambia, en cambio, son solo unos pocos ms. Si necesitas
//     forzar un refresco completo (p.ej. tras cambiar de juego),
//     usa st7789_fill_screen() antes del flush: al escribir sobre
//     toda la pantalla, marca toda la pantalla como sucia.
void st7789_flush(void);

void st7789_fill_screen(uint16_t color);
void st7789_draw_pixel(uint16_t x, uint16_t y, uint16_t color);
void st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

// ---------------------------------------------------------
// Texto (fuente bitmap 5x7). Soporta mayúsculas, dígitos,
// espacio, ':' , '.' y '-'. Cualquier otro carácter se
// dibuja en blanco (hueco).
// ---------------------------------------------------------
#define FONT_WIDTH  5
#define FONT_HEIGHT 7

void st7789_draw_char(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg, uint8_t scale);
void st7789_draw_text(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t scale);
uint16_t st7789_text_width(const char *str, uint8_t scale);

// ---------------------------------------------------------
// st7789_blit: vuelca un buffer de píxeles EXTERNO directo al
// panel, EN EL ACTO, sin pasar por el framebuffer interno ni por
// el rectángulo sucio. Útil si algún juego ya lo usaba así y
// necesita la máxima velocidad para un sprite puntual.
//
// OJO si lo mezclas con las funciones de dibujo de arriba: como
// éstas se acumulan en RAM hasta el siguiente st7789_flush() y
// st7789_blit() en cambio escribe al momento, el orden en que
// aparecen en pantalla puede no coincidir con el orden en que las
// llamas. Si quieres que un sprite externo respete el mismo orden
// que el resto del frame, usa st7789_blit_to_buffer() en su lugar.
// ---------------------------------------------------------
void st7789_blit(uint16_t x0, uint16_t y0, uint16_t w, uint16_t h, const uint16_t *buf);

// Igual que st7789_blit, pero copia el buffer externo AL
// framebuffer interno (marcando la zona como sucia) en vez de
// escribir directo al SPI. Se muestra en el siguiente
// st7789_flush(), igual que el resto de dibujo.
void st7789_blit_to_buffer(uint16_t x0, uint16_t y0, uint16_t w, uint16_t h, const uint16_t *buf);

// ---------------------------------------------------------
// Rotación de pantalla: 0=0°, 1=90°, 2=180°, 3=270°.
// Ajusta MADCTL y actualiza TFT_WIDTH/TFT_HEIGHT en consecuencia.
// Llamar después de st7789_init(). Si al probarla la imagen no
// queda como se espera, prueba con los otros valores (0-3):
// alguno de ellos será el correcto para tu panel concreto.
//
// NOTA: el framebuffer interno está reservado para el tamaño
// máximo del panel (320x240, el mismo número de píxeles en
// cualquier rotación), así que cambiar de rotación en marcha no
// necesita reservar más memoria -- pero si lo haces a media
// ejecución, fuerza un st7789_fill_screen() + st7789_flush() para
// que el rectángulo sucio se recalcule con las dimensiones nuevas.
// ---------------------------------------------------------
void st7789_set_rotation(uint8_t rotation);

#endif
