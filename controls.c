#include "controls.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "quadrature_encoder.pio.h"

// ---------------------------------------------------------
// Rotary encoders via PIO.
//
// Encoder 1: GP20 = A/CLK, GP21 = B/DT
// Encoder 2: GP26 = A/CLK, GP27 = B/DT
//
// The PIO state machines maintain an independent 32-bit quadrature
// count without using GPIO interrupts or CPU time for every edge.
// ---------------------------------------------------------
#define ENCODER_PIO PIO0
#define ENCODER_SM1 0
#define ENCODER_SM2 1

// The official PIO quadrature decoder counts every valid transition.
// A typical mechanical encoder produces four transitions per detent.
#define ENCODER_TRANSITIONS_PER_STEP 4

static PIO encoder_pio = ENCODER_PIO;
static uint encoder_sm1 = ENCODER_SM1;
static uint encoder_sm2 = ENCODER_SM2;
static uint encoder_program_offset;

static int32_t enc1_last_count = 0;
static int32_t enc2_last_count = 0;
static int32_t enc1_raw_pending = 0;
static int32_t enc2_raw_pending = 0;

static void update_encoder_counts(void) {
    int32_t count1 = quadrature_encoder_get_count(encoder_pio, encoder_sm1);
    int32_t count2 = quadrature_encoder_get_count(encoder_pio, encoder_sm2);

    // Signed subtraction is intentional: the PIO counter wraps as a
    // 32-bit two's-complement value, so this also works across wraparound.
    // Invert the PIO sign so the menu keeps the same physical direction
    // as the previous GPIO-IRQ implementation (falling A + B=0 was +1).
    enc1_raw_pending += enc1_last_count - count1;
    enc2_raw_pending += enc2_last_count - count2;

    enc1_last_count = count1;
    enc2_last_count = count2;
}

static bool consume_encoder_step(volatile int32_t *pending, int direction) {
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

// ---------------------------------------------------------
// Buttons: 4 action buttons plus the integrated push switches
// of both encoders.
// ---------------------------------------------------------
static const uint8_t button_pins[] = {
    PIN_BTN_J1_A, PIN_BTN_J1_B, PIN_BTN_J2_A, PIN_BTN_J2_B,
    PIN_ENC1_SW,  PIN_ENC2_SW
};
#define NUM_BUTTONS (sizeof(button_pins) / sizeof(button_pins[0]))

#define DEBOUNCE_US 30000 // 30 ms

static bool button_prev_pressed[NUM_BUTTONS];
static absolute_time_t button_last_change[NUM_BUTTONS];

void controls_init(void) {
    // -----------------------------------------------------
    // Encoders: PIO owns A/B. No GPIO IRQs are installed.
    // -----------------------------------------------------
    encoder_program_offset = pio_add_program(encoder_pio, &quadrature_encoder_program);

    // The program uses computed jumps, so it must be loaded at offset 0.
    // This project currently uses no other PIO program, so that is expected.
    if (encoder_program_offset != 0) {
        panic("quadrature_encoder must be loaded at PIO offset 0");
    }

    quadrature_encoder_program_init(encoder_pio, encoder_sm1, PIN_ENC1_CLK, 0);
    quadrature_encoder_program_init(encoder_pio, encoder_sm2, PIN_ENC2_CLK, 0);

    // Establish a clean baseline after the state machines have started.
    enc1_last_count = quadrature_encoder_get_count(encoder_pio, encoder_sm1);
    enc2_last_count = quadrature_encoder_get_count(encoder_pio, encoder_sm2);
    enc1_raw_pending = 0;
    enc2_raw_pending = 0;

    // -----------------------------------------------------
    // Buttons: active-low with internal pull-ups.
    // -----------------------------------------------------
    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_init(button_pins[i]);
        gpio_set_dir(button_pins[i], GPIO_IN);
        gpio_pull_up(button_pins[i]);
        button_prev_pressed[i] = false;
        button_last_change[i] = get_absolute_time();
    }
}

bool controls_menu_up(void) {
    update_encoder_counts();

    if (consume_encoder_step(&enc1_raw_pending, +1)) return true;
    if (consume_encoder_step(&enc2_raw_pending, +1)) return true;
    return false;
}

bool controls_menu_down(void) {
    update_encoder_counts();

    if (consume_encoder_step(&enc1_raw_pending, -1)) return true;
    if (consume_encoder_step(&enc2_raw_pending, -1)) return true;
    return false;
}

bool controls_menu_select(void) {
    for (int i = 0; i < NUM_BUTTONS; i++) {
        bool pressed = !gpio_get(button_pins[i]); // active low

        if (pressed && !button_prev_pressed[i]) {
            int64_t since_us = absolute_time_diff_us(
                button_last_change[i], get_absolute_time());

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
