/*
 * capture_meta.h - the metadata that travels with every capture.   Shared by all members.
 *
 * ALFP carries these bytes opaquely in the MANIFEST (up to 64 bytes); this file defines what
 * they mean so the field node, the bridge and the dashboard all agree.
 *
 *   Member 2 (Shreya):  node_id, capture_id, uptime_ms, image_index (which sample image)
 *   Member 3 (Rhaylene): sound_class, sound_conf, sound_rms, sound_zcr
 *   Member 4 (Maryam):   trigger_flags, temp/humidity/light, weather_label, weather_conf, trigger_trust
 *
 * A reading that failed is sent as its MISSING value, never as a made-up number, so the
 * dashboard can show "-" honestly instead of a fake 0.
 *
 * Wire layout (little-endian). Version 2 = 31 bytes, version 1 = the first 27 bytes only:
 *   0  version        u8   = 2 (1 on nodes built before queued_ms existed)
 *   1  node_id        u8
 *   2  capture_id     u32  per-node counter
 *   6  uptime_ms      u32  ms since boot when the trigger fired
 *   10 trigger_flags  u8   bit0 PIR, bit1 sound
 *   11 temp_c_x10     i16  e.g. 287 = 28.7 C        missing: INT16_MIN
 *   13 humidity_x10   u16  e.g. 815 = 81.5 %RH      missing: 0xFFFF
 *   15 light_raw      u16  ADC reading 0-4095       missing: 0xFFFF
 *   17 weather_label  u8   Maryam's enum            unknown: 0xFF
 *   18 weather_conf   u8   0-100                    unknown: 0xFF
 *   19 trigger_trust  u8   0-100                    unknown: 0xFF
 *   20 sound_class    u8   label id, common/sound_classes.h (on-node classifier; group via the table)
 *                                                   unknown: 0xFF
 *   21 sound_conf     u8   0-100, the classifier's confidence in sound_class   unknown: 0xFF
 *   22 sound_rms      u16  ADC counts around the capture's own mean (DC-corrected)   missing: 0xFFFF
 *   24 sound_zcr      u16  zero crossings / s       missing: 0xFFFF
 *   26 image_index    u8   sample image used
 *   27 queued_ms      u32  v2 only: ms from the trigger (uptime_ms) to the moment the sender started
 *                          this transfer, written by the sender just before alfp_send_file().
 *                          missing: 0xFFFFFFFF (packed that way; the node fills it in at send time)
 *   27.. (v1) / 31.. (v2)  (optional) original file name, UTF-8, no terminator (image name, the
 *                          "TELEMETRY" marker, or the LoRa photo test's name)
 * capture_meta_unpack() accepts both versions; capture_meta_record_len() says where the name starts.
 */
#ifndef CAPTURE_META_H
#define CAPTURE_META_H

#include <stdint.h>
#include <string.h>

#define CAPTURE_META_VERSION 2u
#define CAPTURE_META_LEN_V1 27u
#define CAPTURE_META_LEN_V2 31u
/* What capture_meta_pack() writes (the current version), so buffers sized with it always fit. */
#define CAPTURE_META_LEN CAPTURE_META_LEN_V2
#define CAPTURE_META_QUEUED_OFF 27u /* where a v2 record keeps queued_ms */

#define CM_TRIG_PIR 0x01u
#define CM_TRIG_SOUND 0x02u

#define CM_MISSING_I16 INT16_MIN
#define CM_MISSING_U16 0xFFFFu
#define CM_UNKNOWN_U8 0xFFu
#define CM_MISSING_U32 0xFFFFFFFFu

typedef struct
{
    uint8_t node_id;
    uint32_t capture_id;
    uint32_t uptime_ms;
    uint8_t trigger_flags;
    int16_t temp_c_x10;
    uint16_t humidity_x10;
    uint16_t light_raw;
    uint8_t weather_label;
    uint8_t weather_conf;
    uint8_t trigger_trust;
    uint8_t sound_class;
    uint8_t sound_conf;
    uint16_t sound_rms;
    uint16_t sound_zcr;
    uint8_t image_index;
    uint32_t queued_ms; /* v2; CM_MISSING_U32 when unknown (always for a v1 record) */
} capture_meta_t;

