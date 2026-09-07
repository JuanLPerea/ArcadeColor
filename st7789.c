#include "st7789.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include <string.h>

// ---------------------------------------------------------
// Comandos ST7789 usados
// ---------------------------------------------------------
#define CMD_SWRESET 0x01
#define CMD_SLPOUT  0x11
#define CMD_INVON   0x21
#define CMD_NORON   0x13
#define CMD_DISPON  0x29
#define CMD_CASET   0x2A
#define CMD_RASET   0x2B
#define CMD_RAMWR   0x2C
#define CMD_MADCTL  0x36
#define CMD_COLMOD  0x3A

static inline void cs_select(void)   { gpio_put(PIN_CS, 0); }
static inline void cs_deselect(void) { gpio_put(PIN_CS, 1); }

// Resolución efectiva actual (ver st7789_set_rotation)
uint16_t st7789_screen_w = 240;
uint16_t st7789_screen_h = 320;

static void write_cmd(uint8_t cmd) {
    cs_select();
    gpio_put(PIN_DC, 0); // comando
    spi_write_blocking(TFT_SPI, &cmd, 1);
    cs_deselect();
}

static void write_data(const uint8_t *data, size_t len) {
    cs_select();
    gpio_put(PIN_DC, 1); // datos
    spi_write_blocking(TFT_SPI, data, len);
    cs_deselect();
}

static inline void write_data_byte(uint8_t b) {
    write_data(&b, 1);
}

// ---------------------------------------------------------
// Framebuffer (doble buffer)
//
// Se guarda YA en formato "de cable": 2 bytes por píxel, byte
// alto primero (el orden que espera el ST7789 por SPI). Así, al
// hacer flush(), los bytes se envían tal cual están en memoria,
// sin ningún paso de recomposición byte a byte.
//
// El tamaño se reserva para el número máximo de píxeles del panel
// (320*240 = 76800), que es el mismo total en cualquier rotación
// (0-3) porque solo cambia cuál de las dos dimensiones es "ancho".
// ---------------------------------------------------------
#define FB_MAX_PIXELS ((uint32_t)320 * 240)
static uint8_t framebuffer[FB_MAX_PIXELS * 2];

// Rectángulo "sucio": la zona mínima que ha cambiado desde el
// último flush. Coordenadas inclusivas.
static bool fb_dirty = false;
static uint16_t fb_dirty_x0, fb_dirty_y0, fb_dirty_x1, fb_dirty_y1;

static inline void mark_dirty(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    if (x0 >= TFT_WIDTH || y0 >= TFT_HEIGHT) return;
    if (x1 >= TFT_WIDTH)  x1 = TFT_WIDTH - 1;
    if (y1 >= TFT_HEIGHT) y1 = TFT_HEIGHT - 1;

    if (!fb_dirty) {
        fb_dirty_x0 = x0; fb_dirty_y0 = y0;
        fb_dirty_x1 = x1; fb_dirty_y1 = y1;
        fb_dirty = true;
    } else {
        if (x0 < fb_dirty_x0) fb_dirty_x0 = x0;
        if (y0 < fb_dirty_y0) fb_dirty_y0 = y0;
        if (x1 > fb_dirty_x1) fb_dirty_x1 = x1;
        if (y1 > fb_dirty_y1) fb_dirty_y1 = y1;
    }
}

static inline uint32_t fb_index(uint16_t x, uint16_t y) {
    return ((uint32_t)y * TFT_WIDTH + x) * 2;
}

