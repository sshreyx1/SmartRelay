/*
 * sound_features.c - see sound_features.h.
 */
#include "sound_features.h"

#include <math.h>
#include <string.h>

#include "sound_fft.h"

#define BIN_HZ ((float)SF_SAMPLE_RATE / (float)SF_FRAME) /* 62.5 */

const char* const SF_FEATURE_NAMES[SF_N_FEATURES] = {
    "band_62_250_mean",  "band_250_500_mean", "band_500_1k_mean", "band_1k_2k_mean",  "band_2k_3k_mean",
    "band_3k_4k_mean",   "band_4k_5k5_mean",  "band_5k5_8k_mean", "band_62_250_std",  "band_250_500_std",
    "band_500_1k_std",   "band_1k_2k_std",    "band_2k_3k_std",   "band_3k_4k_std",   "band_4k_5k5_std",
    "band_5k5_8k_std",   "centroid_mean",     "centroid_std",     "zcr_mean",         "zcr_std",
    "loud_std",          "loud_range",        "active_frac",      "flux_mean",        "log_rms",
};

/* first FFT bin of each band; the band ends where the next one starts (last ends after bin 128) */
static const uint8_t k_band_lo[SF_N_BANDS + 1] = {1, 4, 8, 16, 32, 48, 64, 88, 129};

static void welford(double* mean, double* m2, uint32_t n, double x)
{
    const double d = x - *mean;
    *mean += d / (double)n;
    *m2 += d * (x - *mean);
}

void sf_reset(sf_t* s)
{
    memset(s, 0, sizeof(*s));
}

static void process_frame(sf_t* s)
{
    int32_t sum = 0;
    for (uint32_t n = 0; n < SF_FRAME; n++)
    {
        sum += s->frame[n];
    }
    const int32_t mean = (sum + SF_FRAME / 2) / SF_FRAME;

    /* pass 1: centred samples -> energy, zero crossings, peak of the windowed frame */
    uint64_t energy = 0;
    uint32_t crossings = 0;
    int32_t prev = 0, peak = 0;
    for (uint32_t n = 0; n < SF_FRAME; n++)
    {
        const int32_t c = (int32_t)s->frame[n] - mean;
        energy += (uint64_t)((int64_t)c * c);
        if (n && ((c >= 0) != (prev >= 0)))
        {
            crossings++;
        }
        prev = c;
        const int32_t w = c * sound_hann_q15[n]; /* |c| <= 4095, so |w| < 2^27 */
        const int32_t a = w < 0 ? -w : w;
        if (a > peak)
        {
            peak = a;
        }
    }
    /* pass 2: largest shift g with peak << g < 2^29, so every FFT input is <= 2^14 */
    uint32_t g = 0;
    if (peak)
    {
        while (((int64_t)peak << (g + 1)) < ((int64_t)1 << 29))
        {
            g++;
        }
    }
    for (uint32_t n = 0; n < SF_FRAME; n++)
    {
        const int32_t w = ((int32_t)s->frame[n] - mean) * sound_hann_q15[n];
        s->re[n] = (int16_t)((w * (int32_t)(1u << g) + (1 << 14)) >> 15);
        s->im[n] = 0;
    }
    sound_fft256(s->re, s->im);

    uint64_t band[SF_N_BANDS] = {0};
    uint64_t total = 0, weighted = 0;
    for (uint32_t b = 0; b < SF_N_BANDS; b++)
    {
        for (uint32_t k = k_band_lo[b]; k < k_band_lo[b + 1]; k++)
        {
            const uint32_t p = (uint32_t)((int32_t)s->re[k] * s->re[k]) + (uint32_t)((int32_t)s->im[k] * s->im[k]);
            band[b] += p;
            weighted += (uint64_t)p * k;
        }
        total += band[b];
    }

    s->n_frames++;
    const uint32_t nf = s->n_frames;
    const float log_tot = logf((float)total + 1.0f);
    float flux = 0.0f;
    for (uint32_t b = 0; b < SF_N_BANDS; b++)
    {
        const float shape = logf((float)band[b] + 1.0f) - log_tot;
        welford(&s->band_mean[b], &s->band_m2[b], nf, (double)shape);
        flux += fabsf(shape - s->prev_shape[b]);
        s->prev_shape[b] = shape;
    }
    if (nf > 1)
    {
        s->flux_sum += (double)(flux / (float)SF_N_BANDS);
    }
    const float centroid = total ? (float)((double)weighted / (double)total) * BIN_HZ : 0.0f;
    welford(&s->cen_mean, &s->cen_m2, nf, (double)centroid);
    welford(&s->zcr_mean, &s->zcr_m2, nf, (double)((float)crossings / (float)(SF_FRAME - 1)));

    const float loud = 10.0f * log10f((float)energy / (float)SF_FRAME + 1.0f);
    welford(&s->loud_mean, &s->loud_m2, nf, (double)loud);
    if (nf == 1 || loud < s->loud_min)
    {
        s->loud_min = loud;
    }
    if (nf == 1 || loud > s->loud_max)
    {
        s->loud_max = loud;
    }
    uint32_t bucket = (uint32_t)(loud / SF_HIST_DB);
    s->hist[bucket < SF_HIST_BUCKETS ? bucket : SF_HIST_BUCKETS - 1]++;
}

