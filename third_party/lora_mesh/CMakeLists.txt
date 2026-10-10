# LoRa flooding mesh for CSC09 trail-mesh (Member 1)

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.1.1)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.1.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(lora_mesh C CXX ASM)

pico_sdk_init()

# Waveshare/Semtech LR1121 driver, copied unchanged from the Core1121 demo.
# Pins live in src/gpio/gpio.h, SPI port and clock in src/spi/spi.h and spi.c.
include_directories(src/gpio src/spi src/lr1121)
add_subdirectory(src/gpio)
add_subdirectory(src/spi)
add_subdirectory(src/lr1121)

# The LoRa mesh as a library, so the team firmware can link it directly
add_library(lora_mesh_lib STATIC
    lora/lora_radio.c
    lora/lora_mesh.c
)
target_include_directories(lora_mesh_lib PUBLIC lora mesh)
target_link_libraries(lora_mesh_lib PUBLIC pico_stdlib pico_rand lr1121 gpio spi)

# Test program
add_executable(lora_mesh main.c)

pico_set_program_name(lora_mesh "lora_mesh")
pico_set_program_version(lora_mesh "0.1")

pico_enable_stdio_uart(lora_mesh 0)
pico_enable_stdio_usb(lora_mesh 1)

target_link_libraries(lora_mesh pico_stdlib lora_mesh_lib)

pico_add_extra_outputs(lora_mesh)

# Wiring diagnostic (temporary): flash build/lora_diag.uf2 instead of lora_mesh.uf2
add_executable(lora_diag diag.c)
pico_enable_stdio_uart(lora_diag 0)
pico_enable_stdio_usb(lora_diag 1)
target_link_libraries(lora_diag pico_stdlib hardware_spi gpio spi)
pico_add_extra_outputs(lora_diag)
