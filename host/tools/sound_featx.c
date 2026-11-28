/*
 * sound_featx.c - run the node's feature extractor (firmware/sensing/sound_features.c) over raw
 * recordings on the laptop and print CSV. Training uses these numbers, never a Python copy, so
 * the model sees exactly what the Pico computes.
 *
 *   sound_featx [--header] [--chunk N] file.npy ...
 *
 * Each file is a 1-D numpy array of little-endian uint16 ADC samples ('<u2', as saved by
 * ml/pico_audio.py). Output, one row per file:
 *   file,<SF_FEATURE_NAMES...>,legacy_energy,legacy_zcr
 * legacy_* are mic.c's energy and zcr (firmware/sensing/audio_features.c), for the "before" baseline.
 * --chunk feeds sf_add() N samples at a time (default 4096, like the DMA ring); the output must not
 * depend on it. Exit code 1 if any file could not be read (its row is skipped).
 *
 * Portable C99: no mmap, getopt or unistd.h, so it builds with MSVC too.
 */
#include "float_types.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_features.h"
#include "sound_features.h"

#define MAX_SAMPLES (16000u * 60u)

static uint16_t s_buf[MAX_SAMPLES];
static sf_t s_sf;

/* Returns the number of samples read, or -1 with a message on stderr. */
static int32_t read_npy(const char* p_path, uint16_t* p_out, uint32_t max)
{
    FILE* p_file = fopen(p_path, "rb");
    if (!p_file)
    {
        fprintf(stderr, "%s: cannot open\n", p_path);
        return -1;
    }
    unsigned char pre[12];
    int32_t result = -1;
    if ((10 != fread(pre, 1, 10, p_file)) || (0 != memcmp(pre, "\x93NUMPY", 6)))
    {
        fprintf(stderr, "%s: not a .npy file\n", p_path);
        goto done;
    }
    uint32_t hlen = (uint32_t)pre[8] | ((uint32_t)pre[9] << 8);
    if (pre[6] >= 2)
    {
        if (2 != fread(pre + 10, 1, 2, p_file))
        {
            goto done;
        }
        hlen |= ((uint32_t)pre[10] << 16) | ((uint32_t)pre[11] << 24);
    }
    if ((0 == hlen) || (hlen > 4096))
    {
        fprintf(stderr, "%s: bad header length\n", p_path);
        goto done;
    }
    char header[4097];
    if (fread(header, 1, hlen, p_file) != hlen)
    {
        goto done;
    }
    header[hlen] = '\0';
    if ((!strstr(header, "'<u2'")) || (!strstr(header, "'fortran_order': False")))
    {
        fprintf(stderr, "%s: need a 1-D '<u2' (uint16 little-endian) array, header: %s\n", p_path, header);
        goto done;
    }
    const char* p_shape = strstr(header, "'shape': (");
    if (!p_shape)
    {
        goto done;
    }
    uint32_t count = (uint32_t)strtoul(p_shape + 10, NULL, 10);
    if (count > max)
    {
        fprintf(stderr, "%s: %" PRIu32 " samples, more than %u\n", p_path, count, max);
        goto done;
    }
    unsigned char* p_bytes = (unsigned char*)p_out;
    if (fread(p_bytes, 2, count, p_file) != count)
    {
        fprintf(stderr, "%s: file shorter than its header says\n", p_path);
        goto done;
    }
    for (uint32_t idx = 0; idx < count; idx++) /* little-endian on disk, whatever this machine is */
    {
        p_out[idx] = (uint16_t)(p_bytes[2 * idx] | (p_bytes[2 * idx + 1] << 8));
    }
    result = (int32_t)count;
done:
    fclose(p_file);
    return result;
}

int main(int argc, char** pp_argv)
{
    int header = 0, failed = 0, files = 0;
    uint32_t chunk = 4096;
    for (int idx = 1; idx < argc; idx++)
    {
        if (0 == strcmp(pp_argv[idx], "--header"))
        {
            header = 1;
        }
        else if ((0 == strcmp(pp_argv[idx], "--chunk")) && (idx + 1 < argc))
        {
            chunk = (uint32_t)strtoul(pp_argv[++idx], NULL, 10);
            if (!chunk)
            {
                chunk = 1;
            }
        }
        else if ((0 == strcmp(pp_argv[idx], "--help")) || (0 == strcmp(pp_argv[idx], "-h")))
        {
            printf("usage: sound_featx [--header] [--chunk N] file.npy ...\n"
                   "CSV of the %d sound features (v%d) + legacy mic.c energy/zcr per file\n",
                   SF_N_FEATURES, SF_VERSION);
            return 0;
        }
        else
        {
            /* not an option: a file name, read in the loop below */
        }
    }
    if (header)
    {
        printf("file");
        for (int kdx = 0; kdx < SF_N_FEATURES; kdx++)
        {
            printf(",%s", g_sf_feature_names[kdx]);
        }
        printf(",legacy_energy,legacy_zcr\n");
    }
    for (int idx = 1; idx < argc; idx++)
    {
        if (0 == strcmp(pp_argv[idx], "--header"))
        {
            continue;
        }
        if (0 == strcmp(pp_argv[idx], "--chunk"))
        {
            idx++;
            continue;
        }
        files++;
        int32_t count = read_npy(pp_argv[idx], s_buf, MAX_SAMPLES);
        if (count < 0)
        {
            failed = 1;
            continue;
        }
        sf_reset(&s_sf);
        audio_feat_t legacy;
        audio_feat_reset(&legacy);
        for (uint32_t off = 0; off < (uint32_t)count; off += chunk)
        {
            uint32_t len = (uint32_t)count - off < chunk ? (uint32_t)count - off : chunk;
            sf_add(&s_sf, s_buf + off, len);
            audio_feat_add(&legacy, s_buf + off, len);
        }
        float32_t features[SF_N_FEATURES];
        sf_finish(&s_sf, features);
        audio_result_t audio;
        audio_feat_finish(&legacy, SF_SAMPLE_RATE, &audio);
        printf("%s", pp_argv[idx]);
        for (int kdx = 0; kdx < SF_N_FEATURES; kdx++)
        {
            printf(",%.9g", (float64_t)features[kdx]);
        }
        printf(",%.9g,%.9g\n", (float64_t)audio.energy, (float64_t)audio.zcr);
    }
    if ((!files) && (!header))
    {
        fprintf(stderr, "usage: sound_featx [--header] [--chunk N] file.npy ...\n");
        return 2;
    }
    return failed;
}
