/*
 * sensortest_main.c - sensor_test.uf2: checks Maryam's and Rhaylene's sensors on the real wiring,
 * with no radios and no SD card involved. Flash this first on every node.
 *
 * Every second it prints PIR (GP0), sound trigger (GP14), light (GP27) and the DHT22 (GP4)
 * reading with the weather tree's label. When PIR and the sound trigger are high together it
 * records 5 s of audio exactly like the node firmware and prints the sound class.
 *
 * Keys (USB serial, 115200):  m = record 5 s of audio now   q = 1 s quick mic level
 */
#include <stdio.h>

#include "audio_capture.h"
#include "board.h"
#include "pico/stdlib.h"
#include "sensing.h"

static void wait_ms(uint32_t ms)
{
    sleep_ms(ms);
}

static void record(uint32_t seconds)
{
    audio_result_t a;
    printf("recording %lus...\n", (unsigned long)seconds);
    if (!sensing_record_sound(seconds, &a))
    {
        printf("no DMA channel\n");
        return;
    }
    printf("[AUDIO] %s (class %d) energy %.1f zcr %.3f (%u crossings/s) rms %u, mean ADC %u, %lu samples, %lu lost\n",
           a.label, a.class_id, a.energy, a.zcr, a.zcr_per_s, a.rms, a.mean_adc, (unsigned long)a.samples,
           (unsigned long)sensing_last_sound()->lost);
    sensing_log_sound();
    if (a.mean_adc < 1700 || a.mean_adc > 2400)
    {
        printf("        note: the mic's resting level is %u, not ~2048 (1.65 V). mic.c subtracts 2048, so the\n"
               "        energy includes this offset and quiet rooms may read as LOUD. Tell Rhaylene.\n",
               a.mean_adc);
    }
}

int main(void)
{
    board_spi1_idle(); /* keep the LoRa module and the SD card quiet */
    stdio_init_all();
    sleep_ms(2000);
    printf("\n=== CSC09 sensor test (no radios, no SD) ===\n");
    printf("PIR GP0 | sound trigger GP14 | mic GP26 | DHT22 GP4 | light GP27\n");
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
            last_print = now;
            const sensing_env_t* e = sensing_env();
            if (e->dht_ok)
            {
                printf("PIR %d | sound %d | light %4u | %.1f C %.1f %%RH | weather %s | dht ok %lu fail %lu\n",
                       sensing_pir(), sensing_sound_trigger(), e->light, e->temp_c, e->hum_pct,
                       sensing_weather_name(e->weather), (unsigned long)e->dht_good, (unsigned long)e->dht_fail);
            }
            else
            {
                printf("PIR %d | sound %d | light %4u | DHT22 no reading yet | dht ok %lu fail %lu\n", sensing_pir(),
                       sensing_sound_trigger(), e->light, (unsigned long)e->dht_good, (unsigned long)e->dht_fail);
            }
        }
        if (sensing_dual_trigger(now))
        {
            printf("\n[ALERT] PIR + sound together: this is what makes a node capture\n");
            capture_meta_t m;
            audio_result_t a;
            sensing_capture(now, CM_TRIG_PIR | CM_TRIG_SOUND, &m, &a);
            printf("[AUDIO] %s (class %d) energy %.1f zcr %.3f rms %u mean %u\n", a.label, a.class_id, a.energy, a.zcr,
                   a.rms, a.mean_adc);
            sensing_log_sound();
        }
        int ch = getchar_timeout_us(0);
        if (ch == 'm')
        {
            record(AUDIO_SECONDS);
        }
        else if (ch == 'q')
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
