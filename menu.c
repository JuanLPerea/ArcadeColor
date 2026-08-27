#include "menu.h"
#include "renderer.h"
#include "controls.h"
#include "games/games_list.h"
#include "highscores.h"
#include "pico/stdlib.h"
#include <math.h>

/*
 * Selector tipo "carrete": el juego seleccionado aparece grande y
 * centrado, con el anterior y el siguiente arriba/abajo en letra
 * pequeña. La lista es circular: tras el último elemento vuelve al
 * primero, y viceversa.
 *
 * Pantalla real: 320 (ancho) x 240 (alto), apaisada.
 *
 * Última entrada del carrete: "OPCIONES" (TOTAL_ITEMS = NUM_GAMES + 1),
 * de momento un marcador de posición para futuras funciones de test.
 *
 * DISEÑO SIMPLIFICADO (antes había un bug de borrado con 4 filas
 * animándose a la vez -- dos visibles y dos entrando/saliendo fuera
 * de pantalla): ahora solo existen 3 posiciones fijas en pantalla
 * (prev, centro, next) y, como mucho, 2 filas de texto se mueven a
 * la vez entre esas 3 posiciones. Nunca hay una 4ª fila fantasma.
 */
#define TITLE_SCALE 3
#define PREV_NEXT_SCALE 2
#define SELECTED_SCALE_MAX 4   /* tope; se reduce sola si un título no cabe (ver best_fit_scale) */

#define TITLE_Y 8
#define DIVIDER_Y 36

#define CENTER_Y 120
#define ITEM_SPACING 50   /* distancia entre la fila central y prev/next */

#define ANIM_STEPS 5
#define ANIM_STEP_DELAY_MS 12

#define TOTAL_ITEMS (NUM_GAMES + 1)   /* +1 = entrada "OPCIONES" al final */
#define OPTIONS_INDEX NUM_GAMES

/*
 * Ciclo automático ("attract mode"), portado del mismo concepto de
 * ArcadePi: tras un rato sin ninguna interacción, se muestra la
 * pantalla de récords del juego seleccionado (unos segundos) y
 * luego se lanza su demo; al terminar, se avanza al siguiente juego
 * y se repite. Cualquier input (girar o pulsar) en cualquier punto
 * del ciclo lo cancela y devuelve el control inmediato al usuario.
 *
 * ASUNCIÓN: se asume que existe GAME_MODE_DEMO en el mismo enum que
 * GAME_MODE_1P (games_list.h), análogo a pong_demo() en ArcadePi
 * pero como modo de un único run() en vez de una función aparte. Si
 * el nombre real es otro, es cambiar esta constante.
 */
#define IDLE_TIMEOUT_MS (30 * 1000)   /* inactividad antes de arrancar el ciclo */
#define SCORES_DISPLAY_MS (5 * 1000)  /* cuánto se ve la pantalla de récords */
#define ATTRACT_DEMO_MODE GAME_MODE_DEMO

static const char *PROJECT_TITLE = "ARCADE COLOR";

/* Envuelve un índice al rango [0, TOTAL_ITEMS), para que la lista
 * sea circular. */
static int wrap_index(int idx) {
    idx %= TOTAL_ITEMS;
    if (idx < 0) idx += TOTAL_ITEMS;
    return idx;
}

/* Devuelve el nombre de la entrada en ese índice ("OPCIONES" para la
 * última). El índice se envuelve, así que siempre hay anterior y
 * siguiente -- no hace falta comprobar límites en las llamadas. */
static const char *game_name_at(int idx) {
    idx = wrap_index(idx);
    if (idx == OPTIONS_INDEX) return "OPCIONES";
    return games_list[idx].name;
}

/* Mayor escala (hasta max_scale) con la que el texto cabe en el
 * ancho de pantalla, usando el ancho real que reporta el driver
 * (st7789_text_width) en vez de una aproximación. Evita que
 * títulos largos como "SPACE INVADERS" se corten. */
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

