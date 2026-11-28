/*
 * test_sound_features.c - the streaming spectral features (firmware/sensing/sound_features.c) and
 * the Q15 FFT under them (sound_fft.c), on synthetic signals with known answers.
 */
#include "float_types.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sound_features.h"
#include "sound_fft.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define N (16000 * 5)
static uint16_t buf[N];
static sf_t feat;
static int fails;

#define CHECK(cond, ...)                         \
    do                                           \
    {                                            \
        if (!(cond))                             \
        {                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                 \
            printf("\n");                        \
            fails++;                             \
        }                                        \
    } while (0)

enum
{
    F_BAND0 = 0,
    F_CENTROID = 16,
    F_ZCR = 18,
    F_FLUX = 23,
    F_LOGRMS = 24
};

static void sine(float64_t freq_hz, float64_t amp, float64_t dc_offset, unsigned seed, int noise)
{
    srand(seed);
    for (int idx = 0; idx < N; idx++)
    {
        float64_t value = dc_offset + amp * sin(2 * M_PI * freq_hz * idx / 16000.0) + (noise ? (rand() % (2 * noise + 1)) - noise : 0);
        buf[idx] = (uint16_t)(value < 0 ? 0 : value > 4095 ? 4095 : lround(value));
    }
}

static void features(uint32_t chunk, float32_t out[SF_N_FEATURES])
{
    sf_reset(&feat);
    for (uint32_t off = 0; off < N; off += chunk)
    {
        sf_add(&feat, buf + off, N - off < chunk ? N - off : chunk);
    }
    sf_finish(&feat, out);
}

static int dominant_band(const float32_t* p_feats)
{
    int best = 0;
    for (int band = 1; band < SF_N_BANDS; band++)
    {
        if (p_feats[F_BAND0 + band] > p_feats[F_BAND0 + best])
        {
            best = band;
        }
    }
    return best;
}

static void test_sine_bands(void)
{
    float32_t feats[SF_N_FEATURES];
    sine(1000, 800, 2100, 1, 3);
    features(4096, feats);
    CHECK(dominant_band(feats) == 3, "1 kHz: dominant band %d, want 3 (1-2k)", dominant_band(feats));
    CHECK(fabsf(feats[F_CENTROID] - 1000.0f) < 100.0f, "1 kHz: centroid %.1f Hz", (float64_t)feats[F_CENTROID]);

    sine(6000, 800, 2100, 2, 3);
    features(4096, feats);
    CHECK(dominant_band(feats) == 7, "6 kHz: dominant band %d, want 7 (5.5-8k)", dominant_band(feats));
}

static void test_block_sizes(void)
{
    float32_t ref[SF_N_FEATURES];
    float32_t feats[SF_N_FEATURES];
    sine(700, 500, 2083, 3, 40);
    features(N, ref);
    const uint32_t sizes[] = {1, 7, 256, 1000, 8192};
    for (unsigned size_idx = 0; size_idx < sizeof(sizes) / sizeof(sizes[0]); size_idx++)
    {
        features(sizes[size_idx], feats);
        CHECK(memcmp(feats, ref, sizeof(feats)) == 0, "block size %u changes the output", sizes[size_idx]);
    }
}

static void test_dc_offset(void)
{
    float32_t vec_a[SF_N_FEATURES];
    float32_t vec_b[SF_N_FEATURES];
    sine(1500, 300, 2048, 4, 30);
    features(4096, vec_a);
    for (int idx = 0; idx < N; idx++)
    {
        buf[idx] = (uint16_t)(buf[idx] + 60);
    }
    features(4096, vec_b);
    for (int kdx = 0; kdx < F_LOGRMS + 1; kdx++) /* log_rms is DC-corrected too, so all 25 must hold */
    {
        CHECK(fabsf(vec_a[kdx] - vec_b[kdx]) <= 1e-3f * (1.0f + fabsf(vec_a[kdx])), "+60 DC changes %s: %g -> %g", g_sf_feature_names[kdx],
              (float64_t)vec_a[kdx], (float64_t)vec_b[kdx]);
    }
}

