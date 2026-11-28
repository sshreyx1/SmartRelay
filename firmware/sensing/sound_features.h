/*
 * sound_features.h - streaming spectral features of a capture, for the on-node classifier.
 *                    Owner: Rhaylene. Pure C, no Pico headers (built and tested on the laptop too:
 *                    host/tests/test_sound_features.c, host/tools/sound_featx.c).
 *
 * Why spectral SHAPE: the MAX9814 has automatic gain control, so absolute loudness says little
 * about what made the sound, and the mic's resting level is 2083-2117, not 2048. Every frame has
 * its own mean removed, and the band features are ratios to the frame's total energy.
 *
 * Feed samples with sf_add() in chunks of any size (the DMA ring hands them over in pieces);
 * the output does not depend on how the stream was split. Samples are cut into 256-sample frames
 * (16 ms at 16 kHz, hop 256, no overlap); a last partial frame is ignored. Per frame:
 *   1. remove the frame's own mean (integer), count zero crossings, energy e = sum(c^2)
 *   2. Hann window, then shift left so the peak sits just under 2^14 ("block floating point":
 *      quiet frames keep their precision through the integer FFT; ratios are unchanged by it)
 *   3. 256-point Q15 FFT (sound_fft.c, output = DFT / 256), power per bin, bin width 62.5 Hz
 *   4. 8 bands (bins): 62-250 Hz (1-3), 250-500 (4-7), 500-1k (8-15), 1-2k (16-31), 2-3k (32-47),
 *      3-4k (48-63), 4-5.5k (64-87), 5.5-8k (88-128); total = bins 1-128 (DC excluded)
 *   5. shape band  b_i = ln(E_i + 1) - ln(E_tot + 1)
 *      centroid    = sum(k * P_k) / sum(P_k) * 62.5 Hz           (0 for an all-zero frame)
 *      zcr         = crossings / 255                              (per sample, like mic.c)
 *      loudness    = 10 * log10(e / 256 + 1) dB                   (time domain, before the window)
 *      flux        = mean_i |b_i(t) - b_i(t-1)|                   (shape change, AGC-proof)
 * Over the capture only running sums are kept (Welford mean/variance, a 32-bucket loudness
 * histogram of 2 dB buckets), never the frames. The 25 features, in order:
 *   band_mean[8], band_std[8], centroid_mean, centroid_std, zcr_mean, zcr_std,
 *   loud_std, loud_range (max - min dB), active_frac (frames >= 6 dB above the quietest 10%),
 *   flux_mean, log_rms (ln(1 + DC-corrected RMS of all samples): AGC-affected, use with care)
 *
 * sf_t is about 1.9 kB (frame buffer 512 B + FFT scratch 1 kB + sums); keep it static, not on
 * the 1024-word sensors task stack.
 */
#ifndef SOUND_FEATURES_H
#define SOUND_FEATURES_H

#include "float_types.h"
#include <stdint.h>

#define SF_VERSION 1
#define SF_N_FEATURES 25
#define SF_FRAME 256
#define SF_N_BANDS 8
#define SF_HIST_BUCKETS 32
#define SF_HIST_DB 2.0f /* loudness histogram bucket width */
#define SF_ACTIVE_DB 6.0f
#define SF_SAMPLE_RATE 16000u

typedef struct
{
    uint16_t frame[SF_FRAME];
    uint32_t fill;        /* samples in frame[] */
    int16_t real[SF_FRAME]; /* FFT scratch, here so it is not on the caller's stack */
    int16_t imag[SF_FRAME];
    uint32_t n_frames;
    uint32_t n_samples;   /* every sample added, including a last partial frame */
    uint64_t sum;
    uint64_t sum_sq; /* raw samples, for the DC-corrected RMS */
    float64_t band_mean[SF_N_BANDS];
    float64_t band_m2[SF_N_BANDS];
    float64_t cen_mean;
    float64_t cen_m2;
    float64_t zcr_mean;
    float64_t zcr_m2;
    float64_t loud_mean;
    float64_t loud_m2;
    float32_t loud_min;
    float32_t loud_max;
    float64_t flux_sum;
    float32_t prev_shape[SF_N_BANDS];
    uint16_t hist[SF_HIST_BUCKETS];
} sf_t;

extern const char* const g_sf_feature_names[SF_N_FEATURES];

void sf_reset(sf_t* p_feat);
void sf_add(sf_t* p_feat, const uint16_t* p_samples, uint32_t count);
/* out: SF_N_FEATURES values, always finite (all 0 if no complete frame was seen). */
void sf_finish(const sf_t* p_feat, float32_t out[SF_N_FEATURES]);

/* For capture_meta: RMS around the capture's own mean (ADC counts), and zero crossings per second. */
float32_t sf_rms_dc_corrected(const sf_t* p_feat);
float32_t sf_zcr_per_s(const sf_t* p_feat);

#endif