/*
 * Borra la zona de una fila de texto.
 *
 * renderer_draw_text dibuja con "y" como la esquina SUPERIOR del
 * texto (crece hacia abajo desde ahí), así que el rectángulo a
 * borrar se ancla arriba en "y" y se extiende hacia abajo -- no va
 * centrado en "y". La altura usa siempre SELECTED_SCALE_MAX (el
 * mayor posible) sea cual sea la escala realmente dibujada en esa
 * fila, para no quedarse corto nunca.
 */
static void clear_row(int y) {
    int top_margin = 4;
    int bottom_margin = 6;
    int height = top_margin + FONT_HEIGHT * SELECTED_SCALE_MAX + bottom_margin;
    renderer_fill_rect(0, y - top_margin, TFT_WIDTH, height, COLOR_BLACK);
}

static void draw_row(int y, const char *text, int scale, uint16_t color) {
    renderer_draw_text(centered_x(text, scale), y, text, color, COLOR_BLACK, scale);
}

static void draw_title(void) {
    renderer_fill_rect(0, 0, TFT_WIDTH, DIVIDER_Y + 2, COLOR_BLACK);
    renderer_draw_text(centered_x(PROJECT_TITLE, TITLE_SCALE), TITLE_Y,
                        PROJECT_TITLE, COLOR_CYAN, COLOR_BLACK, TITLE_SCALE);
    renderer_fill_rect(10, DIVIDER_Y, TFT_WIDTH - 20, 2, COLOR_CYAN);
}

/*
 * Título animado: cada letra sube/baja siguiendo una onda senoidal
 * (desfasada por posición, así se ve viajar por el texto) y su
 * color se recorre por una paleta, también desfasado por letra --
 * da un efecto arcoíris desplazándose junto con la ola.
 *
 * La línea separadora (borde inferior del título) se dibuja UNA
 * sola vez en draw_title() y no se vuelve a tocar aquí: solo se
 * borra/redibuja la franja de altura justa para el texto + la
 * amplitud de la ola, para no gastar ancho de banda SPI de más.
 */
#define TITLE_WAVE_AMPLITUDE 4
#define TITLE_WAVE_SPEED 0.28f
#define TITLE_WAVE_CHAR_PHASE 0.6f
#define TITLE_COLOR_CYCLE_EVERY 6     /* frames de animación entre cada cambio de color */
#define TITLE_ANIM_THROTTLE 3         /* redibuja 1 de cada N vueltas del bucle principal (~45 ms) */

static const uint16_t title_palette[] = {
    COLOR_CYAN, COLOR_YELLOW, COLOR_MAGENTA, COLOR_GREEN, COLOR_RED, COLOR_WHITE
};
#define TITLE_PALETTE_LEN (int)(sizeof(title_palette) / sizeof(title_palette[0]))

#define TITLE_BAND_HEIGHT (FONT_HEIGHT * TITLE_SCALE + 2 * TITLE_WAVE_AMPLITUDE + 4)

static float title_wave_phase = 0.0f;
static int title_color_phase = 0;

static void draw_title_wave(void) {
    int text_w = (int)st7789_text_width(PROJECT_TITLE, TITLE_SCALE);
    int x = (TFT_WIDTH - text_w) / 2;
    if (x < 0) x = 0;

    int band_y = TITLE_Y - TITLE_WAVE_AMPLITUDE - 2;
    renderer_fill_rect(0, band_y, TFT_WIDTH, TITLE_BAND_HEIGHT, COLOR_BLACK);

    for (int i = 0; PROJECT_TITLE[i] != '\0'; i++) {
        float angle = title_wave_phase + i * TITLE_WAVE_CHAR_PHASE;
        int y_offset = (int)(sinf(angle) * TITLE_WAVE_AMPLITUDE);
        uint16_t color = title_palette[(i + title_color_phase) % TITLE_PALETTE_LEN];

        renderer_draw_char(x, TITLE_Y + y_offset, PROJECT_TITLE[i],
                            color, COLOR_BLACK, TITLE_SCALE);

        x += (FONT_WIDTH + 1) * TITLE_SCALE;
    }
}

