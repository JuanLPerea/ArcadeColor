#include "controls.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "quadrature_encoder.pio.h"

#define ENCODER_PIO PIO0
#define ENCODER_SM1 0
#define ENCODER_SM2 1

/*
 * El PIO cuenta transiciones de cuadratura.
 * Un detent completo suele producir 4 transiciones.
 *
 * Guardamos el movimiento pendiente y lo convertimos en un evento
 * solamente cuando se alcanza un detent completo.
 */
#define ENCODER_TRANSITIONS_PER_STEP 4

static PIO encoder_pio = ENCODER_PIO;
static uint encoder_sm1 = ENCODER_SM1;
static uint encoder_sm2 = ENCODER_SM2;
static uint encoder_program_offset;

static int32_t enc1_last_count;
static int32_t enc2_last_count;

static int32_t enc1_pending;
static int32_t enc2_pending;

/*
 * Lee ambos encoders UNA sola vez.
 *
 * Importante: no hacemos esto desde controls_menu_up() y
 * controls_menu_down() por separado, porque eso hacía dos lecturas
 * del PIO por vuelta del menú.
 */
void controls_update(void)
{
    int32_t count1 =
        quadrature_encoder_get_count(encoder_pio, encoder_sm1);

    int32_t count2 =
        quadrature_encoder_get_count(encoder_pio, encoder_sm2);

    enc1_pending += enc1_last_count - count1;
    enc2_pending += enc2_last_count - count2;

    enc1_last_count = count1;
    enc2_last_count = count2;
}

/*
 * Consume UN detent completo.
 *
 * Si el usuario gira muy rápido, los pasos permanecen acumulados
 * y no se pierden.
 */
static bool consume_step(int32_t *pending, int direction)
{
    if (direction > 0) {
        if (*pending >= ENCODER_TRANSITIONS_PER_STEP) {
            *pending -= ENCODER_TRANSITIONS_PER_STEP;
            return true;
        }
    } else {
        if (*pending <= -ENCODER_TRANSITIONS_PER_STEP) {
            *pending += ENCODER_TRANSITIONS_PER_STEP;
            return true;
        }
    }

    return false;
}


/* ---------------------------------------------------------
 * Botones
 * --------------------------------------------------------- */

static const uint8_t button_pins[] = {
    PIN_BTN_J1_A,
    PIN_BTN_J1_B,
    PIN_BTN_J2_A,
    PIN_BTN_J2_B,
    PIN_ENC1_SW,
    PIN_ENC2_SW
};

#define NUM_BUTTONS (sizeof(button_pins) / sizeof(button_pins[0]))
#define DEBOUNCE_US 30000

static bool button_prev_pressed[NUM_BUTTONS];
static absolute_time_t button_last_change[NUM_BUTTONS];


void controls_init(void)
{
    /* -----------------------------------------------------
     * Encoders
     * ----------------------------------------------------- */

    encoder_program_offset =
        pio_add_program(
            encoder_pio,
            &quadrature_encoder_program
        );

    if (encoder_program_offset != 0) {
        panic("quadrature_encoder must be loaded at PIO offset 0");
    }

    quadrature_encoder_program_init(
        encoder_pio,
        encoder_sm1,
        PIN_ENC1_CLK,
        0
    );

    quadrature_encoder_program_init(
        encoder_pio,
        encoder_sm2,
        PIN_ENC2_CLK,
        0
    );

    enc1_last_count =
        quadrature_encoder_get_count(
            encoder_pio,
            encoder_sm1
        );

    enc2_last_count =
        quadrature_encoder_get_count(
            encoder_pio,
            encoder_sm2
        );

    enc1_pending = 0;
    enc2_pending = 0;


    /* -----------------------------------------------------
     * Botones
     * ----------------------------------------------------- */

    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_init(button_pins[i]);
        gpio_set_dir(button_pins[i], GPIO_IN);
        gpio_pull_up(button_pins[i]);

        button_prev_pressed[i] = false;
        button_last_change[i] = get_absolute_time();
    }
}


/*
 * Lee el encoder una sola vez y genera eventos.
 *
 * Como ambos encoders controlan el mismo menú, primero acumulamos
 * los dos y después consumimos solamente un evento.
 */
bool controls_menu_up(void)
{
    update_encoder_counts();

    if (consume_step(&enc1_pending, +1))
        return true;

    if (consume_step(&enc2_pending, +1))
        return true;

    return false;
}


bool controls_menu_down(void)
{
    /*
     * Esta función ya no debe volver a actualizar el PIO.
     *
     * IMPORTANTE:
     * menu.c debe llamar primero a controls_update() una vez
     * por iteración.
     */

    if (consume_step(&enc1_pending, -1))
        return true;

    if (consume_step(&enc2_pending, -1))
        return true;

    return false;
}


bool controls_menu_select(void)
{
    for (int i = 0; i < NUM_BUTTONS; i++) {

        bool pressed = !gpio_get(button_pins[i]);

        if (pressed && !button_prev_pressed[i]) {

            int64_t since_us =
                absolute_time_diff_us(
                    button_last_change[i],
                    get_absolute_time()
                );

            if (since_us > DEBOUNCE_US) {

                button_prev_pressed[i] = true;
                button_last_change[i] = get_absolute_time();

                return true;
            }

        } else if (!pressed) {

            button_prev_pressed[i] = false;
        }
    }

    return false;
}