void sf_add(sf_t* s, const uint16_t* samples, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
    {
        const uint16_t v = samples[i];
        s->sum += v;
        s->sum_sq += (uint64_t)v * v;
        s->frame[s->fill++] = v;
        if (s->fill == SF_FRAME)
        {
            process_frame(s);
            s->fill = 0;
        }
    }
    s->n_samples += n;
}

static float stdev(double m2, uint32_t n)
{
    return (float)sqrt(m2 / (double)n);
}

float sf_rms_dc_corrected(const sf_t* s)
{
    if (!s->n_samples)
    {
        return 0.0f;
    }
    const double n = (double)s->n_samples;
    const double mean = (double)s->sum / n;
    const double var = (double)s->sum_sq / n - mean * mean;
    return var > 0.0 ? (float)sqrt(var) : 0.0f;
}

float sf_zcr_per_s(const sf_t* s)
{
    return (float)s->zcr_mean * (float)SF_SAMPLE_RATE;
}

void sf_finish(const sf_t* s, float out[SF_N_FEATURES])
{
    memset(out, 0, sizeof(float) * SF_N_FEATURES);
    const uint32_t n = s->n_frames;
    if (!n)
    {
        return;
    }
    for (uint32_t b = 0; b < SF_N_BANDS; b++)
    {
        out[b] = (float)s->band_mean[b];
        out[SF_N_BANDS + b] = stdev(s->band_m2[b], n);
    }
    float* o = out + 2 * SF_N_BANDS;
    o[0] = (float)s->cen_mean;
    o[1] = stdev(s->cen_m2, n);
    o[2] = (float)s->zcr_mean;
    o[3] = stdev(s->zcr_m2, n);
    o[4] = stdev(s->loud_m2, n);
    o[5] = s->loud_max - s->loud_min;

    /* quietest 10%: the first bucket where the running count reaches n/10; active = 6 dB (3 buckets) above it */
    uint32_t cum = 0, b10 = 0;
    for (b10 = 0; b10 < SF_HIST_BUCKETS; b10++)
    {
        cum += s->hist[b10];
        if (cum * 10u >= n)
        {
            break;
        }
    }
    uint32_t active = 0;
    for (uint32_t b = b10 + (uint32_t)(SF_ACTIVE_DB / SF_HIST_DB); b < SF_HIST_BUCKETS; b++)
    {
        active += s->hist[b];
    }
    o[6] = (float)active / (float)n;
    o[7] = n > 1 ? (float)(s->flux_sum / (double)(n - 1)) : 0.0f;
    o[8] = logf(sf_rms_dc_corrected(s) + 1.0f);
}