/* Avanza la animación del título y la redibuja, con throttling para
 * no competir demasiado por el bus SPI con el resto del menú. Hace
 * su propio renderer_flush(). */
static void update_title_animation(void) {
    static int throttle_counter = 0;
    static int color_tick = 0;

    throttle_counter++;
    if (throttle_counter < TITLE_ANIM_THROTTLE) return;
    throttle_counter = 0;

    title_wave_phase += TITLE_WAVE_SPEED;
    if (title_wave_phase > 6.2831853f) title_wave_phase -= 6.2831853f;

    color_tick++;
    if (color_tick >= TITLE_COLOR_CYCLE_EVERY) {
        color_tick = 0;
        title_color_phase = (title_color_phase + 1) % TITLE_PALETTE_LEN;
    }

    draw_title_wave();
    renderer_flush();
}

/* Dibuja el estado "asentado": anterior y siguiente pequeños,
 * seleccionado grande y centrado. Siempre exactamente 3 filas. */
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
 * Anima la transición de "old_selected" a "new_selected".
 * direction: +1 = bajar (siguiente), -1 = subir (anterior). Se pasa
 * explícito en vez de inferirlo comparando índices porque, con lista
 * circular, "new_selected < old_selected" ya no indica el sentido
 * real (p.ej. del último elemento al primero es ir "hacia adelante"
 * aunque el índice baje).
 *
 * Solo 2 filas se mueven a la vez, dentro de las 3 posiciones fijas
 * (prev/centro/next). La fila que "sale" y la que se "revela" se
 * dejan vacías durante toda la animación -- se rellena la revelada
 * al asentar (draw_settled), fuera de esta función. Así nunca hay
 * más de 3 filas con texto en pantalla, ni durante la transición.
 */
static void animate_transition(int old_selected, int new_selected, int direction) {
    const char *slide_center = game_name_at(old_selected); /* centro -> prev/next */
    const char *slide_edge   = game_name_at(new_selected); /* prev/next -> centro */

    int y_center_start = CENTER_Y;
    int y_center_end   = CENTER_Y - direction * ITEM_SPACING;
    int y_edge_start   = CENTER_Y + direction * ITEM_SPACING;
    int y_edge_end     = CENTER_Y;

    int y_vacating  = (direction > 0) ? (CENTER_Y - ITEM_SPACING) : (CENTER_Y + ITEM_SPACING);
    int y_revealing = (direction > 0) ? (CENTER_Y + ITEM_SPACING) : (CENTER_Y - ITEM_SPACING);
    clear_row(y_vacating);
    clear_row(y_revealing);

    int y_center_prev = y_center_start;
    int y_edge_prev = y_edge_start;

    for (int step = 1; step <= ANIM_STEPS; step++) {
        int y_center = y_center_start + (y_center_end - y_center_start) * step / ANIM_STEPS;
        int y_edge   = y_edge_start   + (y_edge_end   - y_edge_start)   * step / ANIM_STEPS;

        /* Borra tanto la posición nueva como la del paso anterior:
         * cubre cualquier hueco si el salto entre pasos fuera mayor
         * que el margen de clear_row. */
        clear_row(y_center_prev);
        clear_row(y_edge_prev);
        clear_row(y_center);
        clear_row(y_edge);

        draw_row(y_center, slide_center, PREV_NEXT_SCALE, COLOR_WHITE);
        draw_row(y_edge, slide_edge, PREV_NEXT_SCALE, COLOR_WHITE);

        renderer_flush(); // sin esto, este paso intermedio nunca llegaría a verse

        y_center_prev = y_center;
        y_edge_prev = y_edge;

        sleep_ms(ANIM_STEP_DELAY_MS);
    }
}

/* Pantalla de "Opciones": de momento un marcador de posición.
 * Cualquier pulsación del botón de selección vuelve al menú. */
