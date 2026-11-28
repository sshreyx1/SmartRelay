/*
 * sensortest_main.c - sensor_test.uf2: checks Maryam's and Rhaylene's sensors
 * on the real wiring, with no radios and no SD card involved. Flash this first
 * on every node.
 *
 * Every second it prints PIR (GP0), sound trigger (GP14), light (GP27) and the
 * DHT22 (GP4) reading with the weather tree's label. When PIR and the sound
 * trigger are high together it records 5 s of audio exactly like the node
 * firmware and prints the sound class.
 *
 * Keys (USB serial, 115200):  m = record 5 s of audio now   q = 1 s quick mic
 *                             level
 */
#include <inttypes.h>
#include <stdio.h>

#include "audio_capture.h"
#include "board.h"
#include "pico/stdlib.h"
#include "sensing.h"

static void wait_ms (uint32_t msec)
{
    sleep_ms(msec);
}

static void record (uint32_t seconds)
{
    audio_result_t audio;
    printf("recording %" PRIu32 "s...\n", (uint32_t)seconds);
    if (!sensing_record_sound(seconds, &audio))
    {
        printf("no DMA channel\n");
        return;
    }
    printf("[AUDIO] %s (class %d) energy %.1f zcr %.3f (%u crossings/s) rms "
           "%u, mean ADC %u, %" PRIu32 " samples, %" PRIu32 " lost\n",
           audio.p_label, audio.class_id, audio.energy, audio.zcr,
           audio.zcr_per_s, audio.rms, audio.mean_adc, (uint32_t)audio.samples,
           (uint32_t)sensing_last_sound()->lost);
    sensing_log_sound();
    if ((audio.mean_adc < 1700) || (audio.mean_adc > 2400))
    {
        printf("        note: the mic's resting level is %u, not ~2048 (1.65 "
               "V). mic.c subtracts 2048, so the\n"
               "        energy includes this offset and quiet rooms may read "
               "as LOUD. Tell Rhaylene.\n",
               audio.mean_adc);
    }
}

int main (void)
{
    board_spi1_idle(); /* keep the LoRa module and the SD card quiet */
    stdio_init_all();
    sleep_ms(2000);
    printf("\n=== CSC09 sensor test (no radios, no SD) ===\n");
    printf(
        "PIR GP0 | sound trigger GP14 | mic GP26 | DHT22 GP4 | light GP27\n");
    printf("keys: m = record 5 s of audio, q = 1 s mic level\n");

    const sensing_hooks_t hooks = {wait_ms, NULL, NULL};
    sensing_init(&hooks);

    uint32_t last_print = 0;
    for (;;)
    {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        sensing_env_poll(now);
        if (now - last_print >= 1000)
        {
            last_print                 = now;
            const sensing_env_t* p_env = sensing_env();
            if (p_env->b_dht_ok)
            {
                printf("PIR %d | sound %d | light %4u | %.1f C %.1f %%RH | "
                       "weather %s | dht ok %" PRIu32 " fail %" PRIu32 "\n",
                       sensing_pir(), sensing_sound_trigger(), p_env->light,
                       p_env->temp_c, p_env->hum_pct,
                       sensing_weather_name(p_env->weather),
                       (uint32_t)p_env->dht_good, (uint32_t)p_env->dht_fail);
            }
            else
            {
                printf("PIR %d | sound %d | light %4u | DHT22 no reading yet | "
                       "dht ok %" PRIu32 " fail %" PRIu32 "\n",
                       sensing_pir(), sensing_sound_trigger(), p_env->light,
                       (uint32_t)p_env->dht_good, (uint32_t)p_env->dht_fail);
            }
        }
        if (sensing_dual_trigger(now))
        {
            printf("\n[ALERT] PIR + sound together: this is what makes a node "
                   "capture\n");
            capture_meta_t meta;
            audio_result_t audio;
            sensing_capture(now, CM_TRIG_PIR | CM_TRIG_SOUND, &meta, &audio);
            printf(
                "[AUDIO] %s (class %d) energy %.1f zcr %.3f rms %u mean %u\n",
                audio.p_label, audio.class_id, audio.energy, audio.zcr,
                audio.rms, audio.mean_adc);
            sensing_log_sound();
        }
        int chan = getchar_timeout_us(0);
        if ('m' == chan)
        {
            record(AUDIO_SECONDS);
        }
        else if ('q' == chan)
        {
            record(1);
        }
        else
        {
            /* no key, or a key with no command: ignore */
        }
        sleep_ms(10);
    }
}

/*** end of file ***/

