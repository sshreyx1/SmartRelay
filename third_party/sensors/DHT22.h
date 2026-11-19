#ifndef DHT22_H
#define DHT22_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "pico/stdlib.h"

#define DHT_OK 0
#define DHT_ERROR_TIMEOUT -1
#define DHT_ERROR_CHECKSUM -2

void DHT_init();
bool DHT_read(float *temperature, float *humidity);

/* Called around the ~5 ms bit-timing part of DHT_read() (not the 20 ms start pulse). Weak no-ops
 * in DHT22.c; the dual-core firmware (firmware_smp/app/main_smp.c) masks interrupts on the
 * sensors core here, as agreed after Tri 1. Must not block or call the RTOS. */
void DHT_timing_enter(void);
void DHT_timing_exit(void);

#ifdef __cplusplus
}
#endif

#endif // DHT22_H