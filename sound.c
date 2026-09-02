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
 *
 * CANAL1/CANAL2 subidos tras corregir wave_triangle(): con el fallo
 * de escala, la triangular saturaba casi de inmediato y sonaba en
 * la práctica como una cuadrada a plena amplitud (más alta). Ya
 * corregida, es una rampa lineal de verdad, con una amplitud media
 * ~42% menor que una cuadrada del mismo pico (RMS triangular/RMS
 * cuadrada = 1/√3 ≈ 0.577) -- así que con el MISMO volumen sonaba
 * más floja que antes. Estos valores compensan esa diferencia.
 *
 * Es al gusto: súbelos más si los quieres más fuertes (el limitador
 * de la mezcla ya protege contra desbordamiento, aunque a volúmenes
 * muy altos con los 3 canales sonando a la vez notarás algo más de
 * saturación/recorte -- en música chiptune de arcade normalmente no
 * es un problema, hasta le da carácter).
 */
#define CHANNEL1_VOLUME 110
#define CHANNEL2_VOLUME 80
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
 * GREENSLEEVES
 *
 * Adaptación de la partitura:
 * - Tonalidad: Mi menor
 * - Compás: 6/4
 * - Extensión: 16 compases
 * - Adaptada a sintetizador de 2 canales
 * ============================================================ */

#define MUSIC_NOTE_COUNT 75

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

#define NOTE_GS4 415
#define NOTE_CS5 554
#define NOTE_FS5 740
#define NOTE_DS4 311


/*
 * Canal 1: Melodía principal
 *
 * Greensleeves
 * Tonalidad: Mi menor
 * Compás: 6/4
 *
 * Las notas siguen la línea melódica de la guitarra
 * de la partitura original.
 */
static const uint16_t melody_notes[MUSIC_NOTE_COUNT] = {

    // ========================================================
    // COMPÁS 1
    // ========================================================
    NOTE_E4, NOTE_G4, NOTE_A4, NOTE_B4,
    NOTE_C5, NOTE_B4,

    // ========================================================
    // COMPÁS 2
    // ========================================================
    NOTE_A4, NOTE_F4, NOTE_D4,
    NOTE_E4, NOTE_F4,

    // ========================================================
    // COMPÁS 3
    // ========================================================
    NOTE_G4, NOTE_F4, NOTE_E4,
    NOTE_DS4, NOTE_E4,

    // ========================================================
    // COMPÁS 4
    // ========================================================
    NOTE_F4, NOTE_DS4, NOTE_B3,
    NOTE_E4,

    // ========================================================
    // COMPÁS 5
    // ========================================================
    NOTE_E4, NOTE_G4, NOTE_A4, NOTE_B4,
    NOTE_C5, NOTE_B4,

    // ========================================================
    // COMPÁS 6
    // ========================================================
    NOTE_A4, NOTE_F4, NOTE_D4,
    NOTE_E4, NOTE_F4,

    // ========================================================
    // COMPÁS 7
    // ========================================================
    NOTE_G4, NOTE_F4, NOTE_E4,
    NOTE_DS4, NOTE_E4,

    // ========================================================
    // COMPÁS 8
    // ========================================================
    NOTE_F4, NOTE_E4, NOTE_D4,
    0,

    // ========================================================
    // COMPÁS 9
    // ========================================================
    NOTE_D5, NOTE_D5, NOTE_C5, NOTE_B4,

    // ========================================================
    // COMPÁS 10
    // ========================================================
    NOTE_A4, NOTE_F4, NOTE_D4,
    NOTE_E4, NOTE_F4,

    // ========================================================
    // COMPÁS 11
    // ========================================================
    NOTE_G4, NOTE_E4, NOTE_E4,
    NOTE_DS4, NOTE_E4,

    // ========================================================
    // COMPÁS 12
    // ========================================================
    NOTE_F4, NOTE_DS4, NOTE_B3,
    0,

    // ========================================================
    // COMPÁS 13
    // ========================================================
    NOTE_D5, NOTE_D5, NOTE_C5, NOTE_B4,

    // ========================================================
    // COMPÁS 14
    // ========================================================
    NOTE_A4, NOTE_F4, NOTE_D4,
    NOTE_E4, NOTE_F4,

    // ========================================================
    // COMPÁS 15
    // ========================================================
    NOTE_G4, NOTE_F4, NOTE_E4,
    NOTE_DS4, NOTE_E4,

    // ========================================================
    // COMPÁS 16 - FINAL
    // ========================================================
    NOTE_E4, NOTE_E4
};