void st7789_init(void) {
    // --- GPIO ---
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    gpio_init(PIN_DC);
    gpio_set_dir(PIN_DC, GPIO_OUT);

    gpio_init(PIN_RST);
    gpio_set_dir(PIN_RST, GPIO_OUT);

    // --- SPI ---
    spi_init(TFT_SPI, TFT_SPI_FREQ);
    // El ST7789 espera modo SPI 3 (CPOL=1, CPHA=1); el SDK por defecto
    // deja el bus en modo 0, lo que impide que el panel interprete
    // correctamente los comandos y puede dejar la pantalla en negro.
    spi_set_format(TFT_SPI, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    // Nota: no se usa MISO, la pantalla es de solo escritura

    // --- Reset físico ---
    gpio_put(PIN_RST, 1);
    sleep_ms(10);
    gpio_put(PIN_RST, 0);
    sleep_ms(10);
    gpio_put(PIN_RST, 1);
    sleep_ms(150);

    // --- Secuencia de inicialización ---
    write_cmd(CMD_SWRESET);
    sleep_ms(150);

    write_cmd(CMD_SLPOUT);
    sleep_ms(255);

    write_cmd(CMD_COLMOD);
    write_data_byte(0x55); // 16 bits por píxel (RGB565)
    sleep_ms(10);

    write_cmd(CMD_MADCTL);
    write_data_byte(0x00); // valor temporal; st7789_set_rotation() lo sobrescribe justo debajo

    write_cmd(CMD_INVON);  // muchos paneles ST7789 necesitan inversión para colores correctos
    sleep_ms(10);

    write_cmd(CMD_NORON);
    sleep_ms(10);

    write_cmd(CMD_DISPON);
    sleep_ms(100);

    // Apaisada (320 ancho x 240 alto): rotación 1 (90°). Si la
    // imagen sale espejada o con la orientación incorrecta, prueba
    // rotación 3 (también 320x240, pero con el otro sentido de
    // intercambio fila/columna).
    st7789_set_rotation(1);

    // Arranca con el framebuffer y la pantalla física en negro, para
    // que no se vea basura de la GRAM antes del primer dibujo real.
    memset(framebuffer, 0, sizeof(framebuffer));
    mark_dirty(0, 0, TFT_WIDTH - 1, TFT_HEIGHT - 1);
    st7789_flush();
}

void st7789_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    x0 += TFT_COL_OFFSET; x1 += TFT_COL_OFFSET;
    y0 += TFT_ROW_OFFSET; y1 += TFT_ROW_OFFSET;

    uint8_t buf[4];

    write_cmd(CMD_CASET);
    buf[0] = x0 >> 8; buf[1] = x0 & 0xFF;
    buf[2] = x1 >> 8; buf[3] = x1 & 0xFF;
    write_data(buf, 4);

    write_cmd(CMD_RASET);
    buf[0] = y0 >> 8; buf[1] = y0 & 0xFF;
    buf[2] = y1 >> 8; buf[3] = y1 & 0xFF;
    write_data(buf, 4);

    write_cmd(CMD_RAMWR);
}

// ---------------------------------------------------------
// Dibujo: TODO esto ahora escribe en el framebuffer (RAM pura,
// sin SPI) y marca la zona tocada como sucia. Nada llega a la
// pantalla física hasta el siguiente st7789_flush().
// ---------------------------------------------------------

void st7789_draw_pixel(uint16_t x, uint16_t y, uint16_t color) {
    if (x >= TFT_WIDTH || y >= TFT_HEIGHT) return;
    uint32_t idx = fb_index(x, y);
    framebuffer[idx]     = color >> 8;
    framebuffer[idx + 1] = color & 0xFF;
    mark_dirty(x, y, x, y);
}

void st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (x >= TFT_WIDTH || y >= TFT_HEIGHT) return;
    if (w == 0 || h == 0) return;
    if (x + w > TFT_WIDTH)  w = TFT_WIDTH - x;
    if (y + h > TFT_HEIGHT) h = TFT_HEIGHT - y;

    uint8_t hi = color >> 8, lo = color & 0xFF;

    for (uint16_t row = 0; row < h; row++) {
        uint32_t offset = fb_index(x, y + row);
        for (uint16_t col = 0; col < w; col++) {
            framebuffer[offset]     = hi;
            framebuffer[offset + 1] = lo;
            offset += 2;
        }
    }

    mark_dirty(x, y, x + w - 1, y + h - 1);
}

void st7789_fill_screen(uint16_t color) {
    // Caso rápido: negro y blanco tienen los dos bytes iguales
    // (0x00/0x00 y 0xFF/0xFF), así que se pueden rellenar con
    // memset en vez de un bucle píxel a píxel.
    if (color == COLOR_BLACK || color == COLOR_WHITE) {
        uint8_t b = (color == COLOR_BLACK) ? 0x00 : 0xFF;
        memset(framebuffer, b, (size_t)TFT_WIDTH * TFT_HEIGHT * 2);
        mark_dirty(0, 0, TFT_WIDTH - 1, TFT_HEIGHT - 1);
        return;
    }
    st7789_fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, color);
}

