# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(ArcadeColor C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(ArcadeColor
        ArcadeColor.c
        st7789.c
        renderer.c
        controls.c
        menu.c
        records.c
        demo.c
        sound.c
        games/games_list.c
        games/pong.c
        games/space_invaders.c
        games/asteroids.c
        games/breakout.c
        games/defender.c
        games/grantrak.c
        games/lunar_lander.c
        games/night_driver.c
        games/pacman.c
        games/paratrooper.c
        games/pinball.c
        games/tetris.c
        )

# Generate the C header from the PIO quadrature decoder.
pico_generate_pio_header(ArcadeColor ${CMAKE_CURRENT_LIST_DIR}/quadrature_encoder.pio)

pico_set_program_name(ArcadeColor "ArcadeColor")
pico_set_program_version(ArcadeColor "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(ArcadeColor 0)
pico_enable_stdio_usb(ArcadeColor 0)

# Add the standard library to the build
target_link_libraries(ArcadeColor
        pico_stdlib
        hardware_spi
        hardware_pio)

# Add the standard include files to the build
target_include_directories(ArcadeColor PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# Add any user requested libraries
target_link_libraries(ArcadeColor 
        hardware_spi
        m
        )

pico_add_extra_outputs(ArcadeColor)

