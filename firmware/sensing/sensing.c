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
#include "hardware/timer.h"
#include "mic.h"
#include "pico/stdlib.h"
#include "pir.h"
#include "sound.h"
#include "sound_classes.h"
#include "sound_classify.h"
#include "sound_features.h"
#include "weather_tree.h"

static sensing_hooks_t s_h;
static sf_t s_sf; /* 1.9 kB: static, not on the 1024-word sensors task stack */
static sensing_sound_t s_sound;
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

static uint16_t to_u16_meta(float v)
{
    /* 0xFFFF is CM_MISSING_U16, so a real reading saturates one below it */
    return (uint16_t)(v >= 65534.0f ? 65534.0f : v <= 0.0f ? 0.0f : v + 0.5f);
}

bool sensing_record_sound(uint32_t seconds, audio_result_t* audio)
{
    sensing_sound_t* r = &s_sound;
    *r = (sensing_sound_t){.ok = false, .label_id = SC_UNKNOWN, .conf = SC_UNKNOWN};
    audio_timing_t tm;
    if (!audio_capture_sf(seconds, s_h.wait_ms, audio, &r->lost, &s_sf, &tm))
    {
        audio->samples = 0;
        printf("[AUDIO] no free DMA channel, sound features missing\n");
        return false;
    }
    const uint32_t t0 = time_us_32();
    float f[SF_N_FEATURES];
    sf_finish(&s_sf, f);
    sound_classify(f, &r->label_id, &r->conf);
    r->model_us = time_us_32() - t0;
    r->rms = to_u16_meta(sf_rms_dc_corrected(&s_sf));
    r->zcr_per_s = to_u16_meta(sf_zcr_per_s(&s_sf));
    r->feat_us = tm.feat_us;
    r->maxcall_us = tm.maxcall_us;
    r->ok = true;
    if (r->lost)
    {
        printf("[AUDIO] warning: %lu samples lost (task too slow)\n", (unsigned long)r->lost);
    }
    return true;
}

const sensing_sound_t* sensing_last_sound(void)
{
    return &s_sound;
}

void sensing_log_sound(void)
{
    const sensing_sound_t* r = &s_sound;
    if (!r->ok)
    {
        printf("[SOUND] capture failed: label unknown\n");
        return;
    }
    printf("[SOUND] group=%s label=%s id=%u conf=%u rms=%u zcr=%u lost=%lu feat_us=%lu maxcall_us=%lu model_us=%lu\n",
           sound_class_name(sound_class_group(r->label_id)), sound_class_name(r->label_id), r->label_id, r->conf, r->rms,
           r->zcr_per_s, (unsigned long)r->lost, (unsigned long)r->feat_us, (unsigned long)r->maxcall_us,
           (unsigned long)r->model_us);
    const uint32_t budget_us = AUDIO_SECONDS * 1000000u / 4u; /* 25% of the recording */
    if (r->feat_us + r->model_us > budget_us)
    {
        printf("[SOUND] warning: feature + model time %lu us is over the 25%% CPU budget (%lu us)\n",
               (unsigned long)(r->feat_us + r->model_us), (unsigned long)budget_us);
    }
    if (r->maxcall_us > 100000u)
    {
        printf("[SOUND] warning: one feature call took %lu us, eating into the ~512 ms ring slack\n",
               (unsigned long)r->maxcall_us);
    }
}

void sensing_capture(uint32_t now_ms, uint8_t trigger_flags, capture_meta_t* m, audio_result_t* audio)
{
    capture_meta_clear(m);
    m->trigger_flags = trigger_flags;
    m->uptime_ms = now_ms;

    /* on failure the sound fields keep capture_meta_clear()'s unknown / missing values */
    if (sensing_record_sound(AUDIO_SECONDS, audio))
    {
        m->sound_class = s_sound.label_id;
        m->sound_conf = s_sound.conf;
        m->sound_rms = s_sound.rms;
        m->sound_zcr = s_sound.zcr_per_s;
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
