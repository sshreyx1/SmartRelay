# Rhaylene's sound classifier on the laptop: the same C the node runs (firmware/sensing/sound_*),
# the sound_featx CLI the ml/ pipeline calls, and their tests. Portable C99: builds with gcc/clang
# (macOS, Linux, WSL) and MSVC/MinGW (Windows). Included by host/CMakeLists.txt; on Windows with
# MSVC build only this part:  cmake -S host -B build -DSOUND_ONLY=ON && cmake --build build --config Release
set(SOUND_SRC ${ROOT}/firmware/sensing)

add_library(sound STATIC
  ${SOUND_SRC}/sound_features.c
  ${SOUND_SRC}/sound_fft.c
  ${SOUND_SRC}/sound_tables.c
  ${SOUND_SRC}/sound_classify.c
  ${SOUND_SRC}/sound_image_pick.c)
target_include_directories(sound PUBLIC ${SOUND_SRC} ${ROOT}/common)

# -ffp-contract=off: no fused multiply-add on the laptop, because the Pico (Cortex-M0+) has none,
# and the features must match bit for bit as far as possible (ml/check_parity.py).
if(MSVC)
  set(SOUND_WARN /W3 /fp:precise)
  set(SOUND_SAN "")
else()
  set(SOUND_WARN -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -ffp-contract=off)
  set(SOUND_SAN ${SAN})
  target_link_libraries(sound PUBLIC m)
endif()
target_compile_options(sound PRIVATE ${SOUND_WARN} ${SOUND_SAN} -O2)
target_link_options(sound PUBLIC ${SOUND_SAN})

add_executable(sound_featx tools/sound_featx.c ${SOUND_SRC}/audio_features.c)
target_link_libraries(sound_featx sound)
target_compile_options(sound_featx PRIVATE ${SOUND_WARN} ${SOUND_SAN})

add_executable(test_sound_features tests/test_sound_features.c)
target_link_libraries(test_sound_features sound)
target_compile_options(test_sound_features PRIVATE ${SOUND_SAN})

# the generated model must classify the generated vectors exactly like ml/qmodel.py
add_executable(test_sound_model tests/test_sound_model.c)
target_link_libraries(test_sound_model sound)
target_compile_options(test_sound_model PRIVATE ${SOUND_SAN})

add_executable(test_sound_image_pick tests/test_sound_image_pick.c)
target_link_libraries(test_sound_image_pick sound)
target_compile_options(test_sound_image_pick PRIVATE ${SOUND_SAN})

add_executable(test_capture_id tests/test_capture_id.c)
target_include_directories(test_capture_id PRIVATE ${ROOT}/common)
target_compile_options(test_capture_id PRIVATE ${SOUND_WARN} ${SOUND_SAN})
target_link_options(test_capture_id PRIVATE ${SOUND_SAN})

enable_testing()
add_test(NAME sound_features COMMAND test_sound_features)
add_test(NAME sound_model COMMAND test_sound_model)
add_test(NAME sound_image_pick COMMAND test_sound_image_pick)
add_test(NAME capture_id COMMAND test_capture_id)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME sound_classes COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sound_classes.py)
  add_test(NAME capture_id_python COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_capture_id.py)
endif()