static void test_silence_and_clipping(void)
{
    float32_t feats[SF_N_FEATURES];
    for (int idx = 0; idx < N; idx++)
    {
        buf[idx] = 2100;
    }
    features(4096, feats);
    for (int kdx = 0; kdx < SF_N_FEATURES; kdx++)
    {
        CHECK(isfinite(feats[kdx]), "silence: %s not finite", g_sf_feature_names[kdx]);
    }
    CHECK(feats[F_LOGRMS] == 0.0f, "silence: log_rms %g", (float64_t)feats[F_LOGRMS]);
    for (int idx = 0; idx < N; idx++)
    {
        buf[idx] = (idx / 8) & 1 ? 4095 : 0;
    }
    features(4096, feats);
    for (int kdx = 0; kdx < SF_N_FEATURES; kdx++)
    {
        CHECK(isfinite(feats[kdx]), "clipped: %s not finite", g_sf_feature_names[kdx]);
    }
    sf_reset(&feat);
    sf_add(&feat, buf, 100); /* less than one frame */
    sf_finish(&feat, feats);
    for (int kdx = 0; kdx < SF_N_FEATURES; kdx++)
    {
        CHECK(isfinite(feats[kdx]), "short: %s not finite", g_sf_feature_names[kdx]);
    }
}

static void test_fft_vs_dft(void)
{
    int16_t real[SOUND_FFT_N];
    int16_t imag[SOUND_FFT_N];
    float64_t value_x[SOUND_FFT_N];
    srand(5);
    for (int pos = 0; pos < SOUND_FFT_N; pos++)
    {
        value_x[pos] = 9000 * sin(2 * M_PI * 20.3 * pos / SOUND_FFT_N) + 3000 * cos(2 * M_PI * 77 * pos / SOUND_FFT_N) +
               (rand() % 201) - 100;
        real[pos] = (int16_t)lround(value_x[pos]);
        imag[pos] = 0;
    }
    sound_fft256(real, imag);
    float64_t fft = 0;
    float64_t dft = 0;
    int peak_fft = 0;
    int peak_dft = 0;
    float64_t best_fft = -1;
    float64_t best_dft = -1;
    for (int kdx = 1; kdx <= SOUND_FFT_N / 2; kdx++)
    {
        float64_t dft_real = 0;
        float64_t dft_imag = 0;
        for (int pos = 0; pos < SOUND_FFT_N; pos++)
        {
            dft_real += value_x[pos] * cos(2 * M_PI * kdx * pos / SOUND_FFT_N);
            dft_imag -= value_x[pos] * sin(2 * M_PI * kdx * pos / SOUND_FFT_N);
        }
        float64_t power_dft = dft_real * dft_real + dft_imag * dft_imag;
        float64_t fft_real = real[kdx] * 256.0; /* undo the documented 1/256 scaling */
        float64_t file_info = imag[kdx] * 256.0;
        float64_t power_fft = fft_real * fft_real + file_info * file_info;
        dft += power_dft;
        fft += power_fft;
        if (power_dft > best_dft)
        {
            best_dft = power_dft;
            peak_dft = kdx;
        }
        if (power_fft > best_fft)
        {
            best_fft = power_fft;
            peak_fft = kdx;
        }
    }
    CHECK(peak_fft == peak_dft, "FFT peak bin %d, DFT %d", peak_fft, peak_dft);
    CHECK(fabs(fft - dft) / dft < 0.05, "FFT power off by %.2f%%", 100 * fabs(fft - dft) / dft);
}

static void test_quiet_keeps_shape(void)
{
    /* block floating point: a 4-count tone must give the same spectral shape as an 800-count one */
    float32_t loud[SF_N_FEATURES];
    float32_t quiet[SF_N_FEATURES];
    sine(2500, 800, 2100, 6, 0);
    features(4096, loud);
    sine(2500, 4, 2100, 6, 0);
    features(4096, quiet);
    CHECK(dominant_band(quiet) == 4, "quiet 2.5 kHz: dominant band %d, want 4", dominant_band(quiet));
    CHECK(fabsf(loud[F_CENTROID] - quiet[F_CENTROID]) < 150.0f, "quiet centroid %.0f vs loud %.0f",
          (float64_t)quiet[F_CENTROID], (float64_t)loud[F_CENTROID]);
}

int main(void)
{
    test_sine_bands();
    test_block_sizes();
    test_dc_offset();
    test_silence_and_clipping();
    test_fft_vs_dft();
    test_quiet_keeps_shape();
    printf("sound_features: sizeof(sf_t) = %u bytes, %d features, %s\n", (unsigned)sizeof(sf_t), SF_N_FEATURES,
           fails ? "FAILED" : "all tests passed");
    return fails ? 1 : 0;
}
