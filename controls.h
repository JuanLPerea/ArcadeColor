#ifndef CONTROLS_H
#define CONTROLS_H

#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------
// Pines según la tabla de conexionado del proyecto
// ---------------------------------------------------------
#define PIN_ENC1_CLK 20   // Encoder 1 (Jugador 1) - Fase A
#define PIN_ENC1_DT  21   // Encoder 1 (Jugador 1) - Fase B
#define PIN_ENC1_SW  16   // Encoder 1 (Jugador 1) - Botón integrado

#define PIN_ENC2_CLK 26   // Encoder 2 (Jugador 2) - Fase A
#define PIN_ENC2_DT  27   // Encoder 2 (Jugador 2) - Fase B
#define PIN_ENC2_SW  22   // Encoder 2 (Jugador 2) - Botón integrado

#define PIN_BTN_J1_A 0    // Botón A Jugador 1
#define PIN_BTN_J1_B 1    // Botón B Jugador 1
#define PIN_BTN_J2_A 2    // Botón A Jugador 2
#define PIN_BTN_J2_B 3    // Botón B Jugador 2

// Inicializa GPIOs, pull-ups e interrupciones de los encoders.
// Llamar una vez al arrancar, antes de usar el resto de funciones.
void controls_init(void);

// --- API de nivel "menú": cualquiera de los 2 encoders mueve la
// selección, y cualquiera de los 4 botones (o los pulsadores
// integrados de los encoders) selecciona. Cada función consume el
// evento al leerlo, devolviendo true una única vez por paso/pulsación.
// Pensadas para llamarse una vez por iteración del bucle del menú.
bool controls_menu_up(void);
bool controls_menu_down(void);
bool controls_menu_select(void);

#endif
