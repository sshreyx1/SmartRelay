/*
 * sensing.c - see sensing.h.
 */
#include "float_types.h"
#include <inttypes.h>
#include "sensing.h"

#include <math.h>
#include <stdio.h>

#include "dht22.h"
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
#if !SOUND_LEGACY_MODE
static sf_t s_sf; /* 1.9 kB: static, not on the 1024-word sensors task stack */
#else
static const char* s_legacy_label = "";
#endif
static sensing_sound_t s_sound;
static sensing_env_t s_env = { .weather = -1 };
static uint32_t s_last_dht_try;
static uint32_t s_last_dht_good;
static bool b_s_have_dht;
static uint32_t s_cooldown_until;

static uint16_t read_light(void)
{
    adc_select_input(BOARD_LIGHT_ADC);
    return adc_read();
}

static void update_weather(uint32_t now_ms)
{
    s_env.b_dht_ok = b_s_have_dht && ((now_ms - s_last_dht_good) < SENSING_DHT_STALE_MS);
    if (s_env.b_dht_ok)
    {
        float32_t features[3] = { s_env.temp_c, s_env.hum_pct, (float32_t)s_env.light };
        s_env.weather = weather_predict(features);
    }
    else
    {
        s_env.weather = -1;
    }
}

void sensing_init(const sensing_hooks_t* p_hooks)
{
    s_h = *p_hooks;
    pir_init();   /* Maryam: GP0 input, pull-down */
    sound_init(); /* Rhaylene: GP14 input, pull-down */
    mic_init();   /* Rhaylene: adc_init() + GP26 as ADC */
    dht_init();   /* Maryam: GP4 input, pull-up */
    adc_gpio_init(BOARD_LIGHT_GPIO);
    s_env.light = read_light();
    /* dht22.c's own 2 s guard starts at 0 ms, so the first read is allowed 2 s after boot. */
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

    float32_t temp_c = 0.0f;
    float32_t humidity = 0.0f;
    if (s_h.p_timing_begin)
    {
        s_h.p_timing_begin();
    }
    bool b_is_ok = dht_read(&temp_c, &humidity);
    if (s_h.p_timing_end)
    {
        s_h.p_timing_end();
    }
    /* dht22.c does not decode negative temperatures (bit 15); Singapore never needs it, but a
     * reading with that bit set would be nonsense, so it is rejected with the out-of-range check. */
    if (b_is_ok && (temp_c > -40.0f) && (temp_c < 80.0f) && (humidity >= 0.0f) && (humidity <= 100.0f))
    {
        s_env.temp_c = temp_c;
        s_env.hum_pct = humidity;
        b_s_have_dht = true;
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
    return (sound_is_triggered()) && (pir_is_motion_detected());
}

static int16_t to_x10_i16(float32_t value)
{
    return (int16_t)lroundf(value * 10.0f);
}

#if SOUND_LEGACY_MODE
bool sensing_record_sound(uint32_t seconds, audio_result_t* audio)
{
    sensing_sound_t* r = &s_sound;
    *r = (sensing_sound_t) { .ok = false, .label_id = SC_UNKNOWN, .conf = SC_UNKNOWN };
    if (!audio_capture(seconds, s_h.wait_ms, audio, &r->lost))
    {
        audio->samples = 0;
        printf("[AUDIO] no free DMA channel, sound features missing\n");
        return false;
    }
    s_legacy_label = audio->label;
    r->label_id = audio->class_id == 0 ? SC_AMBIENT : SC_UNKNOWN; /* 0 = mic.c's AMBIENT_NOISE */
    r->rms = audio->rms;
    r->zcr_per_s = audio->zcr_per_s;
    r->ok = true;
    if (r->lost)
    {
        printf("[AUDIO] warning: %" PRIu32 " samples lost (task too slow)\n", (uint32_t)r->lost);
    }
    return true;
}
#else
static uint16_t to_u16_meta(float32_t value)
{
    /* 0xFFFF is CM_MISSING_U16, so a real reading saturates one below it */
    return (uint16_t)(value >= 65534.0f ? 65534.0f : value <= 0.0f ? 0.0f : value + 0.5f);
}

bool sensing_record_sound(uint32_t seconds, audio_result_t* p_audio)
{
    sensing_sound_t* p_snd = &s_sound;
    *p_snd = (sensing_sound_t) { .b_is_ok = false, .label_id = SC_UNKNOWN, .conf = SC_UNKNOWN };
    audio_timing_t timing;
    if (!audio_capture_sf(seconds, s_h.p_wait_ms, p_audio, &p_snd->lost, &s_sf, &timing))
    {
        p_audio->samples = 0;
        printf("[AUDIO] no free DMA channel, sound features missing\n");
        return false;
    }
    const uint32_t start_t = time_us_32();
    float32_t features[SF_N_FEATURES];
    sf_finish(&s_sf, features);
    sound_classify(features, &p_snd->label_id, &p_snd->conf);
    p_snd->model_us = time_us_32() - start_t;
    p_snd->rms = to_u16_meta(sf_rms_dc_corrected(&s_sf));
    p_snd->zcr_per_s = to_u16_meta(sf_zcr_per_s(&s_sf));
    p_snd->feat_us = timing.feat_us;
    p_snd->maxcall_us = timing.maxcall_us;
    p_snd->b_is_ok = true;
    if (p_snd->lost)
    {
        printf("[AUDIO] warning: %" PRIu32 " samples lost (task too slow)\n", (uint32_t)p_snd->lost);
    }
    return true;
}
#endif

const sensing_sound_t* sensing_last_sound(void)
{
    return &s_sound;
}

void sensing_log_sound(void)
{
    const sensing_sound_t* p_snd = &s_sound;
    if (!p_snd->b_is_ok)
    {
        printf("[SOUND] capture failed: label unknown\n");
        return;
    }
#if SOUND_LEGACY_MODE
    printf("[SOUND] LEGACY mic.c thresholds: %s -> label=%s id=%u rms=%u zcr=%u lost=%" PRIu32 "\n", s_legacy_label,
           sound_class_name(r->label_id), r->label_id, r->rms, r->zcr_per_s, (uint32_t)r->lost);
    return;
#endif
    printf("[SOUND] group=%s label=%s id=%u conf=%u rms=%u zcr=%u lost=%" PRIu32 " feat_us=%" PRIu32 " maxcall_us=%" PRIu32 " model_us=%" PRIu32 "\n",
           sound_class_name(sound_class_group(p_snd->label_id)), sound_class_name(p_snd->label_id), p_snd->label_id, p_snd->conf,
           p_snd->rms, p_snd->zcr_per_s, (uint32_t)p_snd->lost, (uint32_t)p_snd->feat_us, (uint32_t)p_snd->maxcall_us,
           (uint32_t)p_snd->model_us);
    const uint32_t budget_us = AUDIO_SECONDS * 1000000u / 4u; /* 25% of the recording */
    if (p_snd->feat_us + p_snd->model_us > budget_us)
    {
        printf("[SOUND] warning: feature + model time %" PRIu32 " us is over the 25%% CPU budget (%" PRIu32 " us)\n",
               (uint32_t)(p_snd->feat_us + p_snd->model_us), (uint32_t)budget_us);
    }
    if (p_snd->maxcall_us > 100000u)
    {
        printf("[SOUND] warning: one feature call took %" PRIu32 " us, eating into the ~512 ms ring slack\n",
               (uint32_t)p_snd->maxcall_us);
    }
}

void sensing_capture(uint32_t now_ms, uint8_t trigger_flags, capture_meta_t* p_meta, audio_result_t* p_audio)
{
    capture_meta_clear(p_meta);
    p_meta->trigger_flags = trigger_flags;
    p_meta->uptime_ms = now_ms;

    /* on failure the sound fields keep capture_meta_clear()'s unknown / missing values */
    if (sensing_record_sound(AUDIO_SECONDS, p_audio))
    {
        p_meta->sound_class = s_sound.label_id;
        p_meta->sound_conf = s_sound.conf;
        p_meta->sound_rms = s_sound.rms;
        p_meta->sound_zcr = s_sound.zcr_per_s;
    }

    /* their main.c reads the light again after the recording for the event snapshot */
    s_env.light = read_light();
    uint32_t stamp_ms = to_ms_since_boot(get_absolute_time());
    update_weather(stamp_ms);
    p_meta->light_raw = s_env.light;
    if (s_env.b_dht_ok)
    {
        p_meta->temp_c_x10 = to_x10_i16(s_env.temp_c);
        p_meta->humidity_x10 = (uint16_t)lroundf(s_env.hum_pct * 10.0f);
        p_meta->weather_label = (uint8_t)s_env.weather;
        /* weather_conf / trigger_trust stay "unknown": the tree outputs a label only */
    }
    s_cooldown_until = stamp_ms + SENSING_COOLDOWN_MS;
}

const char* sensing_weather_name(int weather)
{
    return weather < 0 ? "unknown (no DHT22 reading)" : weather_label(weather);
}
