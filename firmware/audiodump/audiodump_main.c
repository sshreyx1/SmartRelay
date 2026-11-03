/*
 * audiodump_main.c - audio_dump.uf2: record the real mic (GP26 / ADC0) at 16 kHz and send the raw
 * samples to the laptop, so the classifier is trained on exactly what the node hears (MAX9814 AGC,
 * bias, RC filter, noise). No FreeRTOS, no radios, no SD: the sensor_test pattern.
 *
 * Unlike audio_capture.c (a 16 kB ring, read while it fills), DMA writes straight into one 160 kB
 * buffer, so nothing can be lost. That only fits because this image has nothing else in RAM.
 *
 * Commands (USB serial, one per line, \n or \r\n):
 *   r<sec>   record 1-5 s, then send it:
 *              @AUD BEGIN rate=16000 n=<N> mean=<mean ADC>
 *              @A <base64 of the little-endian uint16 samples, 64 chars per line>
 *              ...
 *              @AUD END n=<N> crc=<crc32 of those bytes, 8 hex digits>
 *   s        send the last recording again (the laptop asks if the CRC failed)
 *   f        @FEAT v,v,...                  on-device feature vector of the last recording (sound_features.c)
 *   c        @CLS id=.. name=.. conf=..     on-device classification of the last recording
 *   p        @PONG
 * Base64 text instead of raw bytes because Pico stdio turns \n into \r\n.
 * Receiver: ml/pico_audio.py.
 */
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "pico/stdlib.h"
#include "sound_classes.h"
#include "sound_classify.h"
#include "sound_features.h"

#define RATE 16000u
#define MAX_SECONDS 5u
#define MAX_SAMPLES (RATE * MAX_SECONDS)
#define B64_LINE_BYTES 48u /* -> 64 base64 characters */

static uint16_t s_buf[MAX_SAMPLES];
static uint32_t s_n;
static sf_t s_sf;

static bool record(uint32_t seconds)
{
    int ch = dma_claim_unused_channel(false);
    if (ch < 0)
    {
        return false;
    }
    const uint32_t n = seconds * RATE;

    /* the same ADC setup as audio_capture.c */
    adc_select_input(BOARD_MIC_ADC);
    adc_fifo_setup(true, true, 1, false, false);
    adc_set_clkdiv(48000000.0f / (float)RATE - 1.0f);
    adc_fifo_drain();

    dma_channel_config c = dma_channel_get_default_config((uint)ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, DREQ_ADC);
    dma_channel_configure((uint)ch, &c, s_buf, &adc_hw->fifo, n, true);
    adc_run(true);
    dma_channel_wait_for_finish_blocking((uint)ch);

    adc_run(false);
    dma_channel_unclaim((uint)ch);
    adc_fifo_setup(false, false, 0, false, false);
    adc_set_clkdiv(0.0f);
    adc_fifo_drain();
    s_n = n;
    return true;
}

