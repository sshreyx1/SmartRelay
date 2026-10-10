#include "DHT22.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "pico/stdlib.h"
#include <stdio.h>

#define DHT_PIN 4
#define MIN_INTERVAL 2000

static uint32_t expectPulse(bool level);

void DHT_init() {
    gpio_init(DHT_PIN);
    gpio_set_dir(DHT_PIN, GPIO_IN);
    gpio_pull_up(DHT_PIN);
}

bool DHT_read(float *temperature, float *humidity) {
    uint32_t currentMillis = to_ms_since_boot(get_absolute_time());
    static uint32_t lastReadTime = 0;

    if ((currentMillis - lastReadTime) < MIN_INTERVAL) {
        return false;
    }
    lastReadTime = currentMillis;

    uint32_t pulses[80] = {0};

    gpio_set_dir(DHT_PIN, GPIO_OUT);
    gpio_put(DHT_PIN, 0);
    sleep_ms(20);
    gpio_put(DHT_PIN, 1);
    sleep_us(40);
    gpio_set_dir(DHT_PIN, GPIO_IN);

    if (expectPulse(0) == UINT32_MAX || expectPulse(1) == UINT32_MAX) {
        return false;
    }

    for (int i = 0; i < 80; i += 2) {
        pulses[i] = expectPulse(0);
        pulses[i + 1] = expectPulse(1);

        if (pulses[i] == UINT32_MAX || pulses[i + 1] == UINT32_MAX) {
            return false;
        }
    }

    uint8_t bits[5] = {0};
    for (int i = 0; i < 40; ++i) {
        uint32_t lowCycles = pulses[2 * i];
        uint32_t highCycles = pulses[2 * i + 1];
        bits[i / 8] <<= 1;
        if (highCycles > lowCycles) {
            bits[i / 8] |= 1;
        }
    }

    uint16_t rawHumidity = (bits[0] << 8) | bits[1];
    uint16_t rawTemperature = (bits[2] << 8) | bits[3];
    uint8_t checkSum = bits[4];

    *humidity = rawHumidity * 0.1f;
    *temperature = rawTemperature * 0.1f;

    if (checkSum == ((bits[0] + bits[1] + bits[2] + bits[3]) & 0xFF)) {
        return true;
    }
    return false;
}

static uint32_t expectPulse(bool level) {
    uint32_t count = 0;
    while (gpio_get(DHT_PIN) == level) {
        if (++count >= 10000) {
            return UINT32_MAX;
        }
        tight_loop_contents();
    }
    return count;
}