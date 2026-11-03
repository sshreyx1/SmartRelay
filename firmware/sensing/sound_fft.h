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

/* re/im: SOUND_FFT_N values each, transformed in place (output in natural order). */
void sound_fft256(int16_t* re, int16_t* im);

#endif
