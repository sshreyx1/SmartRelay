#include "sound.h"
#include "pico/stdlib.h"

void sound_init(void) {
    gpio_init(SOUND_PIN);
    gpio_set_dir(SOUND_PIN, GPIO_IN);
    gpio_pull_down(SOUND_PIN);
}

bool sound_is_triggered(void) {
    return gpio_get(SOUND_PIN);
}