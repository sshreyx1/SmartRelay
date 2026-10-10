/*
 * test_audio.c - the streaming audio features (firmware/sensing/audio_features.c) must give
 * exactly what Rhaylene's mic_analyze_buffer() gives on the same samples, however the samples
 * are split into blocks (the DMA ring hands them over in pieces).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_features.h"
#include "mic.h"

#define N (16000 * 5)
static uint16_t buf[N];

static int fails;

static void gen(int kind, unsigned seed)
{
    srand(seed);
    for (int i = 0; i < N; i++)
    {
        double v = 2048;
        switch (kind)
        {
        case 0: /* quiet room: small noise around mid-scale */
            v += (rand() % 41) - 20;
            break;
        case 1: /* loud low tone, 400 Hz, big amplitude -> energy > 50000 */
            v += 1500 * sin(2 * M_PI * 400 * i / 16000.0);
            break;
        case 2: /* rustle: medium white noise -> many crossings */
            v += (rand() % 401) - 200;
            break;
        case 3: /* DC offset (mic biased at 1.25 V instead of 1.65 V) */
            v = 1551 + (rand() % 21) - 10;
            break;
        default: /* clipped extremes */
            v = (i & 1) ? 4095 : 0;
            break;
        }
        if (v < 0)
        {
            v = 0;
        }
        if (v > 4095)
        {
            v = 4095;
        }
        buf[i] = (uint16_t)v;
    }
}

static void check(int kind, unsigned seed, uint32_t block)
{
    gen(kind, seed);
    SoundAnalysis ref = mic_analyze_buffer(buf, N); /* Rhaylene's code, unchanged */
    audio_feat_t f;
    audio_feat_reset(&f);
    for (uint32_t i = 0; i < N; i += block)
    {
        uint32_t n = (N - i < block) ? N - i : block;
        audio_feat_add(&f, buf + i, n);
    }
    audio_result_t r;
    audio_feat_finish(&f, 16000, &r);
    int ok = r.energy == ref.energy && r.zcr == ref.zcr && r.class_id == ref.class_id && strcmp(r.label, ref.label) == 0;
    printf("%s kind %d block %5u: mine %s e=%.3f z=%.5f | mic.c %s e=%.3f z=%.5f | rms %u zcr/s %u mean %u\n",
           ok ? "ok  " : "FAIL", kind, block, r.label, r.energy, r.zcr, ref.label, ref.energy, ref.zcr, r.rms,
           r.zcr_per_s, r.mean_adc);
    if (!ok)
    {
        fails++;
    }
}

int main(void)
{
    const uint32_t blocks[] = {1, 7, 256, 4093, 8192, N};
    for (int kind = 0; kind < 5; kind++)
    {
        for (unsigned b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++)
        {
            check(kind, 1234u + (unsigned)kind, blocks[b]);
        }
    }
    printf(fails ? "AUDIO FEATURES: %d FAILED\n" : "AUDIO FEATURES: all identical to mic.c%.0d\n", fails);
    return fails ? 1 : 0;
}
