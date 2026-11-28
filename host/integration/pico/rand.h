#ifndef FAKE_PICO_RAND_H
#define FAKE_PICO_RAND_H
#include <stdint.h>
#include <stdlib.h>
static inline uint32_t get_rand_32 (void)
{
    return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
}
#endif

/*** end of file ***/