static void show_options_screen(void) {
    renderer_clear(COLOR_BLACK);
    renderer_draw_text(centered_x("OPCIONES", TITLE_SCALE), 60,
                        "OPCIONES", COLOR_CYAN, COLOR_BLACK, TITLE_SCALE);
    renderer_draw_text(centered_x("PROXIMAMENTE", PREV_NEXT_SCALE), 130,
                        "PROXIMAMENTE", COLOR_WHITE, COLOR_BLACK, PREV_NEXT_SCALE);
    renderer_flush();

    while (true) {
        controls_update();
        if (controls_menu_select()) break;
        sleep_ms(15);
    }
}

/*
 * Pantalla de récords del juego indicado, durante SCORES_DISPLAY_MS.
 * Devuelve true si se mostró el tiempo completo, false si el usuario
 * la canceló con cualquier input (en cuyo caso NO hay que continuar
 * con la demo -- el usuario ha tomado el control).
 */
static bool show_scores_screen(int game_index) {
    renderer_clear(COLOR_BLACK);
    highscores_draw(game_index, game_name_at(game_index), 30);
    renderer_flush();

    absolute_time_t start = get_absolute_time();
    while (absolute_time_diff_us(start, get_absolute_time()) < SCORES_DISPLAY_MS * 1000) {
        controls_update();
        if (controls_menu_up() || controls_menu_down() || controls_menu_select()) {
            return false; // cancelado por el usuario
        }
        sleep_ms(15);
    }
    return true;
}

/*
 * Récords -> demo para un juego, como una iteración del ciclo
 * automático. "OPCIONES" no tiene récords ni demo, así que no hace
 * nada para ese índice (el llamador simplemente avanza al siguiente).
 */
static void run_attract_cycle(int game_index) {
    if (game_index == OPTIONS_INDEX) return;

    if (show_scores_screen(game_index)) {
        games_list[game_index].run(ATTRACT_DEMO_MODE);
        highscores_flush(); // por si la demo llegó a guardar algún récord
    }
}

void menu_run(void) {
    int selected = 0;
    int idle_ms = 0;

    renderer_clear(COLOR_BLACK);
    draw_title();
    draw_settled(selected);
    renderer_flush();

    while (true) {
        // Una única lectura del PIO por vuelta del bucle, antes de
        // controls_menu_up()/controls_menu_down() (ver controls.c).
        controls_update();

        int direction = 0;
        int new_selected = selected;
        bool had_input = false;

        if (controls_menu_down()) {
            new_selected = wrap_index(selected + 1);
            direction = 1;
            had_input = true;
        }
        if (controls_menu_up()) {
            new_selected = wrap_index(selected - 1);
            direction = -1;
            had_input = true;
        }

        if (direction != 0) {
            animate_transition(selected, new_selected, direction);
            selected = new_selected;
            draw_settled(selected);
            renderer_flush();
        }

        if (controls_menu_select()) {
            had_input = true;

            if (selected == OPTIONS_INDEX) {
                show_options_screen();
            } else {
                // TODO: cuando exista el submenú 1P/2P/Demo, sustituir
                // este GAME_MODE_1P fijo por el modo que elija el jugador.
                games_list[selected].run(GAME_MODE_1P);
                highscores_flush();
            }

            // Al volver, redibuja todo desde cero.
            renderer_clear(COLOR_BLACK);
            draw_title();
            draw_settled(selected);
            renderer_flush();
        }

        if (had_input) {
            idle_ms = 0;
        } else {
            idle_ms += 15;
            if (idle_ms >= IDLE_TIMEOUT_MS) {
                idle_ms = 0;

                run_attract_cycle(selected);
                selected = wrap_index(selected + 1);

                // Tras la demo (u "OPCIONES"), redibuja el menú desde
                // cero con el siguiente juego ya seleccionado.
                renderer_clear(COLOR_BLACK);
                draw_title();
                draw_settled(selected);
                renderer_flush();
            }
        }

        update_title_animation();

        sleep_ms(15);
    }
}
