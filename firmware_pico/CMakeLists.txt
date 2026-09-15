cmake_minimum_required(VERSION 3.13)

# Include Pico SDK
include(pico_sdk_import.cmake)

project(thermal_pico_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# Add firmware executable
add_executable(thermal_pico_firmware
    main.c
)

# Generate header from ws2812.pio
pico_generate_pio_header(thermal_pico_firmware ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)

# Link hardware libraries
target_link_libraries(thermal_pico_firmware
    pico_stdlib
    hardware_pio
    hardware_uart
)

# Enable USB serial output for stdio debugging
pico_enable_stdio_usb(thermal_pico_firmware 1)
pico_enable_stdio_uart(thermal_pico_firmware 0)

# Create uf2, bin, and hex firmware images
pico_add_extra_outputs(thermal_pico_firmware)
