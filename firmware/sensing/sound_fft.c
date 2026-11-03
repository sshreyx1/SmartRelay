/*
 * sound_fft.c - see sound_fft.h.
 */
#include "sound_fft.h"

static uint32_t bitrev8(uint32_t v)
{
    v = ((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4);
    v = ((v & 0xCCu) >> 2) | ((v & 0x33u) << 2);
    v = ((v & 0xAAu) >> 1) | ((v & 0x55u) << 1);
    return v;
}

void sound_fft256(int16_t* re, int16_t* im)
{
    for (uint32_t i = 0; i < SOUND_FFT_N; i++)
    {
        uint32_t j = bitrev8(i);
        if (j > i)
        {
            int16_t t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    /* decimation in time; W = cos - j*sin; each stage: a' = (a + W*b) / 2, b' = (a - W*b) / 2 */
    for (uint32_t half = 1, step = SOUND_FFT_N / 2; half < SOUND_FFT_N; half <<= 1, step >>= 1)
    {
        for (uint32_t k = 0; k < half; k++)
        {
            const int32_t wr = sound_twiddle_cos[k * step];
            const int32_t wi = -sound_twiddle_sin[k * step];
            for (uint32_t i = k; i < SOUND_FFT_N; i += half << 1)
            {
                const uint32_t j = i + half;
                /* Q15 complex multiply with rounding; the shift is arithmetic on every target we build for */
                const int32_t tr = (wr * re[j] - wi * im[j] + (1 << 14)) >> 15;
                const int32_t ti = (wr * im[j] + wi * re[j] + (1 << 14)) >> 15;
                const int32_t ar = re[i], ai = im[i];
                re[i] = (int16_t)((ar + tr) >> 1);
                im[i] = (int16_t)((ai + ti) >> 1);
                re[j] = (int16_t)((ar - tr) >> 1);
                im[j] = (int16_t)((ai - ti) >> 1);
            }
        }
    }
}
