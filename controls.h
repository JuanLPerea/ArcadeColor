#ifndef CONTROLS_H
#define CONTROLS_H

#include <stdbool.h>
#include <stdint.h>

/* Pines */
#define PIN_ENC1_CLK 20
#define PIN_ENC1_DT  21
#define PIN_ENC1_SW  16

#define PIN_ENC2_CLK 26
#define PIN_ENC2_DT  27
#define PIN_ENC2_SW  22

#define PIN_BTN_J1_A 0
#define PIN_BTN_J1_B 1
#define PIN_BTN_J2_A 2
#define PIN_BTN_J2_B 3

void controls_init(void);

/*
 * Lee ambos encoders UNA sola vez. Debe llamarse una vez por
 * iteración del bucle principal, ANTES de controls_menu_up()/
 * controls_menu_down().
 */
void controls_update(void);

bool controls_menu_up(void);
bool controls_menu_down(void);
bool controls_menu_select(void);

/*
 * Debug / diagnóstico: estado interno sin filtrar.
 * encoder_index: 0 = J1, 1 = J2.
 */
int32_t controls_debug_raw_count(int encoder_index);
int32_t controls_debug_pending(int encoder_index);

#endif
