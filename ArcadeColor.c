#include "pico/stdlib.h"
#include "renderer.h"
#include "controls.h"
#include "highscores.h"
#include "sound.h"
#include "menu.h"


int main(void)
{
    stdio_init_all();

    renderer_init();
    controls_init();
    highscores_init();
    sound_init();

    menu_run();

    while (true) {
        tight_loop_contents();
    }
}