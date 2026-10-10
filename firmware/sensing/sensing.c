/*
 * sensing.c - see sensing.h.
 */
#include "sensing.h"

#include <math.h>
#include <stdio.h>

#include "DHT22.h"
#include "audio_capture.h"
#include "board.h"
#include "hardware/adc.h"
#include "mic.h"
#include "pico/stdlib.h"
#include "pir.h"
#include "sound.h"
#include "weather_tree.h"

static sensing_hooks_t s_h;
static sensing_env_t s_env = {.weather = -1};
static uint32_t s_last_dht_try, s_last_dht_good;
static bool s_have_dht;
static uint32_t s_cooldown_until;

static uint16_t read_light(void)
{
    adc_select_input(BOARD_LIGHT_ADC);
    return adc_read();
}

static void update_weather(uint32_t now_ms)
{
    s_env.dht_ok = s_have_dht && (now_ms - s_last_dht_good) < SENSING_DHT_STALE_MS;
    if (s_env.dht_ok)
    {
        float f[3] = {s_env.temp_c, s_env.hum_pct, (float)s_env.light};
        s_env.weather = weather_predict(f);
    }
    else
    {
        s_env.weather = -1;
    }
}

void sensing_init(const sensing_hooks_t* hooks)
{
    s_h = *hooks;
    pir_init();   /* Maryam: GP0 input, pull-down */
    sound_init(); /* Rhaylene: GP14 input, pull-down */
    mic_init();   /* Rhaylene: adc_init() + GP26 as ADC */
    DHT_init();   /* Maryam: GP4 input, pull-up */
    adc_gpio_init(BOARD_LIGHT_GPIO);
    s_env.light = read_light();
    /* DHT22.c's own 2 s guard starts at 0 ms, so the first read is allowed 2 s after boot. */
    s_last_dht_try = 0;
}

bool sensing_env_poll(uint32_t now_ms)
{
    if (now_ms - s_last_dht_try < SENSING_DHT_PERIOD_MS)
    {
        return false;
    }
    s_last_dht_try = now_ms;
    s_env.light = read_light();

    float t = 0.0f, h = 0.0f;
    if (s_h.timing_begin)
    {
        s_h.timing_begin();
    }
    bool ok = DHT_read(&t, &h);
    if (s_h.timing_end)
    {
        s_h.timing_end();
    }
    /* DHT22.c does not decode negative temperatures (bit 15); Singapore never needs it, but a
     * reading with that bit set would be nonsense, so it is rejected with the out-of-range check. */
    if (ok && t > -40.0f && t < 80.0f && h >= 0.0f && h <= 100.0f)
    {
        s_env.temp_c = t;
        s_env.hum_pct = h;
        s_have_dht = true;
        s_last_dht_good = now_ms;
        s_env.dht_good++;
    }
    else
    {
        s_env.dht_fail++;
    }
    update_weather(now_ms);
    return true;
}

const sensing_env_t* sensing_env(void)
{
    return &s_env;
}

bool sensing_pir(void)
{
    return pir_is_motion_detected();
}

bool sensing_sound_trigger(void)
{
    return sound_is_triggered();
}

bool sensing_dual_trigger(uint32_t now_ms)
{
    /* exactly their main.c rule: both pins high in the same 10 ms poll, then a 5 s cooldown */
    if ((int32_t)(now_ms - s_cooldown_until) < 0)
    {
        return false;
    }
    return sound_is_triggered() && pir_is_motion_detected();
}

static int16_t to_x10_i16(float v)
{
    return (int16_t)lroundf(v * 10.0f);
}

void sensing_capture(uint32_t now_ms, uint8_t trigger_flags, capture_meta_t* m, audio_result_t* audio)
{
    capture_meta_clear(m);
    m->trigger_flags = trigger_flags;
    m->uptime_ms = now_ms;

    uint32_t lost = 0;
    if (audio_capture(AUDIO_SECONDS, s_h.wait_ms, audio, &lost))
    {
        m->sound_class = (uint8_t)audio->class_id;
        m->sound_rms = audio->rms;
        m->sound_zcr = audio->zcr_per_s;
        /* sound_conf stays "unknown": the decision tree in mic.c gives a class, not a confidence */
        if (lost)
        {
            printf("[AUDIO] warning: %lu samples lost (task too slow)\n", (unsigned long)lost);
        }
    }
    else
    {
        audio->samples = 0;
        printf("[AUDIO] no free DMA channel, sound features missing\n");
    }

    /* their main.c reads the light again after the recording for the event snapshot */
    s_env.light = read_light();
    uint32_t t = to_ms_since_boot(get_absolute_time());
    update_weather(t);
    m->light_raw = s_env.light;
    if (s_env.dht_ok)
    {
        m->temp_c_x10 = to_x10_i16(s_env.temp_c);
        m->humidity_x10 = (uint16_t)lroundf(s_env.hum_pct * 10.0f);
        m->weather_label = (uint8_t)s_env.weather;
        /* weather_conf / trigger_trust stay "unknown": the tree outputs a label only */
    }
    s_cooldown_until = t + SENSING_COOLDOWN_MS;
}

const char* sensing_weather_name(int w)
{
    return w < 0 ? "unknown (no DHT22 reading)" : weather_label(w);
}
