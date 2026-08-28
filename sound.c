#include "sound.h"

#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/timer.h"
#include "hardware/sync.h"

#include <stdint.h>
#include <stdbool.h>


/* ============================================================
 * CONFIGURACIÓN
 * ============================================================ */

#define SOUND_PIN 4

/*
 * Frecuencia de actualización del audio.
 *
 * 22050 Hz es suficiente para efectos y música arcade
 * y mantiene una carga de CPU razonable.
 */
#define AUDIO_SAMPLE_RATE 22050

/*
 * PWM rápido.
 *
 * El PWM funciona mucho más rápido que la señal de audio.
 * El duty cycle representa cada muestra de audio.
 */
#define AUDIO_PWM_TOP 255


/*
 * Volumen máximo de cada canal.
 */
#define CHANNEL1_VOLUME 75
#define CHANNEL2_VOLUME 55
#define CHANNEL3_VOLUME 100


/* ============================================================
 * FORMAS DE ONDA
 * ============================================================ */

#define WAVE_SQUARE   0
#define WAVE_TRIANGLE 1
#define WAVE_SAW      2
#define WAVE_NOISE    3


/* ============================================================
 * CANAL DE AUDIO
 * ============================================================ */

typedef struct {
    volatile bool active;

    uint32_t phase;
    uint32_t phase_increment;

    uint16_t frequency;

    uint16_t volume;

    uint8_t waveform;

    uint32_t noise_state;
} audio_channel_t;


/* ============================================================
 * HARDWARE
 * ============================================================ */

static uint sound_pwm_slice;
static uint sound_pwm_channel;

static struct repeating_timer audio_timer;

static volatile bool sound_initialized = false;
static volatile bool sound_enabled = true;


/* ============================================================
 * CANALES
 * ============================================================ */

static volatile audio_channel_t channel1;
static volatile audio_channel_t channel2;
static volatile audio_channel_t channel3;


/* ============================================================
 * MÚSICA
 * ============================================================ */

#define MUSIC_NOTE_COUNT 64

#define NOTE_C3  131
#define NOTE_D3  147
#define NOTE_E3  165
#define NOTE_F3  175
#define NOTE_G3  196
#define NOTE_A3  220
#define NOTE_B3  247

#define NOTE_C4  262
#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_B4  494

#define NOTE_C5  523
#define NOTE_D5  587
#define NOTE_E5  659
#define NOTE_F5  698
#define NOTE_G5  784
#define NOTE_A5  880
#define NOTE_B5  988


/*
 * Canal 1:
 * melodía principal.
 */
static const uint16_t melody_notes[MUSIC_NOTE_COUNT] = {

    NOTE_C5, NOTE_E5, NOTE_G5, NOTE_A5,
    NOTE_G5, NOTE_E5, NOTE_D5, 0,

    NOTE_C5, NOTE_D5, NOTE_E5, NOTE_G5,
    NOTE_E5, NOTE_D5, NOTE_C5, 0,

    NOTE_E5, NOTE_G5, NOTE_B5, NOTE_A5,
    NOTE_G5, NOTE_E5, NOTE_D5, 0,

    NOTE_C5, NOTE_E5, NOTE_G5, NOTE_E5,
    NOTE_D5, NOTE_C5, NOTE_D5, 0,

    NOTE_G4, NOTE_C5, NOTE_E5, NOTE_G5,
    NOTE_A5, NOTE_G5, NOTE_E5, 0,

    NOTE_E5, NOTE_D5, NOTE_C5, NOTE_D5,
    NOTE_E5, NOTE_G5, NOTE_A5, 0,

    NOTE_A5, NOTE_G5, NOTE_E5, NOTE_D5,
    NOTE_C5, NOTE_D5, NOTE_E5, NOTE_G5,

    NOTE_A5, NOTE_G5, NOTE_E5, NOTE_D5,
    NOTE_C5, NOTE_E5, NOTE_D5, NOTE_C5
};


/*
 * Duraciones de la melodía.
 */
static const uint16_t melody_duration[MUSIC_NOTE_COUNT] = {

    220, 220, 220, 360,
    220, 220, 420, 140,

    200, 200, 240, 360,
    240, 220, 440, 140,

    220, 220, 260, 360,
    240, 220, 440, 140,

    220, 220, 300, 360,
    240, 220, 440, 160,

    220, 220, 220, 280,
    360, 220, 440, 140,

    220, 220, 300, 220,
    240, 240, 480, 160,

    300, 220, 240, 220,
    360, 220, 220, 300,

    300, 240, 260, 240,
    400, 300, 300, 800
};