// ---------------------------------------------------------
// Flush: transmite SOLO el rectángulo sucio, en una única sesión
// SPI. Se fija la ventana (CASET/RASET/RAMWR) una vez, y dentro
// de esa ventana se envían los bytes de cada fila sucia seguidos;
// el controlador ST7789 avanza solo de fila en fila dentro de la
// ventana, así que no hace falta reabrir RASET por cada una.
// ---------------------------------------------------------
void st7789_flush(void) {
    if (!fb_dirty) return;

    uint16_t x0 = fb_dirty_x0, y0 = fb_dirty_y0;
    uint16_t x1 = fb_dirty_x1, y1 = fb_dirty_y1;
    uint16_t w = x1 - x0 + 1;

    st7789_set_window(x0, y0, x1, y1);

    cs_select();
    gpio_put(PIN_DC, 1);
    for (uint16_t row = y0; row <= y1; row++) {
        uint32_t offset = fb_index(x0, row);
        spi_write_blocking(TFT_SPI, &framebuffer[offset], (size_t)w * 2);
    }
    cs_deselect();

    fb_dirty = false;
}

// ---------------------------------------------------------
// Fuente bitmap 5x7. Cubre ' ' (0x20) hasta 'Z' (0x5A):
// espacio, signos básicos, dígitos 0-9 y mayúsculas A-Z.
// Cada carácter son 7 bytes (uno por fila), usando los 5 bits
// bajos de cada byte para las 5 columnas (bit4 = columna
// izquierda, bit0 = columna derecha).
// ---------------------------------------------------------
static const uint8_t font5x7[59][7] = {
    /* ' ' */ {0b00000,0b00000,0b00000,0b00000,0b00000,0b00000,0b00000},
    /* '!' */ {0b00100,0b00100,0b00100,0b00100,0b00100,0b00000,0b00100},
    /* '"' */ {0,0,0,0,0,0,0},
    /* '#' */ {0,0,0,0,0,0,0},
    /* '$' */ {0,0,0,0,0,0,0},
    /* '%' */ {0,0,0,0,0,0,0},
    /* '&' */ {0,0,0,0,0,0,0},
    /* '\''*/ {0,0,0,0,0,0,0},
    /* '(' */ {0,0,0,0,0,0,0},
    /* ')' */ {0,0,0,0,0,0,0},
    /* '*' */ {0,0,0,0,0,0,0},
    /* '+' */ {0,0,0,0,0,0,0},
    /* ',' */ {0,0,0,0,0,0,0},
    /* '-' */ {0b00000,0b00000,0b00000,0b11111,0b00000,0b00000,0b00000},
    /* '.' */ {0b00000,0b00000,0b00000,0b00000,0b00000,0b00000,0b00100},
    /* '/' */ {0,0,0,0,0,0,0},
    /* '0' */ {0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110},
    /* '1' */ {0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110},
    /* '2' */ {0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111},
    /* '3' */ {0b11111,0b00010,0b00100,0b00010,0b00001,0b10001,0b01110},
    /* '4' */ {0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010},
    /* '5' */ {0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110},
    /* '6' */ {0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110},
    /* '7' */ {0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000},
    /* '8' */ {0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110},
    /* '9' */ {0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100},
    /* ':' */ {0b00000,0b00100,0b00000,0b00000,0b00000,0b00100,0b00000},
    /* ';' */ {0,0,0,0,0,0,0},
    /* '<' */ {0,0,0,0,0,0,0},
    /* '=' */ {0,0,0,0,0,0,0},
    /* '>' */ {0,0,0,0,0,0,0},
    /* '?' */ {0,0,0,0,0,0,0},
    /* '@' */ {0,0,0,0,0,0,0},
    /* 'A' */ {0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001},
    /* 'B' */ {0b11110,0b10001,0b10001,0b11110,0b10001,0b10001,0b11110},
    /* 'C' */ {0b01111,0b10000,0b10000,0b10000,0b10000,0b10000,0b01111},
    /* 'D' */ {0b11110,0b10001,0b10001,0b10001,0b10001,0b10001,0b11110},
    /* 'E' */ {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111},
    /* 'F' */ {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b10000},
    /* 'G' */ {0b01111,0b10000,0b10000,0b10111,0b10001,0b10001,0b01111},
    /* 'H' */ {0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001},
    /* 'I' */ {0b01110,0b00100,0b00100,0b00100,0b00100,0b00100,0b01110},
    /* 'J' */ {0b00001,0b00001,0b00001,0b00001,0b00001,0b10001,0b01110},
    /* 'K' */ {0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001},
    /* 'L' */ {0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111},
    /* 'M' */ {0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001},
    /* 'N' */ {0b10001,0b11001,0b10101,0b10101,0b10011,0b10001,0b10001},
    /* 'O' */ {0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110},
    /* 'P' */ {0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000},
    /* 'Q' */ {0b01110,0b10001,0b10001,0b10001,0b10101,0b10010,0b01101},
    /* 'R' */ {0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001},
    /* 'S' */ {0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110},
    /* 'T' */ {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100},
    /* 'U' */ {0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110},
    /* 'V' */ {0b10001,0b10001,0b10001,0b10001,0b10001,0b01010,0b00100},
    /* 'W' */ {0b10001,0b10001,0b10001,0b10101,0b10101,0b10101,0b01010},
    /* 'X' */ {0b10001,0b10001,0b01010,0b00100,0b01010,0b10001,0b10001},
    /* 'Y' */ {0b10001,0b10001,0b01010,0b00100,0b00100,0b00100,0b00100},
    /* 'Z' */ {0b11111,0b00001,0b00010,0b00100,0b01000,0b10000,0b11111},
};