/*
 * Duraciones de la melodía (ms)
 *
 * Referencia:
 *
 * 150 ms = corchea
 * 300 ms = negra
 * 450 ms = negra con puntillo
 * 600 ms = blanca
 * 900 ms = blanca con puntillo
 *
 * El último compás se alarga para simular el ritardando.
 */
static const uint16_t melody_duration[MUSIC_NOTE_COUNT] = {

    // ========================================================
    // COMPÁS 1
    // ========================================================
    600, 300, 300, 450,
    150, 300,

    // ========================================================
    // COMPÁS 2
    // ========================================================
    600, 300, 450,
    150, 300,

    // ========================================================
    // COMPÁS 3
    // ========================================================
    600, 300, 450,
    150, 300,

    // ========================================================
    // COMPÁS 4
    // ========================================================
    600, 300, 600,
    300,

    // ========================================================
    // COMPÁS 5
    // ========================================================
    600, 300, 300, 450,
    150, 300,

    // ========================================================
    // COMPÁS 6
    // ========================================================
    600, 300, 450,
    150, 300,

    // ========================================================
    // COMPÁS 7
    // ========================================================
    450, 150, 300,
    450, 150, 300,

    // ========================================================
    // COMPÁS 8
    // ========================================================
    900, 600,
    300,

    // ========================================================
    // COMPÁS 9
    // ========================================================
    900, 450, 150, 300,

    // ========================================================
    // COMPÁS 10
    // ========================================================
    600, 300, 450,
    150, 300,

    // ========================================================
    // COMPÁS 11
    // ========================================================
    600, 300, 450,
    150, 300,

    // ========================================================
    // COMPÁS 12
    // ========================================================
    600, 300, 600,
    300,

    // ========================================================
    // COMPÁS 13
    // ========================================================
    900, 450, 150, 300,

    // ========================================================
    // COMPÁS 14
    // ========================================================
    600, 300, 450,
    150, 300,

    // ========================================================
    // COMPÁS 15
    // ========================================================
    450, 150, 300,
    450, 150, 300,

    // ========================================================
    // COMPÁS 16 - RITARDANDO / FINAL
    // ========================================================
    900, 1400
};


/*
 * Canal 2: Acompañamiento / Bajo
 *
 * Simplificado a partir de la armonía del piano.
 *
 * Se mantiene deliberadamente sencillo para que el canal
 * de melodía sea el protagonista.
 */