static uint32_t crc32_bytes(const uint8_t* p, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu; /* zlib / binascii.crc32 */
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void b64_line(const uint8_t* p, uint32_t len)
{
    static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char out[4 + (B64_LINE_BYTES / 3u) * 4u + 2u];
    uint32_t o = 0;
    out[o++] = '@';
    out[o++] = 'A';
    out[o++] = ' ';
    for (uint32_t i = 0; i < len; i += 3)
    {
        uint32_t rem = len - i;
        uint32_t v = (uint32_t)p[i] << 16;
        if (rem > 1)
        {
            v |= (uint32_t)p[i + 1] << 8;
        }
        if (rem > 2)
        {
            v |= p[i + 2];
        }
        out[o++] = k[(v >> 18) & 63u];
        out[o++] = k[(v >> 12) & 63u];
        out[o++] = rem > 1 ? k[(v >> 6) & 63u] : '=';
        out[o++] = rem > 2 ? k[v & 63u] : '=';
    }
    out[o] = '\0';
    puts(out);
}

static void send_recording(void)
{
    uint64_t sum = 0;
    for (uint32_t i = 0; i < s_n; i++)
    {
        sum += s_buf[i];
    }
    const uint8_t* bytes = (const uint8_t*)s_buf; /* RP2040 is little-endian */
    const uint32_t len = s_n * 2u;
    printf("@AUD BEGIN rate=%u n=%lu mean=%lu\n", RATE, (unsigned long)s_n, (unsigned long)(sum / s_n));
    for (uint32_t off = 0; off < len; off += B64_LINE_BYTES)
    {
        b64_line(bytes + off, len - off < B64_LINE_BYTES ? len - off : B64_LINE_BYTES);
    }
    printf("@AUD END n=%lu crc=%08lx\n", (unsigned long)s_n, (unsigned long)crc32_bytes(bytes, len));
    fflush(stdout);
}

/* Runs the streaming extractor over the stored recording in DMA-ring-sized chunks, exactly as the
 * node does while recording. */
static void compute_features(float out[SF_N_FEATURES])
{
    sf_reset(&s_sf);
    for (uint32_t off = 0; off < s_n; off += 4096u)
    {
        sf_add(&s_sf, s_buf + off, s_n - off < 4096u ? s_n - off : 4096u);
    }
    sf_finish(&s_sf, out);
}

static void send_features(void)
{
    float f[SF_N_FEATURES];
    compute_features(f);
    printf("@FEAT ");
    for (int i = 0; i < SF_N_FEATURES; i++)
    {
        printf(i ? ",%.9g" : "%.9g", (double)f[i]);
    }
    printf("\n");
}

static void send_class(void)
{
    float f[SF_N_FEATURES];
    compute_features(f);
    uint8_t id = SC_UNKNOWN, conf = SC_UNKNOWN;
    uint32_t t0 = time_us_32();
    sound_classify(f, &id, &conf);
    uint32_t us = time_us_32() - t0;
    printf("@CLS id=%u name=%s group=%s conf=%u model_us=%lu\n", id, sound_class_name(id),
           sound_class_name(sound_class_group(id)), conf, (unsigned long)us);
}

static void handle(const char* line)
{
    if (line[0] == 'p' && line[1] == '\0')
    {
        printf("@PONG\n");
    }
    else if (line[0] == 'r')
    {
        int sec = 0;
        if (sscanf(line + 1, "%d", &sec) != 1 || sec < 1 || sec > (int)MAX_SECONDS)
        {
            printf("@ERR seconds must be 1-%u\n", MAX_SECONDS);
        }
        else if (!record((uint32_t)sec))
        {
            printf("@ERR no free DMA channel\n");
        }
        else
        {
            send_recording();
        }
    }
    else if (!s_n && (line[0] == 's' || line[0] == 'f' || line[0] == 'c'))
    {
        printf("@ERR no recording yet\n");
    }
    else if (line[0] == 's' && line[1] == '\0')
    {
        send_recording();
    }
    else if (line[0] == 'f' && line[1] == '\0')
    {
        send_features();
    }
    else if (line[0] == 'c' && line[1] == '\0')
    {
        send_class();
    }
    else
    {
        printf("@ERR unknown command '%s' (r<sec> s f c p)\n", line);
    }
    fflush(stdout);
}

int main(void)
{
    board_spi1_idle(); /* keep the LoRa module and the SD card quiet */
    stdio_init_all();
    adc_init();
    adc_gpio_init(26); /* GP26 = ADC0, the MAX9814 */
    sleep_ms(1500);
    printf("@READY audio_dump rate=%u max_s=%u features=v%u n=%u\n", RATE, MAX_SECONDS, SF_VERSION, SF_N_FEATURES);

    char line[32];
    uint32_t len = 0;
    for (;;)
    {
        int ch = getchar_timeout_us(100000);
        if (ch == PICO_ERROR_TIMEOUT)
        {
            continue;
        }
        if (ch == '\n' || ch == '\r')
        {
            if (len)
            {
                line[len] = '\0';
                handle(line);
                len = 0;
            }
        }
        else if (len < sizeof(line) - 1u)
        {
            line[len++] = (char)ch;
        }
    }
}