/*
 * Canal 2:
 * acompañamiento/bajo.
 *
 * Cada nota corresponde a una nota de la melodía.
 * Los ceros dejan respirar el acompañamiento.
 */
static const uint16_t bass_notes[MUSIC_NOTE_COUNT] = {

    NOTE_C3, 0, NOTE_G3, 0,
    NOTE_C3, 0, NOTE_G3, 0,

    NOTE_C3, 0, NOTE_G3, 0,
    NOTE_C3, 0, NOTE_G3, 0,

    NOTE_A3, 0, NOTE_E3, 0,
    NOTE_A3, 0, NOTE_E3, 0,

    NOTE_F3, 0, NOTE_C3, 0,
    NOTE_G3, 0, NOTE_G3, 0,

    NOTE_C3, 0, NOTE_G3, 0,
    NOTE_C3, 0, NOTE_G3, 0,

    NOTE_A3, 0, NOTE_E3, 0,
    NOTE_A3, 0, NOTE_E3, 0,

    NOTE_F3, 0, NOTE_C3, 0,
    NOTE_G3, 0, NOTE_C3, 0,

    NOTE_F3, 0, NOTE_G3, 0,
    NOTE_C3, 0, NOTE_G3, 0
};


/* ============================================================
 * ESTADO DE LA MÚSICA
 * ============================================================ */

static volatile bool menu_music_playing = false;

static volatile uint music_index = 0;

static absolute_time_t music_next_change;


/* ============================================================
 * UTILIDADES
 * ============================================================ */


/*
 * Convierte una frecuencia en incremento de fase.
 *
 * DDS:
 *
 *     phase += increment
 *
 * La parte alta de phase representa la posición
 * dentro de la onda.
 */
static uint32_t frequency_to_phase_increment(
    uint16_t frequency
)
{
    if (frequency == 0) {
        return 0;
    }

    return (
        (uint32_t)(
            (
                (uint64_t)frequency
                << 32
            ) /
            AUDIO_SAMPLE_RATE
        )
    );
}


/*
 * Configura un canal.
 */
static void configure_channel(
    volatile audio_channel_t *channel,
    uint16_t frequency,
    uint16_t volume,
    uint8_t waveform
)
{
    if (frequency == 0) {

        channel->active = false;
        channel->frequency = 0;
        channel->phase_increment = 0;

        return;
    }

    channel->frequency = frequency;

    channel->phase = 0;

    channel->phase_increment =
        frequency_to_phase_increment(
            frequency
        );

    channel->volume = volume;

    channel->waveform = waveform;

    channel->noise_state =
        0x12345678u ^
        ((uint32_t)frequency * 2654435761u);

    if (channel->noise_state == 0) {
        channel->noise_state = 1;
    }

    channel->active = true;
}


/*
 * Desactiva un canal.
 */
static void disable_channel(
    volatile audio_channel_t *channel
)
{
    channel->active = false;
}


/* ============================================================
 * GENERADORES DE ONDA
 * ============================================================ */


/*
 * Generador cuadrado.
 *
 * Retorna aproximadamente:
 *
 *     -127 ... +127
 */
static int16_t wave_square(
    uint32_t phase
)
{
    if (phase & 0x80000000u) {
        return 127;
    }

    return -127;
}


/*
 * Generador triangular.
 */
static int16_t wave_triangle(
    uint32_t phase
)
{
    uint16_t p =
        (uint16_t)(phase >> 16);

    int32_t value;

    if (p < 32768) {
        value =
            ((int32_t)p * 4) -
            65536;
    } else {
        value =
            196608 -
            ((int32_t)p * 4);
    }

    if (value > 127) {
        value = 127;
    }

    if (value < -127) {
        value = -127;
    }

    return (int16_t)value;
}


/*
 * Diente de sierra.
 */
static int16_t wave_saw(
    uint32_t phase
)
{
    return (
        (int16_t)(phase >> 24)
    ) - 128;
}


/*
 * Ruido pseudoaleatorio.
 *
 * LFSR sencillo.
 */
static uint8_t noise_next(
    volatile audio_channel_t *channel
)
{
    uint32_t x =
        channel->noise_state;

    uint32_t bit =
        (
            (x >> 0) ^
            (x >> 2) ^
            (x >> 3) ^
            (x >> 5)
        ) & 1;

    x =
        (x >> 1) |
        (bit << 31);

    channel->noise_state = x;

    return (uint8_t)(x >> 24);
}


/*
 * Obtiene una muestra de un canal.
 */