static const uint16_t bass_notes[MUSIC_NOTE_COUNT] = {

    // ========================================================
    // COMPÁS 1 - Em
    // ========================================================
    NOTE_E3, 0,       NOTE_B3, 0,
    NOTE_E3, 0,

    // ========================================================
    // COMPÁS 2 - D / Am
    // ========================================================
    NOTE_A3, 0,       NOTE_D3,
    0,       NOTE_A3,

    // ========================================================
    // COMPÁS 3 - Em
    // ========================================================
    NOTE_E3, 0,       NOTE_B3,
    0,       NOTE_E3,

    // ========================================================
    // COMPÁS 4 - B7 / Em
    // ========================================================
    NOTE_B3, 0,       NOTE_B3,
    NOTE_E3,

    // ========================================================
    // COMPÁS 5 - Em
    // ========================================================
    NOTE_E3, 0,       NOTE_B3, 0,
    NOTE_E3, 0,

    // ========================================================
    // COMPÁS 6 - D / Am
    // ========================================================
    NOTE_A3, 0,       NOTE_D3,
    0,       NOTE_A3,

    // ========================================================
    // COMPÁS 7 - Em / B7
    // ========================================================
    NOTE_E3, 0,       NOTE_B3,
    0,       NOTE_B3, 0,

    // ========================================================
    // COMPÁS 8 - Em
    // ========================================================
    NOTE_E3, 0,
    0,

    // ========================================================
    // COMPÁS 9 - G
    // ========================================================
    NOTE_G3, 0,       NOTE_D4, NOTE_G3,

    // ========================================================
    // COMPÁS 10 - D
    // ========================================================
    NOTE_D3, 0,       NOTE_A3,
    0,       NOTE_D3,

    // ========================================================
    // COMPÁS 11 - Em
    // ========================================================
    NOTE_E3, 0,       NOTE_B3,
    0,       NOTE_E3,

    // ========================================================
    // COMPÁS 12 - B7
    // ========================================================
    NOTE_B3, 0,       NOTE_B3,
    0,

    // ========================================================
    // COMPÁS 13 - G
    // ========================================================
    NOTE_G3, 0,       NOTE_D4, NOTE_G3,

    // ========================================================
    // COMPÁS 14 - D
    // ========================================================
    NOTE_D3, 0,       NOTE_A3,
    0,       NOTE_D3,

    // ========================================================
    // COMPÁS 15 - B7
    // ========================================================
    NOTE_B3, 0,       NOTE_B3,
    0,       NOTE_E3, 0,

    // ========================================================
    // COMPÁS 16 - Em / FINAL
    // ========================================================
    NOTE_E3, 0
};

/* ============================================================
 * ESTADO DE LA MÚSICA
 * ============================================================ */

static volatile bool menu_music_playing = false;

static volatile uint music_index = 0;

static absolute_time_t music_next_change;

/*
 * Sirena del platillo volante (Space Invaders y lo que la necesite):
 * un silbido continuo de dos tonos que se alternan, típico del OVNI
 * clásico. Usa el CANAL 2 -- libre durante la partida, ya que la
 * música de menú (canales 1+2) se para en cuanto empieza a jugarse
 * (sound_stop_menu_music()) -- así no interfiere con los efectos de
 * disparo/explosión del canal 3, que siguen sonando encima sin
 * cortar la sirena.
 */
static volatile bool channel2_siren_active = false;
static uint8_t channel2_siren_phase = 0;
static absolute_time_t channel2_siren_next_change;

#define SIREN_FREQ_LOW   600
#define SIREN_FREQ_HIGH  900
#define SIREN_STEP_MS     90
#define SIREN_VOLUME      40   // más bajo que los efectos, para que no los tape

/*
 * Expiración del tono genérico de sound_play_tone() (canal 3).
 * Antes, duration_ms se ignoraba por completo y el tono se quedaba
 * sonando indefinidamente. Se comprueba en sound_update(), que ya
 * se llama periódicamente para avanzar la música.
 */
static volatile bool channel3_tone_has_expiry = false;
static absolute_time_t channel3_tone_expiry;

/*
 * Declaraciones adelantadas: configure_channel() y disable_channel()
 * se definen más abajo en este mismo archivo (junto al resto de la
 * mezcla de audio), pero el secuenciador de aquí las necesita antes.
 * Sin esto, el compilador las trata como declaradas implícitamente
 * (con tipo de retorno "int" por defecto), y luego choca con la
 * definición real más abajo -- error de "static declaration follows
 * non-static declaration".
 */
static void configure_channel(volatile audio_channel_t *channel, uint16_t frequency, uint16_t volume, uint8_t waveform);
static void disable_channel(volatile audio_channel_t *channel);

