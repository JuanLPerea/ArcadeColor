#include "menu.h"
#include "renderer.h"
#include "controls.h"
#include "games/games_list.h"
#include "pico/stdlib.h"

/*
 * Selector tipo "carrete": el juego seleccionado aparece grande y
 * centrado, con el anterior y el siguiente arriba/abajo en letra
 * pequeña. Al cambiar de selección se anima un pequeño deslizamiento
 * vertical antes de asentar en la posición final.
 *
 * Pantalla real: 240 (ancho) x 320 (alto), retrato.
 */
#define TITLE_SCALE 3
#define PREV_NEXT_SCALE 2
#define SELECTED_SCALE_MAX 3   /* tope; se reduce sola si un título no cabe (ver best_fit_scale) */

#define TITLE_Y 8
#define DIVIDER_Y 36

#define CENTER_Y 170
#define ITEM_SPACING 60   /* distancia entre la fila central y prev/next */

#define ANIM_STEPS 5
#define ANIM_STEP_DELAY_MS 12

static const char *PROJECT_TITLE = "ARCADE COLOR";

/* Devuelve el nombre del juego en ese índice, o "" si está fuera de rango
 * (no hay anterior antes del primero, ni siguiente después del último). */
static const char *game_name_at(int idx) {
    if (idx < 0 || idx >= NUM_GAMES) return "";
    return games_list[idx].name;
}

/* Mayor escala (hasta max_scale) con la que el texto cabe en el
 * ancho de pantalla, usando el ancho real que reporta el driver
 * (st7789_text_width) en vez de una aproximación. Evita que
 * títulos largos como "SPACE INVADERS" se corten en una pantalla
 * de 240px de ancho a escalas grandes. */
static int best_fit_scale(const char *text, int max_scale) {
    for (int scale = max_scale; scale > 1; scale--) {
        if (st7789_text_width(text, (uint8_t)scale) <= TFT_WIDTH - 10) {
            return scale;
        }
    }
    return 1;
}

static int centered_x(const char *text, int scale) {
    int text_width = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (TFT_WIDTH - text_width) / 2;
    return (x < 0) ? 0 : x;
}

/* Borra una franja horizontal completa a la altura de una fila de
 * texto, del alto suficiente para la mayor escala usada. */
static void clear_row(int y) {
    int h = FONT_HEIGHT * SELECTED_SCALE_MAX + 8; /* margen generoso */
    renderer_fill_rect(0, y - h / 2, TFT_WIDTH, h, COLOR_BLACK);
}

static void draw_row(int y, const char *text, int scale, uint16_t color) {
    if (text[0] == '\0') return;
    renderer_draw_text(centered_x(text, scale), y, text, color, COLOR_BLACK, scale);
}

static void draw_title(void) {
    renderer_fill_rect(0, 0, TFT_WIDTH, DIVIDER_Y + 2, COLOR_BLACK);
    renderer_draw_text(centered_x(PROJECT_TITLE, TITLE_SCALE), TITLE_Y,
                        PROJECT_TITLE, COLOR_CYAN, COLOR_BLACK, TITLE_SCALE);
    renderer_fill_rect(10, DIVIDER_Y, TFT_WIDTH - 20, 2, COLOR_CYAN);
}

/* Dibuja el estado "asentado": anterior y siguiente pequeños,
 * seleccionado grande y centrado. */
static void draw_settled(int selected) {
    clear_row(CENTER_Y - ITEM_SPACING);
    clear_row(CENTER_Y);
    clear_row(CENTER_Y + ITEM_SPACING);

    const char *name = game_name_at(selected);

    draw_row(CENTER_Y - ITEM_SPACING, game_name_at(selected - 1),
              PREV_NEXT_SCALE, COLOR_WHITE);
    draw_row(CENTER_Y, name,
              best_fit_scale(name, SELECTED_SCALE_MAX), COLOR_YELLOW);
    draw_row(CENTER_Y + ITEM_SPACING, game_name_at(selected + 1),
              PREV_NEXT_SCALE, COLOR_WHITE);
}

