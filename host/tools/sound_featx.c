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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_features.h"
#include "sound_features.h"

#define MAX_SAMPLES (16000u * 60u)

static uint16_t s_buf[MAX_SAMPLES];
static sf_t s_sf;

/* Returns the number of samples read, or -1 with a message on stderr. */
static long read_npy(const char* path, uint16_t* out, uint32_t max)
{
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "%s: cannot open\n", path);
        return -1;
    }
    unsigned char pre[12];
    long result = -1;
    if (fread(pre, 1, 10, f) != 10 || memcmp(pre, "\x93NUMPY", 6) != 0)
    {
        fprintf(stderr, "%s: not a .npy file\n", path);
        goto done;
    }
    uint32_t hlen = (uint32_t)pre[8] | ((uint32_t)pre[9] << 8);
    if (pre[6] >= 2)
    {
        if (fread(pre + 10, 1, 2, f) != 2)
        {
            goto done;
        }
        hlen |= ((uint32_t)pre[10] << 16) | ((uint32_t)pre[11] << 24);
    }
    if (hlen == 0 || hlen > 4096)
    {
        fprintf(stderr, "%s: bad header length\n", path);
        goto done;
    }
    char header[4097];
    if (fread(header, 1, hlen, f) != hlen)
    {
        goto done;
    }
    header[hlen] = '\0';
    if (!strstr(header, "'<u2'") || !strstr(header, "'fortran_order': False"))
    {
        fprintf(stderr, "%s: need a 1-D '<u2' (uint16 little-endian) array, header: %s\n", path, header);
        goto done;
    }
    const char* shape = strstr(header, "'shape': (");
    if (!shape)
    {
        goto done;
    }
    unsigned long n = strtoul(shape + 10, NULL, 10);
    if (n > max)
    {
        fprintf(stderr, "%s: %lu samples, more than %u\n", path, n, max);
        goto done;
    }
    unsigned char* bytes = (unsigned char*)out;
    if (fread(bytes, 2, n, f) != n)
    {
        fprintf(stderr, "%s: file shorter than its header says\n", path);
        goto done;
    }
    for (unsigned long i = 0; i < n; i++) /* little-endian on disk, whatever this machine is */
    {
        out[i] = (uint16_t)(bytes[2 * i] | (bytes[2 * i + 1] << 8));
    }
    result = (long)n;
done:
    fclose(f);
    return result;
}

int main(int argc, char** argv)
{
    int header = 0, failed = 0, files = 0;
    uint32_t chunk = 4096;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--header") == 0)
        {
            header = 1;
        }
        else if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc)
        {
            chunk = (uint32_t)strtoul(argv[++i], NULL, 10);
            if (!chunk)
            {
                chunk = 1;
            }
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
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
        for (int k = 0; k < SF_N_FEATURES; k++)
        {
            printf(",%s", SF_FEATURE_NAMES[k]);
        }
        printf(",legacy_energy,legacy_zcr\n");
    }
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--header") == 0)
        {
            continue;
        }
        if (strcmp(argv[i], "--chunk") == 0)
        {
            i++;
            continue;
        }
        files++;
        long n = read_npy(argv[i], s_buf, MAX_SAMPLES);
        if (n < 0)
        {
            failed = 1;
            continue;
        }
        sf_reset(&s_sf);
        audio_feat_t legacy;
        audio_feat_reset(&legacy);
        for (uint32_t off = 0; off < (uint32_t)n; off += chunk)
        {
            uint32_t len = (uint32_t)n - off < chunk ? (uint32_t)n - off : chunk;
            sf_add(&s_sf, s_buf + off, len);
            audio_feat_add(&legacy, s_buf + off, len);
        }
        float f[SF_N_FEATURES];
        sf_finish(&s_sf, f);
        audio_result_t a;
        audio_feat_finish(&legacy, SF_SAMPLE_RATE, &a);
        printf("%s", argv[i]);
        for (int k = 0; k < SF_N_FEATURES; k++)
        {
            printf(",%.9g", (double)f[k]);
        }
        printf(",%.9g,%.9g\n", (double)a.energy, (double)a.zcr);
    }
    if (!files && !header)
    {
        fprintf(stderr, "usage: sound_featx [--header] [--chunk N] file.npy ...\n");
        return 2;
    }
    return failed;
}
