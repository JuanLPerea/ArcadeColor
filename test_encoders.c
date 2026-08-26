/*
 * test_encoders.c
 * ----------------
 * Programa de diagnóstico INDEPENDIENTE del firmware principal.
 * No usa controls.c: lee el PIO directamente, sin ningún filtrado
 * de software por encima (ni umbral, ni debounce, ni clamp), para
 * poder ver el comportamiento crudo real del hardware.
 *
 * Para cada encoder muestra en pantalla:
 *   - Contador crudo del PIO (el registro Y, tal cual)
 *   - Delta desde la última muestra (positivo o negativo)
 *   - Velocidad en transiciones/segundo, medida en ventanas de
 *     SAMPLE_WINDOW_MS
 *   - Un contador de "vueltas de detent" estimadas, dividiendo el
 *     contador crudo por 4 (un ciclo completo de cuadratura), para
 *     que sea fácil contar "giré 3 clics" y comprobar si el número
 *     en pantalla coincide
 *
 * CÓMO USARLO:
 *   1. Añade este fichero como un ejecutable NUEVO en tu
 *      CMakeLists.txt (ver más abajo), sin tocar tu target
 *      principal.
 *   2. Compílalo y flashea SOLO este .uf2 a la Pico.
 *   3. Gira cada encoder despacio, clic a clic, y compara lo que
 *      ves en pantalla contra lo que has girado con la mano.
 *   4. Prueba también girando rápido.
 *   5. Cuéntame los números que veas (sobre todo si el delta salta
 *      de golpe varios números de una vez, o si ves deltas
 *      negativos cuando giras siempre en el mismo sentido) y con
 *      eso afinamos los parámetros con datos reales en vez de a
 *      ciegas.
 *
 * NOTA: si en tu ArcadeColor.c la pantalla se inicializa con una
 * llamada distinta a renderer_init(), sustitúyela aquí por la
 * misma que uses allí.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "quadrature_encoder.pio.h"
#include "renderer.h"

/* --- Pines (copiados de tu proyecto) --- */
#define PIN_ENC1_CLK 20
#define PIN_ENC1_DT  21
#define PIN_ENC2_CLK 26
#define PIN_ENC2_DT  27

#define ENCODER_PIO PIO0
#define ENCODER_SM1 0
#define ENCODER_SM2 1

/*
 * Tasa de muestreo del PIO a probar. Cambia esto y recompila para
 * comparar directamente distintos valores con los mismos giros:
 * prueba 0 (máxima velocidad, sin filtrar), luego 1000, 500, 200...
 */
#define TEST_MAX_STEP_RATE 1000

#define SAMPLE_WINDOW_MS 100

static PIO pio = ENCODER_PIO;

static void draw_label(int row, const char *label) {
    renderer_draw_text(10, 40 + row * 26, label, COLOR_CYAN, COLOR_BLACK, 2);
}

/* Borra solo la zona donde va el valor numérico y lo redibuja,
 * para evitar parpadeo del texto fijo (las etiquetas). */
static void draw_value(int row, const char *text) {
    uint16_t y = 40 + row * 26;
    renderer_fill_rect(150, y - 2, TFT_WIDTH - 160, 20, COLOR_BLACK);
    renderer_draw_text(150, y, text, COLOR_WHITE, COLOR_BLACK, 2);
}

int main() {
    stdio_init_all();
    renderer_init();

    renderer_clear(COLOR_BLACK);
    renderer_draw_text(10, 8, "TEST ENCODERS (raw PIO)", COLOR_YELLOW, COLOR_BLACK, 2);
    renderer_fill_rect(5, 30, TFT_WIDTH - 10, 2, COLOR_CYAN);

    draw_label(0, "J1 crudo:");
    draw_label(1, "J1 delta:");
    draw_label(2, "J1 vel t/s:");
    draw_label(3, "J1 pasos (/4):");
    draw_label(5, "J2 crudo:");
    draw_label(6, "J2 delta:");
    draw_label(7, "J2 vel t/s:");
    draw_label(8, "J2 pasos (/4):");

    uint offset = pio_add_program(pio, &quadrature_encoder_program);
    if (offset != 0) {
        renderer_draw_text(10, 200, "ERROR: offset PIO != 0", COLOR_YELLOW, COLOR_BLACK, 2);
        while (true) tight_loop_contents();
    }

    quadrature_encoder_program_init(pio, ENCODER_SM1, PIN_ENC1_CLK, TEST_MAX_STEP_RATE);
    quadrature_encoder_program_init(pio, ENCODER_SM2, PIN_ENC2_CLK, TEST_MAX_STEP_RATE);

    int32_t enc1_last = quadrature_encoder_get_count(pio, ENCODER_SM1);
    int32_t enc2_last = quadrature_encoder_get_count(pio, ENCODER_SM2);

    absolute_time_t window_start = get_absolute_time();

    char buf[32];

    while (true) {
        sleep_ms(SAMPLE_WINDOW_MS);

        int32_t enc1_now = quadrature_encoder_get_count(pio, ENCODER_SM1);
        int32_t enc2_now = quadrature_encoder_get_count(pio, ENCODER_SM2);

        int32_t delta1 = enc1_now - enc1_last;
        int32_t delta2 = enc2_now - enc2_last;

        int64_t elapsed_us =
            absolute_time_diff_us(window_start, get_absolute_time());
        float elapsed_s = (float)elapsed_us / 1000000.0f;

        float speed1 = (elapsed_s > 0.0f) ? (float)delta1 / elapsed_s : 0.0f;
        float speed2 = (elapsed_s > 0.0f) ? (float)delta2 / elapsed_s : 0.0f;

        /* J1 */
        snprintf(buf, sizeof(buf), "%ld", (long)enc1_now);
        draw_value(0, buf);
        snprintf(buf, sizeof(buf), "%+ld", (long)delta1);
        draw_value(1, buf);
        snprintf(buf, sizeof(buf), "%.0f", speed1);
        draw_value(2, buf);
        snprintf(buf, sizeof(buf), "%ld", (long)(enc1_now / 4));
        draw_value(3, buf);

        /* J2 */
        snprintf(buf, sizeof(buf), "%ld", (long)enc2_now);
        draw_value(5, buf);
        snprintf(buf, sizeof(buf), "%+ld", (long)delta2);
        draw_value(6, buf);
        snprintf(buf, sizeof(buf), "%.0f", speed2);
        draw_value(7, buf);
        snprintf(buf, sizeof(buf), "%ld", (long)(enc2_now / 4));
        draw_value(8, buf);

        /* También por USB serial, útil si quieres capturar un log
         * más largo en el PC mientras giras. */
        printf("J1 raw=%ld delta=%ld v=%.0f t/s | J2 raw=%ld delta=%ld v=%.0f t/s\n",
               (long)enc1_now, (long)delta1, speed1,
               (long)enc2_now, (long)delta2, speed2);

        enc1_last = enc1_now;
        enc2_last = enc2_now;
        window_start = get_absolute_time();
    }
}
