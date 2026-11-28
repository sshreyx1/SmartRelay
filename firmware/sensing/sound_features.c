/*
 * sound_features.c - see sound_features.h.
 */
#include "sound_features.h"

#include <math.h>
#include <string.h>

#include "sound_fft.h"

#define BIN_HZ ((float)SF_SAMPLE_RATE / (float)SF_FRAME) /* 62.5 */

const char* const g_sf_feature_names[SF_N_FEATURES] = {
    "band_62_250_mean", "band_250_500_mean", "band_500_1k_mean", "band_1k_2k_mean", "band_2k_3k_mean",
    "band_3k_4k_mean",  "band_4k_5k5_mean",  "band_5k5_8k_mean", "band_62_250_std", "band_250_500_std",
    "band_500_1k_std",  "band_1k_2k_std",    "band_2k_3k_std",   "band_3k_4k_std",  "band_4k_5k5_std",
    "band_5k5_8k_std",  "centroid_mean",     "centroid_std",     "zcr_mean",        "zcr_std",
    "loud_std",         "loud_range",        "active_frac",      "flux_mean",       "log_rms",
};

/* first FFT bin of each band; the band ends where the next one starts (last ends after bin 128) */
static const uint8_t k_band_lo[SF_N_BANDS + 1] = { 1, 4, 8, 16, 32, 48, 64, 88, 129 };

static void welford(double* p_mean, double* p_sum_sq, uint32_t count, double value_x)
{
    const double delta = value_x - *p_mean;
    *p_mean += delta / (double)count;
    *p_sum_sq += delta * (value_x - *p_mean);
}

void sf_reset(sf_t* p_feat)
{
    memset(p_feat, 0, sizeof(*p_feat));
}

static void process_frame(sf_t* p_feat)
{
    int32_t sum = 0;
    for (uint32_t pos = 0; pos < SF_FRAME; pos++)
    {
        sum += p_feat->frame[pos];
    }
    const int32_t mean = (sum + SF_FRAME / 2) / SF_FRAME;

    /* pass 1: centred samples -> energy, zero crossings, peak of the windowed frame */
    uint64_t energy = 0;
    uint32_t crossings = 0;
    int32_t prev = 0, peak = 0;
    for (uint32_t pos = 0; pos < SF_FRAME; pos++)
    {
        const int32_t centred = (int32_t)p_feat->frame[pos] - mean;
        energy += (uint64_t)((int64_t)centred * centred);
        if (pos && ((centred >= 0) != (prev >= 0)))
        {
            crossings++;
        }
        prev = centred;
        const int32_t windowed = centred * g_sound_hann_q15[pos]; /* |c| <= 4095, so |w| < 2^27 */
        const int32_t magnitude = windowed < 0 ? -windowed : windowed;
        if (magnitude > peak)
        {
            peak = magnitude;
        }
    }
    /* pass 2: largest shift g with peak << g < 2^29, so every FFT input is <= 2^14 */
    uint32_t shift = 0;
    if (peak)
    {
        /* peak >= 0 (a maximum of absolute values), so the unsigned shift is exact */
        while (((uint64_t)peak << (shift + 1u)) < ((uint64_t)1u << 29))
        {
            shift++;
        }
    }
    for (uint32_t pos = 0; pos < SF_FRAME; pos++)
    {
        const int32_t windowed = ((int32_t)p_feat->frame[pos] - mean) * g_sound_hann_q15[pos];
        p_feat->real[pos] = (int16_t)sound_asr32(windowed * (int32_t)(1u << shift) + SOUND_Q15_HALF, 15u);
        p_feat->imag[pos] = 0;
    }
    sound_fft256(p_feat->real, p_feat->imag);

    uint64_t band[SF_N_BANDS] = { 0 };
    uint64_t total = 0, weighted = 0;
    for (uint32_t band_idx = 0; band_idx < SF_N_BANDS; band_idx++)
    {
        for (uint32_t bin_idx = k_band_lo[band_idx]; bin_idx < k_band_lo[band_idx + 1]; bin_idx++)
        {
            const uint32_t power = (uint32_t)((int32_t)p_feat->real[bin_idx] * p_feat->real[bin_idx]) + (uint32_t)((int32_t)p_feat->imag[bin_idx] * p_feat->imag[bin_idx]);
            band[band_idx] += power;
            weighted += (uint64_t)power * bin_idx;
        }
        total += band[band_idx];
    }

    p_feat->n_frames++;
    const uint32_t n_frames = p_feat->n_frames;
    const float log_tot = logf((float)total + 1.0f);
    float flux = 0.0f;
    for (uint32_t band_idx = 0; band_idx < SF_N_BANDS; band_idx++)
    {
        const float shape = logf((float)band[band_idx] + 1.0f) - log_tot;
        welford(&p_feat->band_mean[band_idx], &p_feat->band_m2[band_idx], n_frames, (double)shape);
        flux += fabsf(shape - p_feat->prev_shape[band_idx]);
        p_feat->prev_shape[band_idx] = shape;
    }
    if (n_frames > 1)
    {
        p_feat->flux_sum += (double)(flux / (float)SF_N_BANDS);
    }
    const float centroid = total ? (float)((double)weighted / (double)total) * BIN_HZ : 0.0f;
    welford(&p_feat->cen_mean, &p_feat->cen_m2, n_frames, (double)centroid);
    welford(&p_feat->zcr_mean, &p_feat->zcr_m2, n_frames, (double)((float)crossings / (float)(SF_FRAME - 1)));

    const float loud = 10.0f * log10f((float)energy / (float)SF_FRAME + 1.0f);
    welford(&p_feat->loud_mean, &p_feat->loud_m2, n_frames, (double)loud);
    if ((1 == n_frames) || (loud < p_feat->loud_min))
    {
        p_feat->loud_min = loud;
    }
    if ((1 == n_frames) || (loud > p_feat->loud_max))
    {
        p_feat->loud_max = loud;
    }
    uint32_t bucket = (uint32_t)(loud / SF_HIST_DB);
    p_feat->hist[bucket < SF_HIST_BUCKETS ? bucket : SF_HIST_BUCKETS - 1]++;
}

