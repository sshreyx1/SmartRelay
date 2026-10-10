/*
 * audio_features.c - see audio_features.h.
 */
#include "audio_features.h"

#include <math.h>

void audio_feat_reset(audio_feat_t* f)
{
    f->sum_sq = 0;
    f->sum = 0;
    f->crossings = 0;
    f->n = 0;
    f->prev = 0;
}

void audio_feat_add(audio_feat_t* f, const uint16_t* s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
    {
        int16_t centered = (int16_t)((int16_t)s[i] - 2048);
        f->sum_sq += (uint64_t)((int32_t)centered * centered);
        f->sum += s[i];
        if (f->n > 0)
        {
            int16_t p = f->prev;
            if ((centered >= 0 && p < 0) || (centered < 0 && p >= 0))
            {
                f->crossings++;
            }
        }
        f->prev = centered;
        f->n++;
    }
}

void audio_feat_finish(const audio_feat_t* f, uint32_t sample_rate, audio_result_t* out)
{
    out->samples = f->n;
    if (f->n == 0)
    {
        out->energy = 0.0f;
        out->zcr = 0.0f;
        out->class_id = 0;
        out->label = "AMBIENT_NOISE";
        out->rms = 0;
        out->zcr_per_s = 0;
        out->mean_adc = 0;
        return;
    }
    /* mic.c: (float)(energy_acc / num_samples) with a double accumulator holding exact integers */
    out->energy = (float)((double)f->sum_sq / (double)(int)f->n);
    out->zcr = (float)f->crossings / (float)(int)f->n;

    if (out->energy > AUDIO_ENERGY_LOUD)
    {
        out->class_id = 1;
        out->label = "LOUD_ANIMAL_CALL";
    }
    else if (out->zcr > AUDIO_ZCR_RUSTLE)
    {
        out->class_id = 2;
        out->label = "HIGH_FREQ_RUSTLE";
    }
    else
    {
        out->class_id = 0;
        out->label = "AMBIENT_NOISE";
    }

    float rms = sqrtf(out->energy);
    out->rms = (uint16_t)(rms > 65534.0f ? 65534.0f : rms + 0.5f);
    float zps = out->zcr * (float)sample_rate;
    out->zcr_per_s = (uint16_t)(zps > 65534.0f ? 65534.0f : zps + 0.5f);
    out->mean_adc = (uint16_t)(f->sum / f->n);
}
