#include "menu.h"
#include "renderer.h"
#include "controls.h"
#include "games/games_list.h"
#include "highscores.h"
#include "sound.h"
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
#define SELECTED_SCALE_MAX 4
#define TITLE_Y 8
#define DIVIDER_Y 36

#define CENTER_Y 120
#define ITEM_SPACING 50

#define ANIM_STEPS 5
#define ANIM_STEP_DELAY_MS 12

#define TOTAL_ITEMS (NUM_GAMES + 1)
#define OPTIONS_INDEX NUM_GAMES

#define IDLE_TIMEOUT_MS (30 * 1000)
#define SCORES_DISPLAY_MS (5 * 1000)
#define ATTRACT_DEMO_MODE GAME_MODE_DEMO

static const char *PROJECT_TITLE = "ARCADE COLOR";


static int wrap_index(int idx)
{
    idx %= TOTAL_ITEMS;

    if (idx < 0) {
        idx += TOTAL_ITEMS;
    }

    return idx;
}


static const char *game_name_at(int idx)
{
    idx = wrap_index(idx);

    if (idx == OPTIONS_INDEX) {
        return "OPCIONES";
    }

    return games_list[idx].name;
}


static int best_fit_scale(
    const char *text,
    int max_scale
)
{
    for (
        int scale = max_scale;
        scale > 1;
        scale--
    ) {
        if (
            st7789_text_width(
                text,
                (uint8_t)scale
            ) <= TFT_WIDTH - 10
        ) {
            return scale;
        }
    }

    return 1;
}


static int centered_x(
    const char *text,
    int scale
)
{
    int text_width =
        (int)st7789_text_width(
            text,
            (uint8_t)scale
        );

    int x =
        (TFT_WIDTH - text_width) / 2;

    return (x < 0) ? 0 : x;
}


static void clear_row(int y)
{
    int top_margin = 4;
    int bottom_margin = 6;

    int height =
        top_margin +
        FONT_HEIGHT * SELECTED_SCALE_MAX +
        bottom_margin;

    renderer_fill_rect(
        0,
        y - top_margin,
        TFT_WIDTH,
        height,
        COLOR_BLACK
    );
}


static void draw_row(
    int y,
    const char *text,
    int scale,
    uint16_t color
)
{
    renderer_draw_text(
        centered_x(text, scale),
        y,
        text,
        color,
        COLOR_BLACK,
        scale
    );
}


static void draw_title(void)
{
    renderer_fill_rect(
        0,
        0,
        TFT_WIDTH,
        DIVIDER_Y + 2,
        COLOR_BLACK
    );

    renderer_draw_text(
        centered_x(
            PROJECT_TITLE,
            TITLE_SCALE
        ),
        TITLE_Y,
        PROJECT_TITLE,
        COLOR_CYAN,
        COLOR_BLACK,
        TITLE_SCALE
    );

    renderer_fill_rect(
        10,
        DIVIDER_Y,
        TFT_WIDTH - 20,
        2,
        COLOR_CYAN
    );
}


#define TITLE_WAVE_AMPLITUDE 4
#define TITLE_WAVE_SPEED 0.28f
#define TITLE_WAVE_CHAR_PHASE 0.6f
#define TITLE_COLOR_CYCLE_EVERY 6
#define TITLE_ANIM_THROTTLE 3

static const uint16_t title_palette[] = {
    COLOR_CYAN,
    COLOR_YELLOW,
    COLOR_MAGENTA,
    COLOR_GREEN,
    COLOR_RED,
    COLOR_WHITE
};

#define TITLE_PALETTE_LEN \
    (int)(sizeof(title_palette) / sizeof(title_palette[0]))

#define TITLE_BAND_HEIGHT \
    (FONT_HEIGHT * TITLE_SCALE + \
     2 * TITLE_WAVE_AMPLITUDE + 4)

static float title_wave_phase = 0.0f;
static int title_color_phase = 0;