void sf_add(sf_t* p_feat, const uint16_t* p_samples, uint32_t count)
{
    for (uint32_t idx = 0; idx < count; idx++)
    {
        const uint16_t value = p_samples[idx];
        p_feat->sum += value;
        p_feat->sum_sq += (uint64_t)value * value;
        p_feat->frame[p_feat->fill++] = value;
        if (SF_FRAME == p_feat->fill)
        {
            process_frame(p_feat);
            p_feat->fill = 0;
        }
    }
    p_feat->n_samples += count;
}

static float stdev(double sum_sq, uint32_t count)
{
    return (float)sqrt(sum_sq / (double)count);
}

float sf_rms_dc_corrected(const sf_t* p_feat)
{
    if (!p_feat->n_samples)
    {
        return 0.0f;
    }
    const double pos = (double)p_feat->n_samples;
    const double mean = (double)p_feat->sum / pos;
    const double var = (double)p_feat->sum_sq / pos - mean * mean;
    return var > 0.0 ? (float)sqrt(var) : 0.0f;
}

float sf_zcr_per_s(const sf_t* p_feat)
{
    return (float)p_feat->zcr_mean * (float)SF_SAMPLE_RATE;
}

void sf_finish(const sf_t* p_feat, float out[SF_N_FEATURES])
{
    memset(out, 0, sizeof(float) * SF_N_FEATURES);
    const uint32_t pos = p_feat->n_frames;
    if (!pos)
    {
        return;
    }
    for (uint32_t band_idx = 0; band_idx < SF_N_BANDS; band_idx++)
    {
        out[band_idx] = (float)p_feat->band_mean[band_idx];
        out[SF_N_BANDS + band_idx] = stdev(p_feat->band_m2[band_idx], pos);
    }
    float* p_out_vec = out + 2 * SF_N_BANDS;
    p_out_vec[0] = (float)p_feat->cen_mean;
    p_out_vec[1] = stdev(p_feat->cen_m2, pos);
    p_out_vec[2] = (float)p_feat->zcr_mean;
    p_out_vec[3] = stdev(p_feat->zcr_m2, pos);
    p_out_vec[4] = stdev(p_feat->loud_m2, pos);
    p_out_vec[5] = p_feat->loud_max - p_feat->loud_min;

    /* quietest 10%: the first bucket where the running count reaches n/10; active = 6 dB (3 buckets) above it */
    uint32_t cum = 0, b10 = 0;
    for (b10 = 0; b10 < SF_HIST_BUCKETS; b10++)
    {
        cum += p_feat->hist[b10];
        if (cum * 10u >= pos)
        {
            break;
        }
    }
    uint32_t active = 0;
    for (uint32_t band_idx = b10 + (uint32_t)(SF_ACTIVE_DB / SF_HIST_DB); band_idx < SF_HIST_BUCKETS; band_idx++)
    {
        active += p_feat->hist[band_idx];
    }
    p_out_vec[6] = (float)active / (float)pos;
    p_out_vec[7] = pos > 1 ? (float)(p_feat->flux_sum / (double)(pos - 1)) : 0.0f;
    p_out_vec[8] = logf(sf_rms_dc_corrected(p_feat) + 1.0f);
}
