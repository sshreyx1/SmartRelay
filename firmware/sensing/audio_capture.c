/*
 * audio_capture.c - see audio_capture.h.
 *
 * ADC free-running on input 0 at AUDIO_SAMPLE_RATE -> ADC FIFO -> DMA (16-bit, write address
 * wraps every 16 kB) -> s_ring. The DMA transfer count is the exact number of samples, so the
 * hardware stops by itself. This task reads behind the DMA write position and feeds the samples
 * to audio_feat_add(). It only has to come back within ~0.5 s (8192 samples) to lose nothing.
 */
#include "audio_capture.h"

#include "board.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/timer.h"

#define RING_SAMPLES 8192u
#define RING_BYTES (RING_SAMPLES * 2u) /* 16384 = 2^14 */
#define RING_BITS 14u
#define SAFETY 256u /* stay this far behind the DMA so it never writes where we read */

static uint16_t s_ring[RING_SAMPLES] __attribute__((aligned(RING_BYTES)));

bool audio_capture(uint32_t seconds, void (*wait_ms)(uint32_t ms), audio_result_t* out, uint32_t* lost)
{
    return audio_capture_sf(seconds, wait_ms, out, lost, NULL, NULL);
}

bool audio_capture_sf(uint32_t seconds, void (*wait_ms)(uint32_t ms), audio_result_t* out, uint32_t* lost, sf_t* sf,
                      audio_timing_t* timing)
{
    audio_timing_t tm = { 0, 0, 0 };
    if (sf)
    {
        sf_reset(sf);
    }
    int ch = dma_claim_unused_channel(false);
    if (ch < 0)
    {
        return false;
    }
    const uint32_t total = seconds * AUDIO_SAMPLE_RATE;
    uint32_t dropped = 0;

    adc_select_input(BOARD_MIC_ADC);
    adc_fifo_setup(true, true, 1, false, false); /* FIFO on, DREQ on, threshold 1, no error bit, 12-bit */
    adc_set_clkdiv(48000000.0f / (float)AUDIO_SAMPLE_RATE - 1.0f); /* one sample every (1+div) 48 MHz clocks */
    adc_fifo_drain();

    dma_channel_config c = dma_channel_get_default_config((uint)ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, RING_BITS);
    channel_config_set_dreq(&c, DREQ_ADC);
    dma_channel_configure((uint)ch, &c, s_ring, &adc_hw->fifo, total, true);
    adc_run(true);

    audio_feat_t f;
    audio_feat_reset(&f);
    uint32_t done = 0;
    while (done < total)
    {
        uint32_t avail = total - dma_channel_hw_addr((uint)ch)->transfer_count;
        if (avail - done > RING_SAMPLES - SAFETY)
        {
            uint32_t skip = (avail - done) - (RING_SAMPLES - SAFETY);
            dropped += skip;
            done += skip;
        }
        const uint32_t batch_start = done;
        while (done < avail)
        {
            uint32_t idx = done % RING_SAMPLES;
            uint32_t n = avail - done;
            if (n > RING_SAMPLES - idx)
            {
                n = RING_SAMPLES - idx;
            }
            audio_feat_add(&f, &s_ring[idx], n);
            if (sf)
            {
                const uint32_t t0 = time_us_32();
                sf_add(sf, &s_ring[idx], n);
                const uint32_t dt = time_us_32() - t0;
                tm.feat_us += dt;
                tm.maxcall_us = dt > tm.maxcall_us ? dt : tm.maxcall_us;
                tm.calls++;
            }
            done += n;
        }
        /* if this task was held up WHILE reading, the DMA may have lapped samples we then read:
         * count them as lost so the warning tells the truth */
        uint32_t now_written = total - dma_channel_hw_addr((uint)ch)->transfer_count;
        if (now_written > batch_start + RING_SAMPLES)
        {
            uint32_t bad = now_written - RING_SAMPLES - batch_start;
            dropped += bad < (done - batch_start) ? bad : (done - batch_start);
        }
        if (done < total)
        {
            wait_ms(4);
        }
    }

    adc_run(false);
    dma_channel_abort((uint)ch);
    dma_channel_unclaim((uint)ch);
    adc_fifo_setup(false, false, 0, false, false); /* back to plain adc_read() mode */
    adc_set_clkdiv(0.0f);
    adc_fifo_drain();

    audio_feat_finish(&f, AUDIO_SAMPLE_RATE, out);
    if (lost)
    {
        *lost = dropped;
    }
    if (timing)
    {
        *timing = tm;
    }
    return true;
}
