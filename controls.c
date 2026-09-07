#include "controls.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "quadrature_encoder.pio.h"

#define ENCODER_PIO pio0
#define ENCODER_SM1 0
#define ENCODER_SM2 1

/*
 * Tasa de muestreo del PIO (ver quadrature_encoder_program_init).
 * Con 0 el PIO muestrea los pines a la velocidad máxima del reloj
 * del sistema (~125 MHz, cada ~80 ns), lo que hace que procese
 * CADA rebote individual del contacto mecánico como si fuera una
 * transición real de cuadratura -- el rebote se cuela dentro de Y
 * antes de que el software pueda hacer nada al respecto.
 *
 * Bajando esto a un valor razonable, el PIO deja de mirar los
 * pines constantemente y solo muestrea a esta frecuencia. Un
 * rebote típico se resuelve en 1-5 ms, y un giro humano -incluso
 * rápido- no genera más de un puñado de transiciones por segundo,
 * muy por debajo de esto, así que no se pierde ninguna transición
 * real.
 */
#define ENCODER_MAX_STEP_RATE 1000

/*
 * El PIO cuenta transiciones de cuadratura. Un detent completo
 * produce 4 transiciones limpias (un ciclo completo de cuadratura).
 */
#define ENCODER_TRANSITIONS_PER_STEP 4

#define ENCODER_PENDING_CLAMP (ENCODER_TRANSITIONS_PER_STEP * 3)
#define ENCODER_STEP_DEBOUNCE_US 2000

static PIO encoder_pio = ENCODER_PIO;
static uint encoder_sm1 = ENCODER_SM1;
static uint encoder_sm2 = ENCODER_SM2;
static uint encoder_program_offset;

static int32_t enc1_last_count;
static int32_t enc2_last_count;

/* Acumulado usado SOLO por controls_menu_up()/down() (navegación de
 * menú, con umbral+debounce). Ver controls_get_raw_delta() más abajo
 * para el movimiento continuo sin filtrar que necesitan los juegos. */
static int32_t enc1_pending;
static int32_t enc2_pending;
static absolute_time_t enc1_last_step_time;
static absolute_time_t enc2_last_step_time;

/* Última cuenta cruda consumida por controls_get_raw_delta(), en un
 * acumulador SEPARADO del de arriba -- así un juego leyendo deltas
 * continuos no interfiere con la navegación del menú, y viceversa. */
static int32_t enc1_last_raw_delta_count;
static int32_t enc2_last_raw_delta_count;

static void clamp_pending(int32_t *pending)
{
    if (*pending > ENCODER_PENDING_CLAMP) {
        *pending = ENCODER_PENDING_CLAMP;
    } else if (*pending < -ENCODER_PENDING_CLAMP) {
        *pending = -ENCODER_PENDING_CLAMP;
    }
}

/* ---------------------------------------------------------
 * Botones
 *
 * button_pins[] es la tabla canónica: su índice ES el identificador
 * de botón (ver los BTN_* en controls.h, que son índices en esta
 * misma tabla). Definido ANTES de controls_update() porque este
 * último llama a la función de escaneo de botones de aquí abajo.
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
/* true durante el frame exacto en que cada botón pasó a pulsado
 * (flanco), recalculado en cada controls_update(). */
static bool button_pressed_this_frame[NUM_BUTTONS];

/* Escaneo de los 6 botones con debounce, una vez por controls_update(). */
static void update_buttons(void)
{
    for (int i = 0; i < (int)NUM_BUTTONS; i++) {
        bool pressed = !gpio_get(button_pins[i]);
        button_pressed_this_frame[i] = false;

        if (pressed && !button_prev_pressed[i]) {
            int64_t since_us =
                absolute_time_diff_us(
                    button_last_change[i],
                    get_absolute_time()
                );

            if (since_us > DEBOUNCE_US) {
                button_prev_pressed[i] = true;
                button_last_change[i] = get_absolute_time();
                button_pressed_this_frame[i] = true;
            }
        } else if (!pressed) {
            button_prev_pressed[i] = false;
        }
    }
}

/*
 * Lee ambos encoders Y el estado de los botones UNA sola vez.
 *
 * Debe llamarse una vez por iteración del bucle principal, antes de
 * controls_menu_up()/down(), controls_button_pressed() o
 * controls_get_raw_delta().
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

    clamp_pending(&enc1_pending);
    clamp_pending(&enc2_pending);

    update_buttons();
}

/*
 * Consume UN detent completo, con debounce por tiempo. El debounce
 * se comprueba ANTES de tocar "pending" -- si lo hiciéramos al
 * revés, un giro real que llega antes de la ventana de debounce se
 * perdería en vez de quedar esperando su turno.
 */
