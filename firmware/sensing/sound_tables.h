/*
 * sound_tables.h - Q15 constant tables for the sound FFT and window (generated: sound_tables.c,
 * tools/gen_sound_tables.py). Pure C.
 */
#ifndef SOUND_TABLES_H
#define SOUND_TABLES_H

#include <stdint.h>

#define SOUND_FFT_N 256
#define SOUND_FFT_LOG2N 8

extern const int16_t g_sound_twiddle_cos[SOUND_FFT_N / 2];
extern const int16_t g_sound_twiddle_sin[SOUND_FFT_N / 2];
extern const int16_t g_sound_hann_q15[SOUND_FFT_N];

#endif
