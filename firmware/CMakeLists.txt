# Pico firmware for CSC09 trail mesh nodes (all five members' code in one image).
#
#   export PICO_SDK_PATH=.../pico-sdk            (2.1.1)
#   export FREERTOS_KERNEL_PATH=.../FreeRTOS-Kernel
#   export FATFS_PICO_PATH=.../no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
#   cmake -S firmware -B build-fw && cmake --build build-fw -j
#   (tools/fetch_deps.sh clones all three at the versions this was built with)
#
# Outputs in build-fw/ (drag the .uf2 onto the Pico in BOOTSEL mode):
#   csc09_node_1.uf2 ... csc09_node_5.uf2   the real node firmware, one per node (1 = A = bridge)
#   sensor_test.uf2      sensors only (PIR, sound, mic, DHT22, light), no radios, no SD
#   audio_dump.uf2       raw 16 kHz mic recordings to the laptop for training (ml/pico_audio.py)
#   alfp_selftest.uf2    Pico + SD card only: the full protocol over an in-memory radio
#   lora_photo_tx.uf2 / lora_photo_rx.uf2   two-Pico LoRa photo test
#
# Code from the team, used unchanged except where noted:
#   third_party/lora_mesh  Mridula   (gpio.h: pin defines wrapped in #ifndef so BUSY/IRQ can move)
#   third_party/wifi_mesh  Goutham
#   third_party/sensors    Maryam (pir, DHT22, weather_tree) + Rhaylene (sound, mic)
# Pins: app/board.h.
cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w CACHE STRING "Board type")
include(pico_sdk_import.cmake)
include(FreeRTOS_Kernel_import.cmake)

