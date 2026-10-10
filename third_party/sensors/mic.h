#ifndef MIC_H
#define MIC_H

#include <stdint.h>
#include <stdbool.h>

#define MIC_ADC_PIN 26 // GP26 (ADC0 for MAX9814)
#define SAMPLE_RATE 16000
#define DURATION_SEC 5
#define NUM_SAMPLES (SAMPLE_RATE * DURATION_SEC)

typedef struct {
    float energy;
    float zcr;
    int class_id;
    const char *label;
} SoundAnalysis;

void mic_init(void);
void mic_record_buffer(uint16_t *buffer, int num_samples);
SoundAnalysis mic_analyze_buffer(uint16_t *buffer, int num_samples);

#endif