static inline void capture_meta_clear(capture_meta_t* m)
{
    memset(m, 0, sizeof(*m));
    m->temp_c_x10 = CM_MISSING_I16;
    m->humidity_x10 = CM_MISSING_U16;
    m->light_raw = CM_MISSING_U16;
    m->weather_label = m->weather_conf = m->trigger_trust = CM_UNKNOWN_U8;
    m->sound_class = m->sound_conf = CM_UNKNOWN_U8;
    m->sound_rms = m->sound_zcr = CM_MISSING_U16;
    m->queued_ms = CM_MISSING_U32;
}

static inline void cm_put16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void cm_put32(uint8_t* p, uint32_t v)
{
    cm_put16(p, (uint16_t)v);
    cm_put16(p + 2, (uint16_t)(v >> 16));
}

static inline uint16_t cm_get16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t cm_get32(const uint8_t* p)
{
    return (uint32_t)cm_get16(p) | ((uint32_t)cm_get16(p + 2) << 16);
}

/* Returns bytes written (CAPTURE_META_LEN, version 2). out must hold CAPTURE_META_LEN bytes.
 * queued_ms is always packed as missing: the sender fills it in just before sending. */
static inline uint8_t capture_meta_pack(const capture_meta_t* m, uint8_t* out)
{
    out[0] = CAPTURE_META_VERSION;
    out[1] = m->node_id;
    cm_put32(out + 2, m->capture_id);
    cm_put32(out + 6, m->uptime_ms);
    out[10] = m->trigger_flags;
    cm_put16(out + 11, (uint16_t)m->temp_c_x10);
    cm_put16(out + 13, m->humidity_x10);
    cm_put16(out + 15, m->light_raw);
    out[17] = m->weather_label;
    out[18] = m->weather_conf;
    out[19] = m->trigger_trust;
    out[20] = m->sound_class;
    out[21] = m->sound_conf;
    cm_put16(out + 22, m->sound_rms);
    cm_put16(out + 24, m->sound_zcr);
    out[26] = m->image_index;
    cm_put32(out + CAPTURE_META_QUEUED_OFF, CM_MISSING_U32);
    return CAPTURE_META_LEN;
}

/* Length of the fixed record at the start of in (27 for v1, 31 for v2), i.e. where the optional
 * name starts. 0 if in is not a capture record of a known version or is too short. */
static inline uint8_t capture_meta_record_len(const uint8_t* in, uint8_t len)
{
    if (len >= CAPTURE_META_LEN_V2 && in[0] == 2u)
    {
        return (uint8_t)CAPTURE_META_LEN_V2;
    }
    if (len >= CAPTURE_META_LEN_V1 && in[0] == 1u)
    {
        return (uint8_t)CAPTURE_META_LEN_V1;
    }
    return 0;
}

/* Returns 0 on success, -1 if the bytes are not a version-1 or version-2 record. */
static inline int capture_meta_unpack(const uint8_t* in, uint8_t len, capture_meta_t* m)
{
    uint8_t rec = capture_meta_record_len(in, len);
    if (rec == 0u)
    {
        return -1;
    }
    m->node_id = in[1];
    m->capture_id = cm_get32(in + 2);
    m->uptime_ms = cm_get32(in + 6);
    m->trigger_flags = in[10];
    m->temp_c_x10 = (int16_t)cm_get16(in + 11);
    m->humidity_x10 = cm_get16(in + 13);
    m->light_raw = cm_get16(in + 15);
    m->weather_label = in[17];
    m->weather_conf = in[18];
    m->trigger_trust = in[19];
    m->sound_class = in[20];
    m->sound_conf = in[21];
    m->sound_rms = cm_get16(in + 22);
    m->sound_zcr = cm_get16(in + 24);
    m->image_index = in[26];
    m->queued_ms = rec >= CAPTURE_META_LEN_V2 ? cm_get32(in + CAPTURE_META_QUEUED_OFF) : CM_MISSING_U32;
    return 0;
}

#endif
