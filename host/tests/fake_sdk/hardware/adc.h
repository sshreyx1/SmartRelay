#ifndef FAKE_HARDWARE_ADC_H
#define FAKE_HARDWARE_ADC_H
#include <stdint.h>
static inline void adc_init (void)
{
}
static inline void adc_gpio_init (unsigned gpio)
{
    (void)gpio;
}
static inline uint16_t adc_read (void)
{
    return 2048;
}
#endif

/*** end of file ***/

