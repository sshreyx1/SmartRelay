/*
 * test_sound_features.c - the streaming spectral features (firmware/sensing/sound_features.c) and
 * the Q15 FFT under them (sound_fft.c), on synthetic signals with known answers.
 */
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
static sf_t sf;
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

static void sine(double hz, double amp, double dc, unsigned seed, int noise)
{
    srand(seed);
    for (int i = 0; i < N; i++)
    {
        double v = dc + amp * sin(2 * M_PI * hz * i / 16000.0) + (noise ? (rand() % (2 * noise + 1)) - noise : 0);
        buf[i] = (uint16_t)(v < 0 ? 0 : v > 4095 ? 4095 : lround(v));
    }
}

static void features(uint32_t chunk, float out[SF_N_FEATURES])
{
    sf_reset(&sf);
    for (uint32_t off = 0; off < N; off += chunk)
    {
        sf_add(&sf, buf + off, N - off < chunk ? N - off : chunk);
    }
    sf_finish(&sf, out);
}

static int dominant_band(const float* f)
{
    int best = 0;
    for (int b = 1; b < SF_N_BANDS; b++)
    {
        if (f[F_BAND0 + b] > f[F_BAND0 + best])
        {
            best = b;
        }
    }
    return best;
}

static void test_sine_bands(void)
{
    float f[SF_N_FEATURES];
    sine(1000, 800, 2100, 1, 3);
    features(4096, f);
    CHECK(dominant_band(f) == 3, "1 kHz: dominant band %d, want 3 (1-2k)", dominant_band(f));
    CHECK(fabsf(f[F_CENTROID] - 1000.0f) < 100.0f, "1 kHz: centroid %.1f Hz", (double)f[F_CENTROID]);

    sine(6000, 800, 2100, 2, 3);
    features(4096, f);
    CHECK(dominant_band(f) == 7, "6 kHz: dominant band %d, want 7 (5.5-8k)", dominant_band(f));
}

static void test_block_sizes(void)
{
    float ref[SF_N_FEATURES], f[SF_N_FEATURES];
    sine(700, 500, 2083, 3, 40);
    features(N, ref);
    const uint32_t sizes[] = {1, 7, 256, 1000, 8192};
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
    {
        features(sizes[s], f);
        CHECK(memcmp(f, ref, sizeof(f)) == 0, "block size %u changes the output", sizes[s]);
    }
}

static void test_dc_offset(void)
{
    float a[SF_N_FEATURES], b[SF_N_FEATURES];
    sine(1500, 300, 2048, 4, 30);
    features(4096, a);
    for (int i = 0; i < N; i++)
    {
        buf[i] = (uint16_t)(buf[i] + 60);
    }
    features(4096, b);
    for (int k = 0; k < F_LOGRMS + 1; k++) /* log_rms is DC-corrected too, so all 25 must hold */
    {
        CHECK(fabsf(a[k] - b[k]) <= 1e-3f * (1.0f + fabsf(a[k])), "+60 DC changes %s: %g -> %g", SF_FEATURE_NAMES[k],
              (double)a[k], (double)b[k]);
    }
}

static void test_silence_and_clipping(void)
{
    float f[SF_N_FEATURES];
    for (int i = 0; i < N; i++)
    {
        buf[i] = 2100;
    }
    features(4096, f);
    for (int k = 0; k < SF_N_FEATURES; k++)
    {
        CHECK(isfinite(f[k]), "silence: %s not finite", SF_FEATURE_NAMES[k]);
    }
    CHECK(f[F_LOGRMS] == 0.0f, "silence: log_rms %g", (double)f[F_LOGRMS]);
    for (int i = 0; i < N; i++)
    {
        buf[i] = (i / 8) & 1 ? 4095 : 0;
    }
    features(4096, f);
    for (int k = 0; k < SF_N_FEATURES; k++)
    {
        CHECK(isfinite(f[k]), "clipped: %s not finite", SF_FEATURE_NAMES[k]);
    }
    sf_reset(&sf);
    sf_add(&sf, buf, 100); /* less than one frame */
    sf_finish(&sf, f);
    for (int k = 0; k < SF_N_FEATURES; k++)
    {
        CHECK(isfinite(f[k]), "short: %s not finite", SF_FEATURE_NAMES[k]);
    }
}

static void test_fft_vs_dft(void)
{
    int16_t re[SOUND_FFT_N], im[SOUND_FFT_N];
    double x[SOUND_FFT_N];
    srand(5);
    for (int n = 0; n < SOUND_FFT_N; n++)
    {
        x[n] = 9000 * sin(2 * M_PI * 20.3 * n / SOUND_FFT_N) + 3000 * cos(2 * M_PI * 77 * n / SOUND_FFT_N) +
               (rand() % 201) - 100;
        re[n] = (int16_t)lround(x[n]);
        im[n] = 0;
    }
    sound_fft256(re, im);
    double p_fft = 0, p_dft = 0;
    int peak_fft = 0, peak_dft = 0;
    double best_fft = -1, best_dft = -1;
    for (int k = 1; k <= SOUND_FFT_N / 2; k++)
    {
        double dr = 0, di = 0;
        for (int n = 0; n < SOUND_FFT_N; n++)
        {
            dr += x[n] * cos(2 * M_PI * k * n / SOUND_FFT_N);
            di -= x[n] * sin(2 * M_PI * k * n / SOUND_FFT_N);
        }
        double pd = dr * dr + di * di;
        double fr = re[k] * 256.0, fi = im[k] * 256.0; /* undo the documented 1/256 scaling */
        double pf = fr * fr + fi * fi;
        p_dft += pd;
        p_fft += pf;
        if (pd > best_dft)
        {
            best_dft = pd;
            peak_dft = k;
        }
        if (pf > best_fft)
        {
            best_fft = pf;
            peak_fft = k;
        }
    }
    CHECK(peak_fft == peak_dft, "FFT peak bin %d, DFT %d", peak_fft, peak_dft);
    CHECK(fabs(p_fft - p_dft) / p_dft < 0.05, "FFT power off by %.2f%%", 100 * fabs(p_fft - p_dft) / p_dft);
}

static void test_quiet_keeps_shape(void)
{
    /* block floating point: a 4-count tone must give the same spectral shape as an 800-count one */
    float loud[SF_N_FEATURES], quiet[SF_N_FEATURES];
    sine(2500, 800, 2100, 6, 0);
    features(4096, loud);
    sine(2500, 4, 2100, 6, 0);
    features(4096, quiet);
    CHECK(dominant_band(quiet) == 4, "quiet 2.5 kHz: dominant band %d, want 4", dominant_band(quiet));
    CHECK(fabsf(loud[F_CENTROID] - quiet[F_CENTROID]) < 150.0f, "quiet centroid %.0f vs loud %.0f",
          (double)quiet[F_CENTROID], (double)loud[F_CENTROID]);
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