static void draw_title_wave(void)
{
    int text_w =
        (int)st7789_text_width(
            PROJECT_TITLE,
            TITLE_SCALE
        );

    int x =
        (TFT_WIDTH - text_w) / 2;

    if (x < 0) {
        x = 0;
    }

    int band_y =
        TITLE_Y -
        TITLE_WAVE_AMPLITUDE -
        2;

    renderer_fill_rect(
        0,
        band_y,
        TFT_WIDTH,
        TITLE_BAND_HEIGHT,
        COLOR_BLACK
    );

    for (
        int i = 0;
        PROJECT_TITLE[i] != '\0';
        i++
    ) {
        float angle =
            title_wave_phase +
            i * TITLE_WAVE_CHAR_PHASE;

        int y_offset =
            (int)(
                sinf(angle) *
                TITLE_WAVE_AMPLITUDE
            );

        uint16_t color =
            title_palette[
                (i + title_color_phase) %
                TITLE_PALETTE_LEN
            ];

        renderer_draw_char(
            x,
            TITLE_Y + y_offset,
            PROJECT_TITLE[i],
            color,
            COLOR_BLACK,
            TITLE_SCALE
        );

        x +=
            (FONT_WIDTH + 1) *
            TITLE_SCALE;
    }
}


static void update_title_animation(void)
{
    static int throttle_counter = 0;
    static int color_tick = 0;

    throttle_counter++;

    if (
        throttle_counter <
        TITLE_ANIM_THROTTLE
    ) {
        return;
    }

    throttle_counter = 0;

    title_wave_phase += TITLE_WAVE_SPEED;

    if (
        title_wave_phase >
        6.2831853f
    ) {
        title_wave_phase -= 6.2831853f;
    }

    color_tick++;

    if (
        color_tick >=
        TITLE_COLOR_CYCLE_EVERY
    ) {
        color_tick = 0;

        title_color_phase =
            (title_color_phase + 1) %
            TITLE_PALETTE_LEN;
    }

    draw_title_wave();
    renderer_flush();
}


static void draw_settled(int selected)
{
    clear_row(
        CENTER_Y - ITEM_SPACING
    );

    clear_row(CENTER_Y);

    clear_row(
        CENTER_Y + ITEM_SPACING
    );

    const char *name =
        game_name_at(selected);

    draw_row(
        CENTER_Y - ITEM_SPACING,
        game_name_at(selected - 1),
        PREV_NEXT_SCALE,
        COLOR_WHITE
    );

    draw_row(
        CENTER_Y,
        name,
        best_fit_scale(
            name,
            SELECTED_SCALE_MAX
        ),
        COLOR_YELLOW
    );

    draw_row(
        CENTER_Y + ITEM_SPACING,
        game_name_at(selected + 1),
        PREV_NEXT_SCALE,
        COLOR_WHITE
    );
}


static void animate_transition(
    int old_selected,
    int new_selected,
    int direction
)
{
    const char *slide_center =
        game_name_at(old_selected);

    const char *slide_edge =
        game_name_at(new_selected);

    int y_center_start = CENTER_Y;

    int y_center_end =
        CENTER_Y -
        direction * ITEM_SPACING;

    int y_edge_start =
        CENTER_Y +
        direction * ITEM_SPACING;

    int y_edge_end = CENTER_Y;

    int y_vacating =
        (direction > 0)
        ? (CENTER_Y - ITEM_SPACING)
        : (CENTER_Y + ITEM_SPACING);

    int y_revealing =
        (direction > 0)
        ? (CENTER_Y + ITEM_SPACING)
        : (CENTER_Y - ITEM_SPACING);

    clear_row(y_vacating);
    clear_row(y_revealing);

    int y_center_prev =
        y_center_start;

    int y_edge_prev =
        y_edge_start;

    for (
        int step = 1;
        step <= ANIM_STEPS;
        step++
    ) {
        int y_center =
            y_center_start +
            (y_center_end - y_center_start) *
            step /
            ANIM_STEPS;

        int y_edge =
            y_edge_start +
            (y_edge_end - y_edge_start) *
            step /
            ANIM_STEPS;

        clear_row(y_center_prev);
        clear_row(y_edge_prev);
        clear_row(y_center);
        clear_row(y_edge);

        draw_row(
            y_center,
            slide_center,
            PREV_NEXT_SCALE,
            COLOR_WHITE
        );

        draw_row(
            y_edge,
            slide_edge,
            PREV_NEXT_SCALE,
            COLOR_WHITE
        );

        renderer_flush();

        y_center_prev = y_center;
        y_edge_prev = y_edge;

        /*
         * La música sigue avanzando durante la animación.
         */
        sound_update();

        sleep_ms(ANIM_STEP_DELAY_MS);
    }
}