static bool consume_step(int32_t *pending, int direction,
                          absolute_time_t *last_step_time)
{
    int64_t since_us =
        absolute_time_diff_us(*last_step_time, get_absolute_time());

    if (since_us < ENCODER_STEP_DEBOUNCE_US) {
        return false;
    }

    if (direction > 0) {
        if (*pending >= ENCODER_TRANSITIONS_PER_STEP) {
            *pending -= ENCODER_TRANSITIONS_PER_STEP;
            *last_step_time = get_absolute_time();
            return true;
        }
    } else {
        if (*pending <= -ENCODER_TRANSITIONS_PER_STEP) {
            *pending += ENCODER_TRANSITIONS_PER_STEP;
            *last_step_time = get_absolute_time();
            return true;
        }
    }

    return false;
}

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
        ENCODER_MAX_STEP_RATE
    );

    quadrature_encoder_program_init(
        encoder_pio,
        encoder_sm2,
        PIN_ENC2_CLK,
        ENCODER_MAX_STEP_RATE
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
    enc1_last_step_time = get_absolute_time();
    enc2_last_step_time = get_absolute_time();

    enc1_last_raw_delta_count = enc1_last_count;
    enc2_last_raw_delta_count = enc2_last_count;

    /* -----------------------------------------------------
     * Botones
     * ----------------------------------------------------- */
    for (int i = 0; i < (int)NUM_BUTTONS; i++) {
        gpio_init(button_pins[i]);
        gpio_set_dir(button_pins[i], GPIO_IN);
        gpio_pull_up(button_pins[i]);
        button_prev_pressed[i] = false;
        button_pressed_this_frame[i] = false;
        button_last_change[i] = get_absolute_time();
    }
}

bool controls_menu_up(void)
{
    if (consume_step(&enc1_pending, +1, &enc1_last_step_time))
        return true;
    if (consume_step(&enc2_pending, +1, &enc2_last_step_time))
        return true;
    return false;
}

bool controls_menu_down(void)
{
    if (consume_step(&enc1_pending, -1, &enc1_last_step_time))
        return true;
    if (consume_step(&enc2_pending, -1, &enc2_last_step_time))
        return true;
    return false;
}

/* Cualquiera de los 6 botones, agregados en un solo evento -- lo que
 * ya usaba el menú. Construido sobre el estado que ya calculó
 * update_buttons() dentro de controls_update(). */
bool controls_menu_select(void)
{
    for (int i = 0; i < (int)NUM_BUTTONS; i++) {
        if (button_pressed_this_frame[i]) return true;
    }
    return false;
}

/* ¿Se pulsó este botón (flanco) en el último controls_update()?
 * btn: uno de los BTN_* de controls.h. */
bool controls_button_pressed(int btn)
{
    if (btn < 0 || btn >= (int)NUM_BUTTONS) return false;
    return button_pressed_this_frame[btn];
}

/* ¿Está este botón mantenido pulsado ahora mismo (nivel, no flanco)? */
bool controls_button_down(int btn)
{
    if (btn < 0 || btn >= (int)NUM_BUTTONS) return false;
    return button_prev_pressed[btn];
}

/*
 * Movimiento continuo SIN FILTRAR del encoder, en transiciones de
 * cuadratura crudas (4 = un detent físico), desde la última llamada.
 * Pensado para control analógico-continuo dentro de un juego (p.ej.
 * mover una pala con inercia), NO para navegación de menú -- usa un
 * acumulador propio, separado del de controls_menu_up()/down(), así
 * que ambos sistemas pueden coexistir sin pisarse.
 *
 * encoder_index: 0 = J1, 1 = J2.
 */
int32_t controls_get_raw_delta(int encoder_index)
{
    int32_t current = (encoder_index == 0) ? enc1_last_count : enc2_last_count;
    int32_t *last = (encoder_index == 0) ? &enc1_last_raw_delta_count
                                          : &enc2_last_raw_delta_count;
    int32_t delta = *last - current;
    *last = current;
    return delta;
}

/* ---------------------------------------------------------
 * Debug / diagnóstico
 *
 * Estas funciones exponen el estado interno TAL CUAL, sin
 * ningún filtrado añadido. Solo para el modo de test de
 * encoders en menu.c; no las uses en la lógica normal del
 * menú ni de los juegos.
 * --------------------------------------------------------- */
int32_t controls_debug_raw_count(int encoder_index)
{
    return (encoder_index == 0) ? enc1_last_count : enc2_last_count;
}

int32_t controls_debug_pending(int encoder_index)
{
    return (encoder_index == 0) ? enc1_pending : enc2_pending;
}