/*
 * Secuenciador simple de canal 3, para melodías cortas no
 * bloqueantes (2-4 notas): "pierdes la bola" descendente, "victoria"
 * ascendente. Igual que channel3_tone_has_expiry, se avanza desde
 * sound_update(). También usado internamente por los sound_effect_*
 * de una sola nota, como apagado automático -- antes NINGUNO de
 * ellos paraba el canal 3 por su cuenta, así que en Pong el tono del
 * último rebote se quedaba sonando sostenido hasta el siguiente, en
 * vez de ser un "beep" corto. Con secuencia de 1 sola nota, es
 * exactamente ese apagado automático.
 */
#define CHANNEL3_SEQ_MAX_NOTES 4

typedef struct {
    uint16_t freqs[CHANNEL3_SEQ_MAX_NOTES];
    uint16_t durations_ms[CHANNEL3_SEQ_MAX_NOTES];
    uint8_t  count;
    uint8_t  index;
    bool     active;
    uint16_t volume;
    uint8_t  waveform;
    absolute_time_t next_change;
} Channel3Sequence;

static volatile Channel3Sequence channel3_seq = { .active = false };

static void channel3_seq_start(
    const uint16_t *freqs,
    const uint16_t *durations_ms,
    uint8_t count,
    uint16_t volume,
    uint8_t waveform
)
{
    if (count > CHANNEL3_SEQ_MAX_NOTES) {
        count = CHANNEL3_SEQ_MAX_NOTES;
    }

    for (uint8_t i = 0; i < count; i++) {
        channel3_seq.freqs[i] = freqs[i];
        channel3_seq.durations_ms[i] = durations_ms[i];
    }

    channel3_seq.count    = count;
    channel3_seq.index    = 0;
    channel3_seq.volume   = volume;
    channel3_seq.waveform = waveform;
    channel3_seq.active   = true;

    // La secuencia y el temporizador de "un solo tono" comparten el
    // canal 3 -- se excluyen mutuamente, solo uno de los dos manda
    // en cada momento.
    channel3_tone_has_expiry = false;

    configure_channel(&channel3, channel3_seq.freqs[0], volume, waveform);
    channel3_seq.next_change = make_timeout_time_ms(channel3_seq.durations_ms[0]);
}

static void channel3_seq_update(void)
{
    if (!channel3_seq.active) {
        return;
    }
    if (!time_reached(channel3_seq.next_change)) {
        return;
    }

    channel3_seq.index++;

    if (channel3_seq.index >= channel3_seq.count) {
        channel3_seq.active = false;
        disable_channel(&channel3);
        return;
    }

    configure_channel(
        &channel3,
        channel3_seq.freqs[channel3_seq.index],
        channel3_seq.volume,
        channel3_seq.waveform
    );
    channel3_seq.next_change =
        make_timeout_time_ms(channel3_seq.durations_ms[channel3_seq.index]);
}

/* Efecto corto de una sola nota, con apagado automático -- usada por
 * todos los sound_effect_* de una nota (shoot/explosion/select/move/
 * game_over/success). Es channel3_seq_start() con una única nota. */
static void channel3_play_short(
    uint16_t freq,
    uint16_t volume,
    uint8_t waveform,
    uint16_t duration_ms
)
{
    uint16_t freqs[1]     = { freq };
    uint16_t durations[1] = { duration_ms };
    channel3_seq_start(freqs, durations, 1, volume, waveform);
}


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
 *
 * CORREGIDO: protegida con save_and_disable_interrupts(), como ya
 * hacían sound_start_menu_music()/sound_update(). Antes, si el timer
 * de audio (que lee estos mismos campos ~22050 veces/segundo)
 * interrumpía justo a mitad de esta función, podía leer una muestra
 * con una mezcla de valores viejos/nuevos (p.ej. la forma de onda ya
 * actualizada pero el phase_increment todavía el antiguo) -- un
 * "clic" audible puntual al reconfigurar un canal que ya sonaba
 * (típicamente al retriggerar un efecto). Con esto, cada llamada a
 * configure_channel() (incluyendo todos los sound_effect_*, que
 * antes no tenían ninguna protección) queda atómica frente al timer.
 * Nota: save_and_disable_interrupts()/restore_interrupts() se
 * pueden anidar sin problema, así que esto no rompe nada en
 * sound_start_menu_music()/sound_update(), que ya envuelven sus
 * propias llamadas a esta función en su propia protección.
 */
