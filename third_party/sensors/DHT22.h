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

#ifdef __cplusplus
}
#endif

#endif // DHT22_H