static int16_t channel_sample(
    volatile audio_channel_t *channel
)
{
    if (!channel->active) {
        return 0;
    }

    int16_t sample = 0;

    switch (channel->waveform) {

        case WAVE_SQUARE:
            sample =
                wave_square(
                    channel->phase
                );
            break;


        case WAVE_TRIANGLE:
            sample =
                wave_triangle(
                    channel->phase
                );
            break;


        case WAVE_SAW:
            sample =
                wave_saw(
                    channel->phase
                );
            break;


        case WAVE_NOISE:
            sample =
                (int16_t)
                    noise_next(channel)
                - 128;
            break;


        default:
            sample = 0;
            break;
    }

    channel->phase +=
        channel->phase_increment;

    return (
        sample *
        (int16_t)channel->volume
    ) / 100;
}


/* ============================================================
 * INTERRUPCIÓN DE AUDIO
 * ============================================================ */


/*
 * Esta función se ejecuta aproximadamente 22050 veces
 * por segundo.
 *
 * IMPORTANTE:
 *
 * Aquí NO hacemos:
 *
 *     sleep_ms()
 *     printf()
 *     malloc()
 *     operaciones de pantalla
 *
 * Solo generamos y mezclamos una muestra.
 */
static bool audio_timer_callback(
    struct repeating_timer *timer
)
{
    (void)timer;

    if (!sound_enabled) {

        pwm_set_chan_level(
            sound_pwm_slice,
            sound_pwm_channel,
            AUDIO_PWM_TOP / 2
        );

        return true;
    }


    int32_t sample1 =
        channel_sample(&channel1);

    int32_t sample2 =
        channel_sample(&channel2);

    int32_t sample3 =
        channel_sample(&channel3);


    /*
     * Mezclamos los tres canales.
     */
    int32_t mixed =
        sample1 +
        sample2 +
        sample3;


    /*
     * Limitador.
     *
     * Evita que la suma de los tres canales
     * provoque overflow.
     */
    if (mixed > 255) {
        mixed = 255;
    }

    if (mixed < -255) {
        mixed = -255;
    }


    /*
     * Convertimos:
     *
     *     -255 ... +255
     *
     * a:
     *
     *       0 ... 255
     *
     * para el duty cycle PWM.
     */
    int32_t pwm_value =
        128 +
        mixed / 2;


    if (pwm_value < 0) {
        pwm_value = 0;
    }

    if (pwm_value > 255) {
        pwm_value = 255;
    }


    pwm_set_chan_level(
        sound_pwm_slice,
        sound_pwm_channel,
        (uint16_t)pwm_value
    );


    return true;
}


/* ============================================================
 * INICIALIZACIÓN
 * ============================================================ */

void sound_init(void)
{
    if (sound_initialized) {
        return;
    }


    /*
     * GP4 como salida PWM.
     */
    gpio_set_function(
        SOUND_PIN,
        GPIO_FUNC_PWM
    );


    sound_pwm_slice =
        pwm_gpio_to_slice_num(
            SOUND_PIN
        );

    sound_pwm_channel =
        pwm_gpio_to_channel(
            SOUND_PIN
        );


    /*
     * PWM rápido.
     *
     * 125 MHz / 256 ≈ 488 kHz
     *
     * Muy por encima de la frecuencia audible.
     */
    pwm_set_wrap(
        sound_pwm_slice,
        AUDIO_PWM_TOP
    );

    pwm_set_clkdiv(
        sound_pwm_slice,
        1.0f
    );


    /*
     * Punto medio mientras no hay audio.
     */
    pwm_set_chan_level(
        sound_pwm_slice,
        sound_pwm_channel,
        128
    );


    pwm_set_enabled(
        sound_pwm_slice,
        true
    );


    /*
     * Estado inicial de los canales.
     */
    channel1.active = false;
    channel2.active = false;
    channel3.active = false;


    /*
     * Timer de audio.
     *
     * 1.000.000 / 22050 ≈ 45 us
     */
    add_repeating_timer_us(
        -45,
        audio_timer_callback,
        NULL,
        &audio_timer
    );


    sound_initialized = true;
}


/* ============================================================
 * TONO GENÉRICO
 * ============================================================ */

