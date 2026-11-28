/*
 * audio_features.h - Rhaylene's sound analysis, computed while the samples
 * stream in.
 *
 * Her mic.c records 5 s at 16 kHz into one 160 kB buffer (80 000 x uint16) and
 * then runs mic_analyze_buffer(). 160 kB is 60% of the RP2040's RAM, which does
 * not fit next to FreeRTOS, two radio stacks and the SD driver. Energy and zero
 * crossings are running sums, so this file computes exactly the same numbers
 * block by block from a small DMA ring instead.
 *
 * Same maths as mic_analyze_buffer():
 *   centered = sample - 2048
 *   energy   = sum(centered^2) / N
 *   zcr      = crossings / N              (sign change between consecutive
 *                                         samples)
 *   class    = 1 LOUD_ANIMAL_CALL if energy > 50000
 *              2 HIGH_FREQ_RUSTLE if zcr > 0.15
 *              0 AMBIENT_NOISE    otherwise
 * host/tests/test_audio.c feeds the same samples to her mic_analyze_buffer()
 * and to this code and checks the results are identical. If Rhaylene changes
 * her thresholds, change them here too.
 *
 * Pure C, no Pico headers, so it builds and is tested on the laptop.
 */
#ifndef AUDIO_FEATURES_H
#define AUDIO_FEATURES_H

#include "float_types.h"
#include <stdbool.h>
#include <stdint.h>

/* mic.c: energy > 50000 -> LOUD_ANIMAL_CALL */
#define AUDIO_ENERGY_LOUD 50000.0f
#define AUDIO_ZCR_RUSTLE  0.15f /* mic.c: zcr > 0.15 -> HIGH_FREQ_RUSTLE */

typedef struct
{
    uint64_t sum_sq;    /* sum of (sample - 2048)^2, exact */
    uint64_t sum;       /* sum of raw samples, for the DC level check */
    uint32_t crossings; /* zero crossings */
    uint32_t count;     /* samples seen */
    int16_t  prev;      /* previous centered sample */
} audio_feat_t;

typedef struct
{
    float32_t energy; /* same as SoundAnalysis.energy */
    /* same as SoundAnalysis.zcr (crossings per sample) */
    float32_t   zcr;
    int         class_id; /* same as SoundAnalysis.class_id */
    const char* p_label;  /* same as SoundAnalysis.label */
    /* sqrt(energy), ADC counts: what capture_meta carries as sound_rms */
    uint16_t rms;
    uint16_t zcr_per_s; /* crossings per second: capture_meta sound_zcr */
    /* average raw reading; about 2048 if the mic is biased at 1.65 V */
    uint16_t mean_adc;
    uint32_t samples;
} audio_result_t;

void audio_feat_reset(audio_feat_t* p_feat);
void audio_feat_add(audio_feat_t* p_feat, const uint16_t* p_samples,
                    uint32_t count);
void audio_feat_finish(const audio_feat_t* p_feat, uint32_t sample_rate,
                       audio_result_t* p_out);

#endif

/*** end of file ***/

