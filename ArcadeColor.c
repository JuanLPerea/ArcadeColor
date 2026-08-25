#include "pico/stdlib.h"
#include "renderer.h"
#include "controls.h"
#include "menu.h"

int main(void) {
    stdio_init_all();

    renderer_init();
    controls_init();

    menu_run(); // no retorna en uso normal

    while (true) {
        tight_loop_contents();
    }
}