/*
 * Anima la transición de "old_selected" a "new_selected" (deben
 * diferir en exactamente 1). Todas las filas se dibujan a la misma
 * escala pequeña mientras se desplazan -- solo al asentar (fuera de
 * esta función, con draw_settled) el elemento central crece. Eso da
 * el efecto de "carrete girando" sin tener que animar el tamaño de
 * fuente carácter a carácter.
 */
static void animate_transition(int old_selected, int new_selected) {
    int direction = (new_selected > old_selected) ? 1 : -1;

    /* Filas involucradas en el desplazamiento, en su posición ANTES
     * de la animación (todas a escala pequeña): la que sale por un
     * extremo, las dos que se deslizan a la posición vecina, y la
     * que entra por el otro extremo. */
    const char *outgoing = game_name_at(old_selected - direction);
    const char *slide_a  = game_name_at(old_selected);      /* -> centro pasa a prev/next */
    const char *slide_b  = game_name_at(new_selected);      /* prev/next pasa a centro    */
    const char *incoming = game_name_at(new_selected + direction);

    int y_outgoing_start = CENTER_Y - direction * ITEM_SPACING;
    int y_a_start = CENTER_Y;
    int y_b_start = CENTER_Y + direction * ITEM_SPACING;
    int y_incoming_start = CENTER_Y + direction * ITEM_SPACING * 2;

    int y_outgoing_end = y_outgoing_start - direction * ITEM_SPACING;
    int y_a_end = y_a_start - direction * ITEM_SPACING;
    int y_b_end = y_b_start - direction * ITEM_SPACING;
    int y_incoming_end = y_incoming_start - direction * ITEM_SPACING;

    for (int step = 1; step <= ANIM_STEPS; step++) {
        int t_num = step;
        int t_den = ANIM_STEPS;

        int y_outgoing = y_outgoing_start + (y_outgoing_end - y_outgoing_start) * t_num / t_den;
        int y_a        = y_a_start        + (y_a_end        - y_a_start)        * t_num / t_den;
        int y_b        = y_b_start        + (y_b_end        - y_b_start)        * t_num / t_den;
        int y_incoming = y_incoming_start + (y_incoming_end - y_incoming_start) * t_num / t_den;

        clear_row(y_outgoing);
        clear_row(y_a);
        clear_row(y_b);
        clear_row(y_incoming);

        draw_row(y_outgoing, outgoing, PREV_NEXT_SCALE, COLOR_WHITE);
        draw_row(y_a, slide_a, PREV_NEXT_SCALE, COLOR_WHITE);
        draw_row(y_b, slide_b, PREV_NEXT_SCALE, COLOR_WHITE);
        draw_row(y_incoming, incoming, PREV_NEXT_SCALE, COLOR_WHITE);

        renderer_flush(); // sin esto, este paso intermedio nunca llegaría a verse

        sleep_ms(ANIM_STEP_DELAY_MS);
    }
}

void menu_run(void) {
    int selected = 0;

    renderer_clear(COLOR_BLACK);
    draw_title();
    draw_settled(selected);
    renderer_flush();

    while (true) {
        // Una única lectura del PIO por vuelta del bucle, antes de
        // controls_menu_up()/controls_menu_down() (ver controls.c).
        controls_update();

        int new_selected = selected;

        if (controls_menu_down()) {
            if (selected < NUM_GAMES - 1) new_selected = selected + 1;
        }
        if (controls_menu_up()) {
            if (selected > 0) new_selected = selected - 1;
        }

        if (new_selected != selected) {
            animate_transition(selected, new_selected);
            selected = new_selected;
            draw_settled(selected);
            renderer_flush();
        }

        if (controls_menu_select()) {
            // TODO: cuando exista el submenú 1P/2P/Demo, sustituir este
            // GAME_MODE_1P fijo por el modo que elija el jugador.
            games_list[selected].run(GAME_MODE_1P);

            // Al volver del juego, redibuja todo desde cero.
            renderer_clear(COLOR_BLACK);
            draw_title();
            draw_settled(selected);
            renderer_flush();
        }

        sleep_ms(15);
    }
}