static void configure_channel(
    volatile audio_channel_t *channel,
    uint16_t frequency,
    uint16_t volume,
    uint8_t waveform
)
{
    uint32_t save =
        save_and_disable_interrupts();

    if (frequency == 0) {

        channel->active = false;
        channel->frequency = 0;
        channel->phase_increment = 0;

        restore_interrupts(save);
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

    restore_interrupts(save);
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
 *
 * CORREGIDO: el factor de escala original (*4) hacía que el valor
 * saliera del rango [-127,127] casi de inmediato (en los primeros
 * ~16 valores de p de 32768 posibles), así que el clamp final
 * saturaba la señal enseguida -- sonaba prácticamente como una
 * cuadrada, no como una rampa suave. El factor correcto para una
 * rampa lineal de verdad sobre todo el semiperiodo es 254/32768,
 * no 4.
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
            -127 +
            (((int32_t)p * 254) / 32768);
    } else {
        value =
            127 -
            ((((int32_t)p - 32768) * 254) / 32768);
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
     * CORREGIDO: antes duration_ms se ignoraba por completo (el
     * tono sonaba indefinidamente) y había una llamada duplicada a
     * configure_channel() que no hacía nada distinto de la primera.
     * Ahora se guarda cuándo debe apagarse y sound_update() lo
     * comprueba en cada vuelta.
     *
     * También cancela cualquier secuencia de canal3_seq que
     * estuviera sonando -- comparten el canal 3, así que si no se
     * cancela, el siguiente sound_update() podría pisar este tono
     * con la nota que tocara de la secuencia.
     */
    channel3_seq.active = false;

    configure_channel(
        &channel3,
        frequency_hz,
        CHANNEL3_VOLUME,
        WAVE_SQUARE
    );

    if (duration_ms > 0) {
        channel3_tone_has_expiry = true;
        channel3_tone_expiry =
            make_timeout_time_ms(duration_ms);
    } else {
        channel3_tone_has_expiry = false;
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

    // La sirena del platillo también usa el canal 2 -- si por lo que
    // sea estuviera sonando, la música de menú manda.
    channel2_siren_active = false;


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
    /*
     * Auto-apagado del tono genérico de sound_play_tone() (ver
     * comentario junto a channel3_tone_has_expiry más arriba).
     */
    if (
        channel3_tone_has_expiry &&
        time_reached(channel3_tone_expiry)
    ) {
        channel3_tone_has_expiry = false;
        disable_channel(&channel3);
    }

    // Avanza la melodía corta de canal 3 (efectos de una nota con
    // apagado automático, o las secuencias de 2-4 notas).
    channel3_seq_update();

    // Avanza el silbido de la sirena del platillo (canal 2).
    if (
        channel2_siren_active &&
        time_reached(channel2_siren_next_change)
    ) {
        channel2_siren_phase ^= 1;
        configure_channel(
            &channel2,
            channel2_siren_phase ? SIREN_FREQ_HIGH : SIREN_FREQ_LOW,
            SIREN_VOLUME,
            WAVE_TRIANGLE
        );
        channel2_siren_next_change = make_timeout_time_ms(SIREN_STEP_MS);
    }

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
 * Disparo: beep corto y agudo.
 *
 * CORREGIDO: antes se dejaba sonando indefinidamente (ningún
 * sound_effect_* tenía apagado automático -- ver el comentario largo
 * junto a channel3_seq arriba). Ahora dura CHANNEL3_SFX_MS y se para
 * sola.
 */
#define CHANNEL3_SFX_MS 55   // duración de los efectos cortos de una nota

void sound_effect_shoot(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_play_short(1100, CHANNEL3_VOLUME, WAVE_SQUARE, CHANNEL3_SFX_MS);
}


/*
 * Explosión: ráfaga corta de ruido blanco.
 *
 * CORREGIDO: mismo apagado automático.
 */
void sound_effect_explosion(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_play_short(100, CHANNEL3_VOLUME, WAVE_NOISE, 150);
}


/*
 * Selección del menú.
 *
 * CORREGIDO: mismo apagado automático.
 */
void sound_effect_select(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_play_short(880, CHANNEL3_VOLUME, WAVE_SQUARE, CHANNEL3_SFX_MS);
}


/*
 * Movimiento del selector / rebote en pared.
 *
 * CORREGIDO: mismo apagado automático. Tono más grave que
 * sound_effect_shoot(), para que se distingan sin mirar la pantalla
 * (p.ej. rebote en pared vs. rebote en pala en Pong).
 */
void sound_effect_move(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_play_short(660, CHANNEL3_VOLUME, WAVE_SQUARE, CHANNEL3_SFX_MS);
}


/*
 * Game over: un solo tono grave y algo más largo -- distinto de
 * sound_effect_lose_point() (que es la secuencia descendente de
 * "has perdido la bola/punto", más corta y repetible muchas veces
 * por partida).
 *
 * CORREGIDO: mismo apagado automático.
 */
void sound_effect_game_over(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_play_short(180, CHANNEL3_VOLUME, WAVE_SAW, 400);
}


/*
 * Éxito puntual (p.ej. subida de nivel) -- una sola nota alegre,
 * corta. Para una victoria de partida completa usa
 * sound_effect_victory() en su lugar (secuencia de 3 notas).
 *
 * CORREGIDO: mismo apagado automático.
 */
void sound_effect_success(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_play_short(1047, CHANNEL3_VOLUME, WAVE_TRIANGLE, 150);
}


/*
 * Pierdes la bola / el punto: 4 notas descendentes, cortas.
 * Nuevo efecto -- no existía en el sistema original.
 */
void sound_effect_lose_point(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    static const uint16_t freqs[4]     = { 392, 330, 262, 196 }; // sol-mi-do-sol descendente
    static const uint16_t durations[4] = {  70,  70,  70, 140 };

    channel3_seq_start(freqs, durations, 4, CHANNEL3_VOLUME, WAVE_SQUARE);
}


/*
 * Victoria de partida: fanfarria corta de 3 notas ascendentes.
 * Nuevo efecto -- no existía en el sistema original. Distinto de
 * sound_effect_success() (una sola nota, para eventos menores como
 * subir de nivel).
 */
void sound_effect_victory(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    static const uint16_t freqs[3]     = { 523, 659, 784 }; // do-mi-sol ascendente
    static const uint16_t durations[3] = { 110, 110, 260 };

    channel3_seq_start(freqs, durations, 3, CHANNEL3_VOLUME, WAVE_SQUARE);
}


/*
 * Apaga solamente el canal de efectos.
 *
 * La música continúa.
 */
void sound_effect_stop(void)
{
    channel3_seq.active = false;
    channel3_tone_has_expiry = false;

    disable_channel(
        &channel3
    );
}


/*
 * Sirena del platillo volante: silbido continuo de dos tonos
 * alternando (canal 2), hasta llamar a sound_siren_stop(). Pensada
 * para arrancar cuando el platillo aparece en pantalla y pararla
 * cuando se destruye o sale de pantalla.
 */
void sound_siren_start(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel2_siren_active = true;
    channel2_siren_phase  = 0;

    configure_channel(
        &channel2,
        SIREN_FREQ_LOW,
        SIREN_VOLUME,
        WAVE_TRIANGLE
    );

    channel2_siren_next_change = make_timeout_time_ms(SIREN_STEP_MS);
}

void sound_siren_stop(void)
{
    channel2_siren_active = false;

    disable_channel(
        &channel2
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