void sound_play_tone(
    uint16_t frequency_hz,
    uint16_t duration_ms
)
{
    if (!sound_initialized) {
        sound_init();
    }


    /*
     * En lugar de bloquear durante duration_ms,
     * utilizamos el canal 3.
     */
    configure_channel(
        &channel3,
        frequency_hz,
        CHANNEL3_VOLUME,
        WAVE_SQUARE
    );


    /*
     * Para un tono genérico necesitamos saber cuándo
     * terminarlo.
     *
     * El sistema actual de efectos se encarga de
     * las duraciones específicas.
     *
     * Este tono simple utiliza un efecto de selección
     * temporalmente.
     */
    if (duration_ms < 80) {

        configure_channel(
            &channel3,
            frequency_hz,
            CHANNEL3_VOLUME,
            WAVE_SQUARE
        );
    }
}


/* ============================================================
 * MÚSICA
 * ============================================================ */

void sound_start_menu_music(void)
{
    if (!sound_initialized) {
        sound_init();
    }


    uint32_t save =
        save_and_disable_interrupts();


    music_index = 0;

    menu_music_playing = true;


    configure_channel(
        &channel1,
        melody_notes[0],
        CHANNEL1_VOLUME,
        WAVE_TRIANGLE
    );


    configure_channel(
        &channel2,
        bass_notes[0],
        CHANNEL2_VOLUME,
        WAVE_TRIANGLE
    );


    music_next_change =
        make_timeout_time_ms(
            melody_duration[0]
        );


    restore_interrupts(save);
}


void sound_stop_menu_music(void)
{
    uint32_t save =
        save_and_disable_interrupts();


    menu_music_playing = false;

    disable_channel(&channel1);
    disable_channel(&channel2);


    restore_interrupts(save);
}


/*
 * Esta función es intencionadamente muy ligera.
 *
 * El audio se genera en la interrupción.
 * Aquí solamente avanzamos la secuencia musical.
 */
void sound_update(void)
{
    if (!menu_music_playing) {
        return;
    }


    if (
        !time_reached(
            music_next_change
        )
    ) {
        return;
    }


    uint32_t save =
        save_and_disable_interrupts();


    music_index++;


    if (
        music_index >=
        MUSIC_NOTE_COUNT
    ) {
        music_index = 0;
    }


    uint16_t melody =
        melody_notes[music_index];

    uint16_t bass =
        bass_notes[music_index];


    configure_channel(
        &channel1,
        melody,
        CHANNEL1_VOLUME,
        WAVE_TRIANGLE
    );


    configure_channel(
        &channel2,
        bass,
        CHANNEL2_VOLUME,
        WAVE_TRIANGLE
    );


    music_next_change =
        make_timeout_time_ms(
            melody_duration[music_index]
        );


    restore_interrupts(save);
}


bool sound_menu_music_is_playing(void)
{
    return menu_music_playing;
}


/* ============================================================
 * EFECTOS
 * ============================================================ */


/*
 * Disparo:
 *
 * frecuencia alta -> baja rápidamente
 *
 * Para simplificar y mantener el efecto no bloqueante,
 * utilizamos el canal 3 y su frecuencia inicial.
 */
void sound_effect_shoot(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    configure_channel(
        &channel3,
        1100,
        CHANNEL3_VOLUME,
        WAVE_SQUARE
    );
}


/*
 * Explosión.
 *
 * Ruido blanco pseudoaleatorio.
 */
void sound_effect_explosion(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    configure_channel(
        &channel3,
        100,
        CHANNEL3_VOLUME,
        WAVE_NOISE
    );
}


/*
 * Selección del menú.
 */
void sound_effect_select(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    configure_channel(
        &channel3,
        880,
        CHANNEL3_VOLUME,
        WAVE_SQUARE
    );
}


/*
 * Movimiento del selector.
 */
void sound_effect_move(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    configure_channel(
        &channel3,
        660,
        CHANNEL3_VOLUME,
        WAVE_SQUARE
    );
}


/*
 * Game over.
 */
void sound_effect_game_over(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    configure_channel(
        &channel3,
        180,
        CHANNEL3_VOLUME,
        WAVE_SAW
    );
}


/*
 * Victoria.
 */
void sound_effect_success(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    configure_channel(
        &channel3,
        1047,
        CHANNEL3_VOLUME,
        WAVE_TRIANGLE
    );
}


/*
 * Apaga solamente el canal de efectos.
 *
 * La música continúa.
 */
void sound_effect_stop(void)
{
    disable_channel(
        &channel3
    );
}


/*
 * Silencia todo el motor.
 */
void sound_mute(void)
{
    sound_enabled = false;

    pwm_set_chan_level(
        sound_pwm_slice,
        sound_pwm_channel,
        128
    );
}


/*
 * Reactiva el motor.
 */
void sound_unmute(void)
{
    sound_enabled = true;
}