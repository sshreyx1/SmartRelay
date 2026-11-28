#include "float_types.h"
#include "dht22.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "pico/stdlib.h"
#include <stdio.h>

#define DHT_PIN 4
#define MIN_INTERVAL 2000

static uint32_t expect_pulse(bool b_level);

/* Default: nothing (the FreeRTOS build and the bench test programs). A firmware that needs the
 * bit-timing part protected defines its own non-weak versions. */
__attribute__((weak)) void dht_timing_enter(void) {}
__attribute__((weak)) void dht_timing_exit(void) {}

void dht_init() {
    gpio_init(DHT_PIN);
    gpio_set_dir(DHT_PIN, GPIO_IN);
    gpio_pull_up(DHT_PIN);
}

bool dht_read(float32_t *p_temperature, float32_t *p_humidity) {
    uint32_t current_millis = to_ms_since_boot(get_absolute_time());
    static uint32_t last_read_time = 0;

    if ((current_millis - last_read_time) < MIN_INTERVAL) {
        return false;
    }
    last_read_time = current_millis;

    uint32_t pulses[80] = {0};

    gpio_set_dir(DHT_PIN, GPIO_OUT);
    gpio_put(DHT_PIN, 0);
    sleep_ms(20);
    /* ~5 ms from releasing the line to the 40th bit: every bit is decoded by counting loop
     * iterations, so nothing may interrupt this part (see dht_timing_enter in dht22.h). */
    dht_timing_enter();
    gpio_put(DHT_PIN, 1);
    sleep_us(40);
    gpio_set_dir(DHT_PIN, GPIO_IN);

    bool b_is_ok = (UINT32_MAX != expect_pulse(0)) && (UINT32_MAX != expect_pulse(1));

    for (int idx = 0; b_is_ok && (idx < 80); idx += 2) {
        pulses[idx] = expect_pulse(0);
        pulses[idx + 1] = expect_pulse(1);

        if ((UINT32_MAX == pulses[idx]) || (UINT32_MAX == pulses[idx + 1])) {
            b_is_ok = false;
        }
    }
    dht_timing_exit();
    if (!b_is_ok) {
        return false;
    }

    uint8_t bits[5] = {0};
    for (int idx = 0; idx < 40; ++idx) {
        uint32_t low_cycles = pulses[2 * idx];
        uint32_t high_cycles = pulses[2 * idx + 1];
        bits[idx / 8] <<= 1;
        if (high_cycles > low_cycles) {
            bits[idx / 8] |= 1;
        }
    }

    uint16_t raw_humidity = (bits[0] << 8) | bits[1];
    uint16_t raw_temperature = (bits[2] << 8) | bits[3];
    uint8_t check_sum = bits[4];

    *p_humidity = raw_humidity * 0.1f;
    *p_temperature = raw_temperature * 0.1f;

    if (check_sum == ((bits[0] + bits[1] + bits[2] + bits[3]) & 0xFF)) {
        return true;
    }
    return false;
}

static uint32_t expect_pulse(bool b_level) {
    uint32_t count = 0;
    while (gpio_get(DHT_PIN) == b_level) {
        if (++count >= 10000) {
            return UINT32_MAX;
        }
        tight_loop_contents();
    }
    return count;
}