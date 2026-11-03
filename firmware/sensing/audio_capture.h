/*
 * audio_capture.h - record the MAX9814 mic (GP26 / ADC0) for a few seconds at 16 kHz and return
 * Rhaylene's features. The ADC is paced by its own clock and DMA copies every sample into a 16 kB
 * ring, so the timing is exact even while the comms task is busy, and no 160 kB buffer is needed.
 */
#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "audio_features.h"
#include "sound_features.h"

#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 16000u /* mic.h SAMPLE_RATE */
#endif
#ifndef AUDIO_SECONDS
#define AUDIO_SECONDS 5u /* mic.h DURATION_SEC */
#endif

/* Blocks the CALLING task for `seconds`. wait_ms() is how it sleeps between ring reads
 * (vTaskDelay in FreeRTOS, sleep_ms without). Returns false if no DMA channel was free.
 * lost = samples that were overwritten before they could be read (should be 0). */
bool audio_capture(uint32_t seconds, void (*wait_ms)(uint32_t ms), audio_result_t* out, uint32_t* lost);

/* Time spent in the spectral feature code during one capture (sf_add calls only). */
typedef struct
{
    uint32_t feat_us;    /* total */
    uint32_t maxcall_us; /* longest single sf_add() call: must stay far below the ~512 ms ring slack */
    uint32_t calls;
} audio_timing_t;

/* Same as audio_capture(), and also feeds every chunk to sf_add(sf) (Rhaylene's spectral features,
 * sound_features.h; sf is reset first). sf and timing may be NULL. */
bool audio_capture_sf(uint32_t seconds, void (*wait_ms)(uint32_t ms), audio_result_t* out, uint32_t* lost, sf_t* sf,
                      audio_timing_t* timing);

#endif
