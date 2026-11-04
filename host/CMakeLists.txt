# Laptop build: the ALFP library + mesh simulator + tests + experiments.
#   cmake -S host -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(alfp_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(ALFP_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" ON)
# The ALFP simulator below is POSIX-only; on Windows with MSVC build just the sound tools and tests.
option(SOUND_ONLY "Build only the sound classifier tools and tests (host/sound.cmake)" OFF)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(WARN -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion)
if(ALFP_SANITIZE AND NOT MSVC)
  set(SAN -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

include(sound.cmake)
if(SOUND_ONLY OR MSVC)
  return()
endif()

# The protocol library: exactly the same sources the Pico firmware compiles.
add_library(alfp STATIC
  ${ROOT}/alfp/src/alfp.c
  ${ROOT}/alfp/src/alfp_wire.c
  ${ROOT}/alfp/src/alfp_crc.c
  ${ROOT}/alfp/src/alfp_crc_tables.c
  ${ROOT}/alfp/src/alfp_profiles.c
  ${ROOT}/alfp/src/alfp_outbox.c
  ${ROOT}/common/uplink.c)
target_include_directories(alfp PUBLIC ${ROOT}/alfp/include ${ROOT}/mesh ${ROOT}/common)
target_compile_options(alfp PRIVATE ${WARN} ${SAN} -O2 -g)
target_link_options(alfp PUBLIC ${SAN})

add_library(sim STATIC
  sim/sim_mesh.c
  sim/storage_posix.c
  sim/sim_net.c)
target_include_directories(sim PUBLIC sim)
target_link_libraries(sim PUBLIC alfp m)
target_compile_options(sim PRIVATE -Wall -Wextra -Werror ${SAN} -O2 -g)

add_executable(test_units tests/test_units.c)
target_link_libraries(test_units alfp)
target_compile_options(test_units PRIVATE -Wall -Wextra ${SAN} -O1 -g)

add_executable(test_transfer tests/test_transfer.c)
target_link_libraries(test_transfer sim)
target_compile_options(test_transfer PRIVATE -Wall -Wextra ${SAN} -O1 -g)

add_executable(test_filtering tests/test_filtering.c)
target_link_libraries(test_filtering sim)
target_compile_options(test_filtering PRIVATE -Wall -Wextra ${SAN} -O1 -g)

add_executable(test_uplink tests/test_uplink.c)
target_link_libraries(test_uplink sim)
target_compile_options(test_uplink PRIVATE -Wall -Wextra ${SAN} -O1 -g)

# Rhaylene's mic.c (unchanged) vs the streaming version the firmware uses
add_executable(test_audio tests/test_audio.c ${ROOT}/firmware/sensing/audio_features.c ${ROOT}/third_party/sensors/mic.c)
target_include_directories(test_audio PRIVATE ${ROOT}/firmware/sensing ${ROOT}/third_party/sensors tests/fake_sdk)
target_link_libraries(test_audio m)
target_compile_options(test_audio PRIVATE -Wall ${SAN} -O1 -g)
target_link_options(test_audio PRIVATE ${SAN})

add_executable(experiments experiments/experiments.c)
target_link_libraries(experiments sim)
target_compile_options(experiments PRIVATE -Wall -Wextra -O2 -g ${SAN})

enable_testing()
add_test(NAME units COMMAND test_units)
add_test(NAME audio_features COMMAND test_audio)
add_test(NAME transfer COMMAND test_transfer WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME filtering COMMAND test_filtering WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME uplink COMMAND test_uplink WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME uplink_python
    COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/test_bridge_receiver.py ${CMAKE_BINARY_DIR}/uplink_stream.txt
            ${CMAKE_BINARY_DIR}/sim_out/uplink/node2/samples/img_00.jpg)
  set_tests_properties(uplink_python PROPERTIES DEPENDS uplink)
endif()

# Mridula's real lora_mesh.c + ALFP, two processes over a fake UDP radio (real time, ~20 s)
find_program(BASH bash)
if(BASH)
  add_test(NAME lora_mesh_integration COMMAND ${BASH} ${CMAKE_CURRENT_SOURCE_DIR}/integration/run_integration.sh 0.2)
  set_tests_properties(lora_mesh_integration PROPERTIES PASS_REGULAR_EXPRESSION "INTEGRATION PASS" TIMEOUT 300)
  # Goutham's real wifi_mesh.c + ALFP, three processes, E -> C -> A through his topology filter
  add_test(NAME wifi_mesh_integration COMMAND ${BASH} ${CMAKE_CURRENT_SOURCE_DIR}/integration/run_wifi_integration.sh 0.2)
  set_tests_properties(wifi_mesh_integration PROPERTIES PASS_REGULAR_EXPRESSION "WIFI INTEGRATION PASS" TIMEOUT 300)
endif()
