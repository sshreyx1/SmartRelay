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

static inline void capture_meta_clear(capture_meta_t* p_meta)
{
    memset(p_meta, 0, sizeof(*p_meta));
    p_meta->temp_c_x10 = CM_MISSING_I16;
    p_meta->humidity_x10 = CM_MISSING_U16;
    p_meta->light_raw = CM_MISSING_U16;
    p_meta->weather_label = p_meta->weather_conf = p_meta->trigger_trust = CM_UNKNOWN_U8;
    p_meta->sound_class = p_meta->sound_conf = CM_UNKNOWN_U8;
    p_meta->sound_rms = p_meta->sound_zcr = CM_MISSING_U16;
    p_meta->queued_ms = CM_MISSING_U32;
}

static inline void cm_put16(uint8_t* p_buf, uint16_t value)
{
    p_buf[0] = (uint8_t)value;
    p_buf[1] = (uint8_t)(value >> 8);
}

static inline void cm_put32(uint8_t* p_buf, uint32_t value)
{
    cm_put16(p_buf, (uint16_t)value);
    cm_put16(p_buf + 2, (uint16_t)(value >> 16));
}

static inline uint16_t cm_get16(const uint8_t* p_buf)
{
    return (uint16_t)(p_buf[0] | (p_buf[1] << 8));
}

static inline uint32_t cm_get32(const uint8_t* p_buf)
{
    return (uint32_t)cm_get16(p_buf) | ((uint32_t)cm_get16(p_buf + 2) << 16);
}

/* Returns bytes written (CAPTURE_META_LEN, version 2). out must hold CAPTURE_META_LEN bytes.
 * queued_ms is always packed as missing: the sender fills it in just before sending. */
static inline uint8_t capture_meta_pack(const capture_meta_t* p_meta, uint8_t* p_out)
{
    p_out[0] = CAPTURE_META_VERSION;
    p_out[1] = p_meta->node_id;
    cm_put32(p_out + 2, p_meta->capture_id);
    cm_put32(p_out + 6, p_meta->uptime_ms);
    p_out[10] = p_meta->trigger_flags;
    cm_put16(p_out + 11, (uint16_t)p_meta->temp_c_x10);
    cm_put16(p_out + 13, p_meta->humidity_x10);
    cm_put16(p_out + 15, p_meta->light_raw);
    p_out[17] = p_meta->weather_label;
    p_out[18] = p_meta->weather_conf;
    p_out[19] = p_meta->trigger_trust;
    p_out[20] = p_meta->sound_class;
    p_out[21] = p_meta->sound_conf;
    cm_put16(p_out + 22, p_meta->sound_rms);
    cm_put16(p_out + 24, p_meta->sound_zcr);
    p_out[26] = p_meta->image_index;
    cm_put32(p_out + CAPTURE_META_QUEUED_OFF, CM_MISSING_U32);
    return CAPTURE_META_LEN;
}

/* Length of the fixed record at the start of in (27 for v1, 31 for v2), i.e. where the optional
 * name starts. 0 if in is not a capture record of a known version or is too short. */
static inline uint8_t capture_meta_record_len(const uint8_t* p_input, uint8_t len)
{
    if (len >= CAPTURE_META_LEN_V2 && p_input[0] == 2u)
    {
        return (uint8_t)CAPTURE_META_LEN_V2;
    }
    if (len >= CAPTURE_META_LEN_V1 && p_input[0] == 1u)
    {
        return (uint8_t)CAPTURE_META_LEN_V1;
    }
    return 0;
}

/* Returns 0 on success, -1 if the bytes are not a version-1 or version-2 record. */
static inline int capture_meta_unpack(const uint8_t* p_input, uint8_t len, capture_meta_t* p_meta)
{
    uint8_t rec = capture_meta_record_len(p_input, len);
    if (rec == 0u)
    {
        return -1;
    }
    p_meta->node_id = p_input[1];
    p_meta->capture_id = cm_get32(p_input + 2);
    p_meta->uptime_ms = cm_get32(p_input + 6);
    p_meta->trigger_flags = p_input[10];
    p_meta->temp_c_x10 = (int16_t)cm_get16(p_input + 11);
    p_meta->humidity_x10 = cm_get16(p_input + 13);
    p_meta->light_raw = cm_get16(p_input + 15);
    p_meta->weather_label = p_input[17];
    p_meta->weather_conf = p_input[18];
    p_meta->trigger_trust = p_input[19];
    p_meta->sound_class = p_input[20];
    p_meta->sound_conf = p_input[21];
    p_meta->sound_rms = cm_get16(p_input + 22);
    p_meta->sound_zcr = cm_get16(p_input + 24);
    p_meta->image_index = p_input[26];
    p_meta->queued_ms = rec >= CAPTURE_META_LEN_V2 ? cm_get32(p_input + CAPTURE_META_QUEUED_OFF) : CM_MISSING_U32;
    return 0;
}

#endif
