/*
 * sensing.h - glue between Maryam's and Rhaylene's sensor code and the capture pipeline.
 *
 * Their code is used unchanged (third_party/sensors): pir.c, sound.c, mic.c (init), DHT22.c,
 * weather_tree.h. This file adds:
 *   - the dual trigger from their main.c: PIR motion AND sound trigger high at the same time
 *   - a DHT22 reading every 2 s, kept with its age so a failed sensor is reported as missing
 *   - the light reading (GP27 / ADC1) and Maryam's weather tree on (temp, humidity, light)
 *   - the 5 s audio features (audio_capture.c) instead of the 160 kB record buffer
 *   - packing it all into capture_meta_t, which travels with the image in the ALFP MANIFEST
 */
#ifndef SENSING_H
#define SENSING_H

#include <stdbool.h>
#include <stdint.h>

#include "audio_features.h"
#include "capture_meta.h"

typedef struct
{
    void (*wait_ms)(uint32_t ms); /* sleep without hogging the CPU */
    void (*timing_begin)(void);   /* make the next few ms uninterrupted (DHT22 bit timing), may be NULL */
    void (*timing_end)(void);
} sensing_hooks_t;

typedef struct
{
    bool dht_ok;      /* a good DHT22 reading in the last SENSING_DHT_STALE_MS */
    float temp_c;     /* valid if dht_ok */
    float hum_pct;    /* valid if dht_ok */
    uint16_t light;   /* 0-4095 */
    int weather;      /* weather_tree.h: 0 Cloudy, 1 Hot_Dry, 2 Humid_Rainy, -1 unknown (no DHT) */
    uint32_t dht_fail; /* failed DHT22 reads since boot */
    uint32_t dht_good;
} sensing_env_t;

#define SENSING_DHT_PERIOD_MS 2100u   /* DHT22.c refuses reads closer than 2000 ms */
#define SENSING_DHT_STALE_MS 10000u   /* older than this = "missing" in the metadata */
#define SENSING_COOLDOWN_MS 5000u     /* their main.c: 5 s PIR settle time after a capture */

void sensing_init(const sensing_hooks_t* hooks);

/* Call every loop. Reads the DHT22 + light every SENSING_DHT_PERIOD_MS. Returns true when a new
 * DHT22 attempt was made (good or bad) so the caller can log. */
bool sensing_env_poll(uint32_t now_ms);

const sensing_env_t* sensing_env(void);

/* Current trigger pins. */
bool sensing_pir(void);
bool sensing_sound_trigger(void);

/* Dual trigger from their main.c: true when PIR and sound are both high and the 5 s cooldown is over. */
bool sensing_dual_trigger(uint32_t now_ms);

/* Run one capture: records AUDIO_SECONDS of audio, re-reads light, runs the weather tree and fills
 * *m (node_id / capture_id / image_index are left for main.c). Starts the cooldown. Blocks for
 * the recording time. */
void sensing_capture(uint32_t now_ms, uint8_t trigger_flags, capture_meta_t* m, audio_result_t* audio);

const char* sensing_weather_name(int w);

#endif
