/*
 * sound_fft.c - see sound_fft.h.
 */
#include "sound_fft.h"

static uint32_t bitrev8(uint32_t value)
{
    value = ((value & 0xF0u) >> 4) | ((value & 0x0Fu) << 4);
    value = ((value & 0xCCu) >> 2) | ((value & 0x33u) << 2);
    value = ((value & 0xAAu) >> 1) | ((value & 0x55u) << 1);
    return value;
}

void sound_fft256(int16_t* p_real, int16_t* p_imag)
{
    for (uint32_t idx = 0; idx < SOUND_FFT_N; idx++)
    {
        uint32_t jdx = bitrev8(idx);
        if (jdx > idx)
        {
            int16_t swap = p_real[idx];
            p_real[idx] = p_real[jdx];
            p_real[jdx] = swap;
            swap = p_imag[idx];
            p_imag[idx] = p_imag[jdx];
            p_imag[jdx] = swap;
        }
    }
    /* decimation in time; W = cos - j*sin; each stage: a' = (a + W*b) / 2, b' = (a - W*b) / 2 */
    for (uint32_t half = 1, step = SOUND_FFT_N / 2; half < SOUND_FFT_N; half <<= 1, step >>= 1)
    {
        for (uint32_t kdx = 0; kdx < half; kdx++)
        {
            const int32_t w_real = g_sound_twiddle_cos[kdx * step];
            const int32_t w_imag = -g_sound_twiddle_sin[kdx * step];
            for (uint32_t idx = kdx; idx < SOUND_FFT_N; idx += half << 1)
            {
                const uint32_t jdx = idx + half;
                /* Q15 complex multiply with rounding; sound_asr32() is a well-defined arithmetic shift */
                const int32_t twid_real = sound_asr32(w_real * p_real[jdx] - w_imag * p_imag[jdx] + SOUND_Q15_HALF, 15u);
                const int32_t twid_imag = sound_asr32(w_real * p_imag[jdx] + w_imag * p_real[jdx] + SOUND_Q15_HALF, 15u);
                const int32_t real_a = p_real[idx], imag_a = p_imag[idx];
                p_real[idx] = (int16_t)sound_asr32(real_a + twid_real, 1u);
                p_imag[idx] = (int16_t)sound_asr32(imag_a + twid_imag, 1u);
                p_real[jdx] = (int16_t)sound_asr32(real_a - twid_real, 1u);
                p_imag[jdx] = (int16_t)sound_asr32(imag_a - twid_imag, 1u);
            }
        }
    }
}