static void show_options_screen(void)
{
    renderer_clear(COLOR_BLACK);

    renderer_draw_text(
        centered_x(
            "OPCIONES",
            TITLE_SCALE
        ),
        60,
        "OPCIONES",
        COLOR_CYAN,
        COLOR_BLACK,
        TITLE_SCALE
    );

    renderer_draw_text(
        centered_x(
            "PROXIMAMENTE",
            PREV_NEXT_SCALE
        ),
        130,
        "PROXIMAMENTE",
        COLOR_WHITE,
        COLOR_BLACK,
        PREV_NEXT_SCALE
    );

    renderer_flush();

    while (true) {
        controls_update();

        if (controls_menu_select()) {
            break;
        }

        sound_update();

        sleep_ms(15);
    }
}


static bool show_scores_screen(
    int game_index
)
{
    renderer_clear(COLOR_BLACK);

    highscores_draw(
        game_index,
        game_name_at(game_index),
        30
    );

    renderer_flush();

    absolute_time_t start =
        get_absolute_time();

    while (
        absolute_time_diff_us(
            start,
            get_absolute_time()
        ) <
        SCORES_DISPLAY_MS * 1000
    ) {
        controls_update();

        if (
            controls_menu_up() ||
            controls_menu_down() ||
            controls_menu_select()
        ) {
            return false;
        }

        /*
         * La música no se reproduce durante
         * la pantalla de récords.
         */
        sleep_ms(15);
    }

    return true;
}


static void run_attract_cycle(
    int game_index
)
{
    if (game_index == OPTIONS_INDEX) {
        return;
    }

    /*
     * El attract mode se ejecuta sin música de menú.
     */
    sound_stop_menu_music();

    if (show_scores_screen(game_index)) {
        games_list[game_index].run(
            ATTRACT_DEMO_MODE
        );

        highscores_flush();
    }
}


void menu_run(void)
{
    int selected = 0;
    int idle_ms = 0;

    /*
     * Inicializamos e iniciamos la música
     * solamente cuando el menú está preparado.
     */
    sound_init();
    sound_start_menu_music();

    renderer_clear(COLOR_BLACK);

    draw_title();
    draw_settled(selected);

    renderer_flush();

    while (true) {

        /*
         * Actualización no bloqueante de la música.
         */
        sound_update();

        controls_update();

        int direction = 0;
        int new_selected = selected;
        bool had_input = false;

        if (controls_menu_down()) {
            new_selected =
                wrap_index(selected + 1);

            direction = 1;
            had_input = true;
        }

        if (controls_menu_up()) {
            new_selected =
                wrap_index(selected - 1);

            direction = -1;
            had_input = true;
        }

        if (direction != 0) {
            animate_transition(
                selected,
                new_selected,
                direction
            );

            selected = new_selected;

            draw_settled(selected);

            renderer_flush();
        }

        if (controls_menu_select()) {
            had_input = true;

            /*
             * La música debe parar antes de entrar
             * en cualquier pantalla/juego.
             */
            sound_stop_menu_music();

            if (selected == OPTIONS_INDEX) {

                show_options_screen();

            } else {

                games_list[selected].run(
                    GAME_MODE_1P
                );

                highscores_flush();
            }

            /*
             * Al regresar al menú, reiniciamos
             * la música y redibujamos todo.
             */
            sound_start_menu_music();

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

                selected =
                    wrap_index(selected + 1);

                /*
                 * El attract mode puede haber terminado
                 * dejando el sonido apagado, así que
                 * volvemos a arrancar la música.
                 */
                sound_start_menu_music();

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