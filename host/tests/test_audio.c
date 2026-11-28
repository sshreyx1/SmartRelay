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
    for (int idx = 0; idx < N; idx++)
    {
        double value = 2048;
        switch (kind)
        {
        case 0: /* quiet room: small noise around mid-scale */
            value += (rand() % 41) - 20;
            break;
        case 1: /* loud low tone, 400 Hz, big amplitude -> energy > 50000 */
            value += 1500 * sin(2 * M_PI * 400 * idx / 16000.0);
            break;
        case 2: /* rustle: medium white noise -> many crossings */
            value += (rand() % 401) - 200;
            break;
        case 3: /* DC offset (mic biased at 1.25 V instead of 1.65 V) */
            value = 1551 + (rand() % 21) - 10;
            break;
        default: /* clipped extremes */
            value = (idx & 1) ? 4095 : 0;
            break;
        }
        if (value < 0)
        {
            value = 0;
        }
        if (value > 4095)
        {
            value = 4095;
        }
        buf[idx] = (uint16_t)value;
    }
}

static void check(int kind, unsigned seed, uint32_t block)
{
    gen(kind, seed);
    SoundAnalysis ref = mic_analyze_buffer(buf, N); /* Rhaylene's code, unchanged */
    audio_feat_t feat;
    audio_feat_reset(&feat);
    for (uint32_t idx = 0; idx < N; idx += block)
    {
        uint32_t chunk = (N - idx < block) ? N - idx : block;
        audio_feat_add(&feat, buf + idx, chunk);
    }
    audio_result_t res;
    audio_feat_finish(&feat, 16000, &res);
    int ok_flag = res.energy == ref.energy && res.zcr == ref.zcr && res.class_id == ref.class_id && strcmp(res.p_label, ref.p_label) == 0;
    printf("%s kind %d block %5u: mine %s e=%.3f z=%.5f | mic.c %s e=%.3f z=%.5f | rms %u zcr/s %u mean %u\n",
           ok_flag ? "ok  " : "FAIL", kind, block, res.p_label, res.energy, res.zcr, ref.p_label, ref.energy, ref.zcr, res.rms,
           res.zcr_per_s, res.mean_adc);
    if (!ok_flag)
    {
        fails++;
    }
}

int main(void)
{
    const uint32_t blocks[] = {1, 7, 256, 4093, 8192, N};
    for (int kind = 0; kind < 5; kind++)
    {
        for (unsigned block_idx = 0; block_idx < sizeof(blocks) / sizeof(blocks[0]); block_idx++)
        {
            check(kind, 1234u + (unsigned)kind, blocks[block_idx]);
        }
    }
    printf(fails ? "AUDIO FEATURES: %d FAILED\n" : "AUDIO FEATURES: all identical to mic.c%.0d\n", fails);
    return fails ? 1 : 0;
}
