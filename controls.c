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
 * transición real de cuadratura — el rebote se cuela dentro de Y
 * antes de que el software pueda hacer nada al respecto.
 *
 * Bajando esto a un valor razonable, el PIO deja de mirar los
 * pines constantemente y solo muestrea a esta frecuencia. Un
 * rebote típico se resuelve en 1-5 ms, y un giro humano —incluso
 * rápido— no genera más de un puñado de transiciones por segundo,
 * muy por debajo de esto, así que no se pierde ninguna transición
 * real. Es un filtrado en el origen, mucho más fiable que intentar
 * reconstruir el movimiento después de que el ruido ya ha entrado
 * en el contador.
 *
 * Si con esto la cuenta se ve "atascada", puede que tu encoder
 * rebote más de 1 ms: prueba a bajarlo a 500. Si notas transiciones
 * perdidas en giros muy rápidos (poco probable), súbelo a 2000.
 */
#define ENCODER_MAX_STEP_RATE 1000

/*
 * El PIO cuenta transiciones de cuadratura. Un detent completo
 * produce 4 transiciones limpias (un ciclo completo de cuadratura).
 * Ahora que el rebote se filtra en el propio PIO (ENCODER_MAX_STEP_RATE),
 * podemos volver a este valor, que es el físicamente correcto y el
 * más resistente al ruido residual: exige un movimiento neto
 * consistente antes de disparar un paso.
 */
#define ENCODER_TRANSITIONS_PER_STEP 4

/*
 * Con el filtrado ya hecho en el PIO, estas dos son solo una red de
 * seguridad adicional, no el filtro principal:
 *
 * 1) ENCODER_PENDING_CLAMP: evita que cualquier ruido residual
 *    "guarde" un acumulado grande que se descargue de golpe más
 *    tarde. Al ser mayor que el umbral, da además histéresis.
 *
 * 2) ENCODER_STEP_DEBOUNCE_US: tiempo mínimo entre dos pasos
 *    aceptados del mismo encoder. Bajo aposta, porque ya no tiene
 *    que cargar con todo el peso del filtrado.
 */
#define ENCODER_PENDING_CLAMP (ENCODER_TRANSITIONS_PER_STEP * 3)
#define ENCODER_STEP_DEBOUNCE_US 2000

static PIO encoder_pio = ENCODER_PIO;
static uint encoder_sm1 = ENCODER_SM1;
static uint encoder_sm2 = ENCODER_SM2;
static uint encoder_program_offset;

static int32_t enc1_last_count;
static int32_t enc2_last_count;
static int32_t enc1_pending;
static int32_t enc2_pending;
static absolute_time_t enc1_last_step_time;
static absolute_time_t enc2_last_step_time;

static void clamp_pending(int32_t *pending)
{
    if (*pending > ENCODER_PENDING_CLAMP) {
        *pending = ENCODER_PENDING_CLAMP;
    } else if (*pending < -ENCODER_PENDING_CLAMP) {
        *pending = -ENCODER_PENDING_CLAMP;
    }
}

/*
 * Lee ambos encoders UNA sola vez.
 *
 * Importante: no hacemos esto desde controls_menu_up() y
 * controls_menu_down() por separado, porque eso hacía dos lecturas
 * del PIO por vuelta del menú.
 *
 * menu.c debe llamar a esta función una vez por iteración del
 * bucle principal, ANTES de controls_menu_up()/controls_menu_down().
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
}

/*
 * Consume UN detent completo, con debounce por tiempo.
 *
 * IMPORTANTE: el debounce se comprueba ANTES de tocar `pending`.
 * Si lo hiciéramos al revés (restar primero y comprobar el tiempo
 * después), un giro real que llega antes de que se cumpla la
 * ventana de debounce se descartaría PERDIENDO ese movimiento en
 * vez de dejarlo esperando su turno — eso es lo que pasaba antes y
 * hacía que se "comiera" pasos en giros continuos.
 *
 * Si el usuario gira muy rápido, los pasos quedan acumulados en
 * `pending` (dentro del límite del clamp) y se van liberando uno
 * por ventana de debounce, sin perderse.
 */
static bool consume_step(int32_t *pending, int direction,
                          absolute_time_t *last_step_time)
{
    int64_t since_us =
        absolute_time_diff_us(*last_step_time, get_absolute_time());

    if (since_us < ENCODER_STEP_DEBOUNCE_US) {
        /* Aún no toca: el movimiento sigue acumulado en pending. */
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
 * Consume un evento "arriba" del acumulado de los encoders.
 *
 * IMPORTANTE: esta función YA NO lee el PIO. Debe llamarse
 * DESPUÉS de controls_update() en cada vuelta del bucle del menú.
 */
bool controls_menu_up(void)
{
    if (consume_step(&enc1_pending, +1, &enc1_last_step_time))
        return true;
    if (consume_step(&enc2_pending, +1, &enc2_last_step_time))
        return true;
    return false;
}

/*
 * Consume un evento "abajo" del acumulado de los encoders.
 *
 * IMPORTANTE: esta función tampoco lee el PIO.
 *
 * menu.c debe llamar primero a controls_update() una vez
 * por iteración, antes de controls_menu_up()/controls_menu_down().
 */
bool controls_menu_down(void)
{
    if (consume_step(&enc1_pending, -1, &enc1_last_step_time))
        return true;
    if (consume_step(&enc2_pending, -1, &enc2_last_step_time))
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

/* ---------------------------------------------------------
 * Debug / diagnóstico
 *
 * Estas funciones exponen el estado interno TAL CUAL, sin
 * ningún filtrado añadido. Solo para el modo de test de
 * encoders en menu.c; no las uses en la lógica normal del
 * menú.
 * --------------------------------------------------------- */
int32_t controls_debug_raw_count(int encoder_index)
{
    return (encoder_index == 0) ? enc1_last_count : enc2_last_count;
}

int32_t controls_debug_pending(int encoder_index)
{
    return (encoder_index == 0) ? enc1_pending : enc2_pending;
}
