/* Minimal stand-ins so Rhaylene's mic.c compiles on the laptop (only mic_analyze_buffer is used). */
#ifndef FAKE_PICO_STDLIB_H
#define FAKE_PICO_STDLIB_H
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t absolute_time_t;
static inline absolute_time_t get_absolute_time(void) { return 0; }
static inline absolute_time_t delayed_by_us(absolute_time_t t, uint64_t us) { return t + us; }
static inline void sleep_until(absolute_time_t t) { (void)t; }
#endif
