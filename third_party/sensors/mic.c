#include "mic.h"
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include <math.h>

void mic_init(void) {
    adc_init();
    adc_gpio_init(MIC_ADC_PIN);
}

void mic_record_buffer(uint16_t *buffer, int num_samples) {
    uint64_t sample_interval_us = 1000000 / SAMPLE_RATE;
    absolute_time_t target_time = get_absolute_time();

    for (int i = 0; i < num_samples; i++) {
        buffer[i] = adc_read();
        target_time = delayed_by_us(target_time, sample_interval_us);
        sleep_until(target_time);
    }
}

SoundAnalysis mic_analyze_buffer(uint16_t *buffer, int num_samples) {
    SoundAnalysis analysis;
    double energy_acc = 0.0;
    int zero_crossings = 0;

    // Remove DC offset (~2048 for 1.65V center)
    for (int i = 0; i < num_samples; i++) {
        int16_t centered = (int16_t)buffer[i] - 2048;
        energy_acc += (centered * centered);

        if (i > 0) {
            int16_t prev_centered = (int16_t)buffer[i - 1] - 2048;
            if ((centered >= 0 && prev_centered < 0) || (centered < 0 && prev_centered >= 0)) {
                zero_crossings++;
            }
        }
    }

    analysis.energy = (float)(energy_acc / num_samples);
    analysis.zcr = (float)zero_crossings / num_samples;

    // Decision Tree Logic
    if (analysis.energy > 50000.0f) {
        analysis.class_id = 1;
        analysis.label = "LOUD_ANIMAL_CALL";
    } else if (analysis.zcr > 0.15f) {
        analysis.class_id = 2;
        analysis.label = "HIGH_FREQ_RUSTLE";
    } else {
        analysis.class_id = 0;
        analysis.label = "AMBIENT_NOISE";
    }

    return analysis;
}