/*
 * audio_features.c - see audio_features.h.
 */
#include "float_types.h"
#include "audio_features.h"

#include <math.h>

void audio_feat_reset (audio_feat_t* p_feat)
{
    p_feat->sum_sq    = 0;
    p_feat->sum       = 0;
    p_feat->crossings = 0;
    p_feat->count     = 0;
    p_feat->prev      = 0;
}

void audio_feat_add (audio_feat_t* p_feat, const uint16_t* p_samples,
                     uint32_t count)
{
    for (uint32_t idx = 0; idx < count; idx++)
    {
        int16_t centered = (int16_t)((int16_t)p_samples[idx] - 2048);
        p_feat->sum_sq += (uint64_t)((int32_t)centered * centered);
        p_feat->sum += p_samples[idx];
        if (p_feat->count > 0)
        {
            int16_t prev = p_feat->prev;
            if (((centered >= 0) && (prev < 0)) ||
                ((centered < 0) && (prev >= 0)))
            {
                p_feat->crossings++;
            }
        }
        p_feat->prev = centered;
        p_feat->count++;
    }
}

void audio_feat_finish (const audio_feat_t* p_feat, uint32_t sample_rate,
                        audio_result_t* p_out)
{
    p_out->samples = p_feat->count;
    if (0 == p_feat->count)
    {
        p_out->energy    = 0.0f;
        p_out->zcr       = 0.0f;
        p_out->class_id  = 0;
        p_out->p_label   = "AMBIENT_NOISE";
        p_out->rms       = 0;
        p_out->zcr_per_s = 0;
        p_out->mean_adc  = 0;
        return;
    }
    /* mic.c: (float)(energy_acc / num_samples) with a double accumulator
     * holding exact integers */
    p_out->energy =
        (float32_t)((float64_t)p_feat->sum_sq / (float64_t)(int)p_feat->count);
    p_out->zcr = (float32_t)p_feat->crossings / (float32_t)(int)p_feat->count;

    if (p_out->energy > AUDIO_ENERGY_LOUD)
    {
        p_out->class_id = 1;
        p_out->p_label  = "LOUD_ANIMAL_CALL";
    }
    else if (p_out->zcr > AUDIO_ZCR_RUSTLE)
    {
        p_out->class_id = 2;
        p_out->p_label  = "HIGH_FREQ_RUSTLE";
    }
    else
    {
        p_out->class_id = 0;
        p_out->p_label  = "AMBIENT_NOISE";
    }

    float32_t rms    = sqrtf(p_out->energy);
    p_out->rms       = (uint16_t)(rms > 65534.0f ? 65534.0f : rms + 0.5f);
    float32_t zps    = p_out->zcr * (float32_t)sample_rate;
    p_out->zcr_per_s = (uint16_t)(zps > 65534.0f ? 65534.0f : zps + 0.5f);
    p_out->mean_adc  = (uint16_t)(p_feat->sum / p_feat->count);
}

/*** end of file ***/

