cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(PICO_PLATFORM rp2040)

include(pico_sdk_import.cmake)

set(PROJECT PM2040_GK)
project(${PROJECT} C CXX ASM)

pico_sdk_init()

add_compile_options(-O2 -Wall)

# For boards with crystals which take a bit longer to stabilize.
add_compile_definitions(PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

add_executable(${PROJECT} main.c gk.c)

# Same memory map as GB mini, plus a section holding the emulator core: it
# is stored in flash and copied to RAM by main.c once the cart bus is up,
# keeping the boot path (crt0 copy/zero) as short as GB mini's.
pico_set_linker_script(${PROJECT} ${CMAKE_CURRENT_SOURCE_DIR}/memmap.ld)

target_compile_definitions(${PROJECT} PRIVATE GK_PICO=1)

pico_generate_pio_header(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/oe.pio)
pico_generate_pio_header(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/pushData.pio)
pico_generate_pio_header(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/hale.pio)
pico_generate_pio_header(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/lale.pio)
pico_generate_pio_header(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/writecheck.pio)
pico_generate_pio_header(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/writecheck_addr.pio)

pico_enable_stdio_usb(${PROJECT} 0)
pico_enable_stdio_uart(${PROJECT} 0)

pico_add_extra_outputs(${PROJECT})

target_link_libraries(${PROJECT} pico_stdlib pico_multicore hardware_pio hardware_dma hardware_flash)
