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

/*
 * Identificadores de botón para controls_button_pressed()/down().
 * Coinciden con el índice de cada pin dentro de button_pins[] en
 * controls.c -- si cambias el orden ahí, cambia esto también.
 */
#define BTN_J1_A    0
#define BTN_J1_B    1
#define BTN_J2_A    2
#define BTN_J2_B    3
#define BTN_ENC1_SW 4
#define BTN_ENC2_SW 5

void controls_init(void);

/*
 * Lee ambos encoders y el estado de los 6 botones UNA sola vez. Debe
 * llamarse una vez por iteración del bucle principal, ANTES de
 * controls_menu_up()/down(), controls_button_pressed()/down() o
 * controls_get_raw_delta().
 */
void controls_update(void);

/* --- Navegación de menú: eventos "de un paso", con umbral y
 * debounce por tiempo (ver controls.c). --- */
bool controls_menu_up(void);
bool controls_menu_down(void);
bool controls_menu_select(void); /* cualquiera de los 6 botones, agregados */

/* --- API para juegos --- */

/* ¿Se pulsó este botón (flanco, con debounce) en el último
 * controls_update()? btn: uno de los BTN_* de arriba. */
bool controls_button_pressed(int btn);

/* ¿Está este botón mantenido pulsado ahora mismo (nivel, no flanco)? */
bool controls_button_down(int btn);

/*
 * Movimiento continuo SIN FILTRAR del encoder, en transiciones de
 * cuadratura crudas (4 = un detent físico), desde la última llamada.
 * Para control analógico-continuo (p.ej. mover una pala con
 * inercia) -- NO tiene umbral ni debounce, y usa un acumulador
 * separado del de controls_menu_up()/down(), así que no interfiere
 * con la navegación del menú.
 * encoder_index: 0 = J1, 1 = J2.
 */
int32_t controls_get_raw_delta(int encoder_index);

/*
 * Debug / diagnóstico: estado interno sin filtrar. encoder_index:
 * 0 = J1, 1 = J2. Solo para el modo de test de encoders en menu.c.
 */
int32_t controls_debug_raw_count(int encoder_index);
int32_t controls_debug_pending(int encoder_index);

#endif
