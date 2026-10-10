#include "pir.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

void pir_init(void) {
    gpio_init(PIR_PIN);
    gpio_set_dir(PIR_PIN, GPIO_IN);
    gpio_pull_down(PIR_PIN);
}

bool pir_is_motion_detected(void) {
    return gpio_get(PIR_PIN);
}