void st7789_draw_char(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg, uint8_t scale) {
    if (scale == 0) scale = 1;
    if (c < ' ' || c > 'Z') c = ' '; // fuera de rango -> hueco en blanco
    const uint8_t *glyph = font5x7[c - ' '];

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            bool on = (bits >> (FONT_WIDTH - 1 - col)) & 0x01;
            uint16_t px_color = on ? color : bg;
            st7789_fill_rect(x + col * scale, y + row * scale, scale, scale, px_color);
        }
    }
}

void st7789_draw_text(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t scale) {
    uint16_t cursor_x = x;
    while (*str) {
        st7789_draw_char(cursor_x, y, *str, color, bg, scale);
        cursor_x += (FONT_WIDTH + 1) * scale; // +1 columna de espacio entre letras
        str++;
    }
}

uint16_t st7789_text_width(const char *str, uint8_t scale) {
    if (scale == 0) scale = 1;
    size_t len = 0;
    while (str[len]) len++;
    if (len == 0) return 0;
    return (uint16_t)(len * (FONT_WIDTH + 1) * scale - scale); // sin espacio sobrante al final
}

// ---------------------------------------------------------
// st7789_blit: escritura DIRECTA al panel, sin pasar por el
// framebuffer ni por el rectángulo sucio. Se mantiene igual que
// antes (para compatibilidad con quien ya la usaba así); ver aviso
// en st7789.h sobre mezclarla con las funciones de dibujo de arriba.
// ---------------------------------------------------------
void st7789_blit(uint16_t x0, uint16_t y0, uint16_t w, uint16_t h, const uint16_t *buf) {
    st7789_set_window(x0, y0, x0 + w - 1, y0 + h - 1);

    cs_select();
    gpio_put(PIN_DC, 1);

    uint32_t total_px = (uint32_t)w * h;
    uint8_t chunk[256]; // 128 píxeles por tanda
    uint32_t idx = 0;

    while (idx < total_px) {
        uint32_t n = total_px - idx;
        if (n > 128) n = 128;

        for (uint32_t i = 0; i < n; i++) {
            uint16_t c = buf[idx + i];
            chunk[2 * i]     = c >> 8;   // el ST7789 espera byte alto primero
            chunk[2 * i + 1] = c & 0xFF;
        }
        spi_write_blocking(TFT_SPI, chunk, n * 2);
        idx += n;
    }

    cs_deselect();
}

void st7789_blit_to_buffer(uint16_t x0, uint16_t y0, uint16_t w, uint16_t h, const uint16_t *buf) {
    if (x0 >= TFT_WIDTH || y0 >= TFT_HEIGHT) return;
    if (x0 + w > TFT_WIDTH)  w = TFT_WIDTH - x0;
    if (y0 + h > TFT_HEIGHT) h = TFT_HEIGHT - y0;

    for (uint16_t row = 0; row < h; row++) {
        uint32_t offset = fb_index(x0, y0 + row);
        const uint16_t *src_row = buf + (uint32_t)row * w;
        for (uint16_t col = 0; col < w; col++) {
            uint16_t c = src_row[col];
            framebuffer[offset]     = c >> 8;
            framebuffer[offset + 1] = c & 0xFF;
            offset += 2;
        }
    }

    mark_dirty(x0, y0, x0 + w - 1, y0 + h - 1);
}

void st7789_set_rotation(uint8_t rotation) {
    uint8_t madctl;

    switch (rotation & 0x03) {
        case 0: madctl = 0x00; st7789_screen_w = 240; st7789_screen_h = 320; break;
        case 1: madctl = 0x60; st7789_screen_w = 320; st7789_screen_h = 240; break;
        case 2: madctl = 0xC0; st7789_screen_w = 240; st7789_screen_h = 320; break;
        default: madctl = 0xA0; st7789_screen_w = 320; st7789_screen_h = 240; break;
    }

    write_cmd(CMD_MADCTL);
    write_data_byte(madctl);
}