project(csc09_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

set(NODE_IDS "1;2;3;4;5" CACHE STRING "Which node images to build (1 = bridge A, 2-5 = B-E)")
option(USE_LORA_MESH "Link Mridula's LoRa mesh from third_party/lora_mesh" ON)
option(USE_WIFI_MESH "Link Goutham's WiFi mesh from third_party/wifi_mesh" ON)
set(LORA_MESH_DIR ${CMAKE_CURRENT_LIST_DIR}/../third_party/lora_mesh CACHE PATH "Mridula's lora_mesh folder")
set(WIFI_MESH_DIR ${CMAKE_CURRENT_LIST_DIR}/../third_party/wifi_mesh CACHE PATH "Goutham's wifi_mesh folder")
set(SENSORS_DIR ${CMAKE_CURRENT_LIST_DIR}/../third_party/sensors CACHE PATH "Maryam's + Rhaylene's sensor code")

# ---- pins that changed from Mridula's original wiring ----------------------------------------
# BUSY moved GP14 -> GP6 (GP14 is the sound trigger). DIO9/IRQ moved GP15 -> GP7 (GP15 is the
# Maker board's SD card CS, hard-wired). Applied to all of her sources through these defines.
set(LORA_BUSY_PIN 6 CACHE STRING "Core1121 BUSY pin")
set(LORA_IRQ_PIN 7 CACHE STRING "Core1121 DIO9 (IRQ) pin")
add_compile_definitions(RADIO_BUSY=${LORA_BUSY_PIN} RADIO_IRQ=${LORA_IRQ_PIN})
if(LORA_IRQ_PIN EQUAL 15)
  message(FATAL_ERROR "GP15 is the SD card CS on the Maker Pi Pico. Move the Core1121 DIO9 wire (e.g. to GP7).")
endif()

# ---- microSD: the Maker Pi Pico slot, sharing SPI1 with the LoRa module ----------------------
set(SPI1_BAUD_HZ 1000000 CACHE STRING "Shared SPI1 speed for LoRa + SD (1 MHz = what Mridula tested)")
set(SD_DEFS SD_SPI_INST=1 SD_SCK=10 SD_MOSI=11 SD_MISO=12 SD_CS=15 SPI1_BAUD_HZ=${SPI1_BAUD_HZ}u)
message(STATUS "microSD: Maker slot, SPI1 GP10/11/12, CS GP15, shared with LoRa (CS GP13) at ${SPI1_BAUD_HZ} Hz")
message(STATUS "LoRa: BUSY GP${LORA_BUSY_PIN}, DIO9 GP${LORA_IRQ_PIN}")

set(AUDIO_SECONDS 5 CACHE STRING "Audio recorded per capture (Rhaylene's mic.h DURATION_SEC)")
set(ALFP_LORA_GAP_MS 500 CACHE STRING "LoRa pacing; use 1000 if the bridge is 3+ hops away")

if(NOT DEFINED ENV{FATFS_PICO_PATH} AND NOT FATFS_PICO_PATH)
  message(FATAL_ERROR "Set FATFS_PICO_PATH to a clone of carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico (tools/fetch_deps.sh)")
endif()
if(NOT FATFS_PICO_PATH)
  set(FATFS_PICO_PATH $ENV{FATFS_PICO_PATH})
endif()
add_subdirectory(${FATFS_PICO_PATH}/src fatfs)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

add_library(alfp STATIC
  ${ROOT}/alfp/src/alfp.c
  ${ROOT}/alfp/src/alfp_wire.c
  ${ROOT}/alfp/src/alfp_crc.c
  ${ROOT}/alfp/src/alfp_crc_tables.c
  ${ROOT}/alfp/src/alfp_profiles.c
  ${ROOT}/alfp/src/alfp_outbox.c
  ${ROOT}/common/uplink.c)
target_include_directories(alfp PUBLIC ${ROOT}/alfp/include ${ROOT}/mesh ${ROOT}/common)
target_compile_definitions(alfp PUBLIC ALFP_LORA_GAP_MS=${ALFP_LORA_GAP_MS}u)
target_compile_options(alfp PRIVATE -Wall -Wextra -Werror -O2)

add_library(sd_storage STATIC app/storage_fatfs.c app/hw_config.c app/board.c)
target_include_directories(sd_storage PUBLIC app)
target_compile_definitions(sd_storage PUBLIC ${SD_DEFS})
target_link_libraries(sd_storage PUBLIC alfp no-OS-FatFS-SD-SDIO-SPI-RPi-Pico pico_stdlib hardware_spi)

# ---- Mridula's LoRa mesh (unchanged sources) ----
if(USE_LORA_MESH)
  add_subdirectory(${LORA_MESH_DIR}/src/gpio lora_gpio)
  add_subdirectory(${LORA_MESH_DIR}/src/spi lora_spi)
  add_subdirectory(${LORA_MESH_DIR}/src/lr1121 lora_lr1121)
  target_include_directories(spi PUBLIC ${LORA_MESH_DIR}/src/gpio ${LORA_MESH_DIR}/src/spi)
  target_include_directories(lr1121 PUBLIC ${LORA_MESH_DIR}/src/gpio ${LORA_MESH_DIR}/src/spi)
  add_library(lora_mesh_lib STATIC ${LORA_MESH_DIR}/lora/lora_radio.c ${LORA_MESH_DIR}/lora/lora_mesh.c)
  target_include_directories(lora_mesh_lib PUBLIC ${LORA_MESH_DIR}/lora ${CMAKE_CURRENT_LIST_DIR}/../mesh)
  target_link_libraries(lora_mesh_lib PUBLIC pico_stdlib pico_rand lr1121 gpio spi)
endif()

# ---- Goutham's WiFi mesh (unchanged sources) ----
if(USE_WIFI_MESH)
  add_library(wifi_mesh_lib STATIC ${WIFI_MESH_DIR}/wifi_mesh.c ${WIFI_MESH_DIR}/wst_link.c)
  target_include_directories(wifi_mesh_lib PUBLIC ${WIFI_MESH_DIR} ${CMAKE_CURRENT_LIST_DIR}/../mesh)
  target_link_libraries(wifi_mesh_lib PUBLIC pico_stdlib pico_rand hardware_uart hardware_irq)
endif()

# ---- Rhaylene's on-node sound classifier: pure C, the same files the laptop tests (host/sound.cmake) ----
add_library(sound STATIC
  sensing/sound_features.c sensing/sound_fft.c sensing/sound_tables.c sensing/sound_classify.c
  sensing/sound_image_pick.c)
target_include_directories(sound PUBLIC sensing ${ROOT}/common)
target_compile_options(sound PRIVATE -Wall -Wextra -Werror -O2)
target_link_libraries(sound PUBLIC pico_stdlib)

# ---- Maryam's + Rhaylene's sensors (unchanged sources) + the glue ----
add_library(sensing STATIC
  ${SENSORS_DIR}/pir.c ${SENSORS_DIR}/sound.c ${SENSORS_DIR}/mic.c ${SENSORS_DIR}/DHT22.c
  sensing/sensing.c sensing/audio_capture.c sensing/audio_features.c)
target_include_directories(sensing PUBLIC ${SENSORS_DIR} sensing app ${ROOT}/common)
target_compile_definitions(sensing PUBLIC AUDIO_SECONDS=${AUDIO_SECONDS}u)
target_link_libraries(sensing PUBLIC sound pico_stdlib hardware_adc hardware_dma hardware_gpio hardware_timer)
# DHT22.c's 40 us "release the line" wait must be exact. Under FreeRTOS, sleep_us() blocks the task
# and wakes it through an interrupt and the timer task, which can hold the line high too long while
# the DHT22 starts answering. Busy-wait instead, without touching Maryam's source.
set_source_files_properties(${SENSORS_DIR}/DHT22.c PROPERTIES COMPILE_DEFINITIONS "sleep_us=busy_wait_us")
set_source_files_properties(sensing/sensing.c sensing/audio_capture.c sensing/audio_features.c sensortest/sensortest_main.c
  PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# ---- node firmware (FreeRTOS), one image per node ----
foreach(N ${NODE_IDS})
  set(T csc09_node_${N})
  add_executable(${T} app/main.c app/radios.c app/freertos_hooks.c)
  target_include_directories(${T} PRIVATE ${CMAKE_CURRENT_LIST_DIR} app)
  target_compile_definitions(${T} PRIVATE NODE_ID=${N})
  target_compile_options(${T} PRIVATE -Wall -Wextra)
  if(USE_LORA_MESH)
    target_compile_definitions(${T} PRIVATE HAVE_LORA_MESH=1)
    target_link_libraries(${T} PRIVATE lora_mesh_lib)
  endif()
  if(USE_WIFI_MESH)
    target_compile_definitions(${T} PRIVATE HAVE_WIFI_MESH=1)
    target_link_libraries(${T} PRIVATE wifi_mesh_lib)
  endif()
  target_link_libraries(${T} PRIVATE alfp sd_storage sensing FreeRTOS-Kernel-Heap4 pico_stdlib pico_rand
    hardware_gpio hardware_spi hardware_uart)
  pico_enable_stdio_usb(${T} 1)
  pico_enable_stdio_uart(${T} 0) # UART0 is the ESP-01, and its default stdio pin GP0 is the PIR
  pico_add_extra_outputs(${T})
endforeach()

# ---- sensor test (no FreeRTOS, no radios, no SD) ----
add_executable(sensor_test sensortest/sensortest_main.c app/board.c)
target_include_directories(sensor_test PRIVATE app)
target_link_libraries(sensor_test PRIVATE sensing pico_stdlib hardware_spi)
pico_enable_stdio_usb(sensor_test 1)
pico_enable_stdio_uart(sensor_test 0)
pico_add_extra_outputs(sensor_test)

# ---- audio dump: raw 16 kHz mic recordings to the laptop (no FreeRTOS, no radios, no SD) ----
# 160 kB static buffer: only fits because nothing else is in this image. Receiver: ml/pico_audio.py
add_executable(audio_dump audiodump/audiodump_main.c app/board.c)
target_include_directories(audio_dump PRIVATE app)
target_compile_options(audio_dump PRIVATE -Wall -Wextra)
target_link_libraries(audio_dump PRIVATE sound pico_stdlib hardware_adc hardware_dma hardware_spi)
pico_enable_stdio_usb(audio_dump 1)
pico_enable_stdio_uart(audio_dump 0)
pico_add_extra_outputs(audio_dump)

# ---- self-test (no FreeRTOS, no radios) ----
add_executable(alfp_selftest selftest/selftest_main.c ${ROOT}/common/loopback_radio.c)
target_link_libraries(alfp_selftest PRIVATE alfp sd_storage pico_stdlib pico_rand)
pico_enable_stdio_usb(alfp_selftest 1)
pico_enable_stdio_uart(alfp_selftest 0)
pico_add_extra_outputs(alfp_selftest)

# ---- two-Pico LoRa photo test (Pico + LoRa module + microSD each) ----
if(USE_LORA_MESH)
  foreach(ROLE tx rx)
    add_executable(lora_photo_${ROLE} phototest/phototest_main.c)
    if(ROLE STREQUAL "rx")
      target_compile_definitions(lora_photo_${ROLE} PRIVATE NODE_ID=1)
    else()
      target_compile_definitions(lora_photo_${ROLE} PRIVATE NODE_ID=2)
    endif()
    target_link_libraries(lora_photo_${ROLE} PRIVATE alfp sd_storage lora_mesh_lib pico_stdlib pico_rand)
    pico_enable_stdio_usb(lora_photo_${ROLE} 1)
    pico_enable_stdio_uart(lora_photo_${ROLE} 0)
    pico_add_extra_outputs(lora_photo_${ROLE})
  endforeach()
endif()
