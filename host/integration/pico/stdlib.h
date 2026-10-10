/* Laptop stand-in for the Pico SDK functions lora_mesh.c uses (integration test only). */
#ifndef FAKE_PICO_STDLIB_H
#define FAKE_PICO_STDLIB_H
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
typedef uint64_t absolute_time_t;
static inline absolute_time_t get_absolute_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
static inline uint32_t to_ms_since_boot(absolute_time_t t)
{
    return (uint32_t)(t / 1000u);
}
static inline void sleep_ms(uint32_t ms)
{
    usleep(ms * 1000u);
}
#endif
