/*
 * sound_fft.h - in-place radix-2 FFT, N = 256, int16 Q15, for the RP2040 (no FPU).   Owner: Rhaylene.
 *
 * Scaling: every one of the 8 butterfly stages halves its outputs (>>1), so the result is the
 * DFT divided by 256:  X[k] = (1/256) * sum_n x[n] * exp(-2*pi*j*k*n/256).
 * That can never overflow as long as every input sample has |x| < 16384 (sound_features.c
 * guarantees this by normalising each frame). All arithmetic is integer and identical on the
 * Pico and the laptop, so host tests see exactly what the node computes.
 *
 * Pure C, no Pico headers.
 */
#ifndef SOUND_FFT_H
#define SOUND_FFT_H

#include <stdint.h>

#include "sound_tables.h"

/* Rounding constant for a Q15 product: 0.5 in Q15 (2^14). Spelled out so no shift touches a
 * signed value (BARR-C 5.3.b). */
#define SOUND_Q15_HALF ((int32_t)16384)

/*
 * sound_asr32 - arithmetic shift right of a signed value: floor(value / 2^shift), shift 0..31.
 *
 * NOTE: ">>" on a negative signed integer is implementation-defined in C99 (BARR-C 5.3.b bans
 * bitwise operators on signed data). This gives the same result as gcc/clang's arithmetic shift
 * on every input, using only well-defined operations: for value < 0, -(value + 1) is in
 * 0..INT32_MAX, so it converts to uint32_t and back without loss.
 */
static inline int32_t sound_asr32(int32_t value, uint32_t shift)
{
    int32_t result;
    if (value < 0)
    {
        result = -(int32_t)((uint32_t)(-(value + 1)) >> shift) - 1;
    }
    else
    {
        result = (int32_t)((uint32_t)value >> shift);
    }
    return result;
}

/* re/im: SOUND_FFT_N values each, transformed in place (output in natural order). */
void sound_fft256(int16_t* p_real, int16_t* p_imag);

#endif
