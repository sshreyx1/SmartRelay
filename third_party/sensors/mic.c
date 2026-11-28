#include "float_types.h"
#include "mic.h"
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include <math.h>

void mic_init (void)
{
    adc_init();
    adc_gpio_init(MIC_ADC_PIN);
}

void mic_record_buffer (uint16_t* p_buffer, int num_samples)
{
    uint64_t        sample_interval_us = 1000000 / SAMPLE_RATE;
    absolute_time_t target_time        = get_absolute_time();

    for (int idx = 0; idx < num_samples; idx++)
    {
        p_buffer[idx] = adc_read();
        target_time   = delayed_by_us(target_time, sample_interval_us);
        sleep_until(target_time);
    }
}

SoundAnalysis mic_analyze_buffer (uint16_t* p_buffer, int num_samples)
{
    SoundAnalysis analysis;
    float64_t     energy_acc     = 0.0;
    int           zero_crossings = 0;

    // Remove DC offset (~2048 for 1.65V center)
    for (int idx = 0; idx < num_samples; idx++)
    {
        int16_t centered = (int16_t)p_buffer[idx] - 2048;
        energy_acc += (centered * centered);

        if (idx > 0)
        {
            int16_t prev_centered = (int16_t)p_buffer[idx - 1] - 2048;
            if (((centered >= 0) && (prev_centered < 0)) ||
                ((centered < 0) && (prev_centered >= 0)))
            {
                zero_crossings++;
            }
        }
    }

    analysis.energy = (float32_t)(energy_acc / num_samples);
    analysis.zcr    = (float32_t)zero_crossings / num_samples;

    // Decision Tree Logic
    if (analysis.energy > 50000.0f)
    {
        analysis.class_id = 1;
        analysis.p_label  = "LOUD_ANIMAL_CALL";
    }
    else if (analysis.zcr > 0.15f)
    {
        analysis.class_id = 2;
        analysis.p_label  = "HIGH_FREQ_RUSTLE";
    }
    else
    {
        analysis.class_id = 0;
        analysis.p_label  = "AMBIENT_NOISE";
    }

    return analysis;
}

/*** end of file ***/

