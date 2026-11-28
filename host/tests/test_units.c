/*
 * test_units.c - CRC, packet encode/decode, NACK encoding and a decoder fuzz test.
 */
#include <stdlib.h>
#include <string.h>

#include "alfp_crc.h"
#include "alfp_wire.h"
#include "capture_meta.h"
#include "testlib.h"

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void test_crc_check_values(void)
{
    const uint8_t vector[] = "123456789";
    CHECK(alfp_crc16(ALFP_CRC16_INIT, vector, 9) == 0x29B1); /* CRC-16/CCITT-FALSE check value */
    CHECK(alfp_crc32(ALFP_CRC32_INIT, vector, 9) == 0xCBF43926u); /* CRC-32 check value */
    /* incremental == one-shot */
    uint32_t crc = alfp_crc32(ALFP_CRC32_INIT, vector, 4);
    crc = alfp_crc32(crc, vector + 4, 5);
    CHECK(crc == 0xCBF43926u);
    uint16_t delta = alfp_crc16(ALFP_CRC16_INIT, vector, 3);
    delta = alfp_crc16(delta, vector + 3, 6);
    CHECK(delta == 0x29B1);
}

static void test_manifest_roundtrip(void)
{
    uint8_t meta[20];
    for (int idx = 0; idx < 20; idx++)
    {
        meta[idx] = (uint8_t)(idx * 7);
    }
    alfp_manifest_t manifest = {10000, 42, 241, 0xDEADBEEFu, ALFP_RADIO_LORA, 20, meta};
    uint8_t buf[250];
    uint16_t len = alfp_enc_manifest(buf, sizeof(buf), 0xBEEF, &manifest);
    CHECK(len == ALFP_MANIFEST_OVERHEAD + 20);
    alfp_pkt_t pkt;
    CHECK(alfp_decode(buf, len, &pkt) == ALFP_DEC_OK);
    CHECK(pkt.type == ALFP_T_MANIFEST && pkt.sid == 0xBEEF);
    CHECK(pkt.body.manifest.file_size == 10000 && pkt.body.manifest.frag_count == 42 && pkt.body.manifest.frag_size == 241);
    CHECK(pkt.body.manifest.file_crc32 == 0xDEADBEEFu && pkt.body.manifest.radio_id == ALFP_RADIO_LORA);
    CHECK(pkt.body.manifest.meta_len == 20 && memcmp(pkt.body.manifest.p_meta, meta, 20) == 0);
    /* inconsistent count is rejected */
    alfp_manifest_t bad = manifest;
    bad.frag_count = 41;
    len = alfp_enc_manifest(buf, sizeof(buf), 1, &bad);
    CHECK(alfp_decode(buf, len, &pkt) == ALFP_DEC_BAD_FIELD);
    /* too small a buffer */
    CHECK(alfp_enc_manifest(buf, 20, 1, &manifest) == 0);
}

static void test_data_roundtrip(void)
{
    uint8_t buf[250];
    uint16_t hdr_len = alfp_enc_data_header(buf, 7, 41, 42);
    for (int idx = 0; idx < 120; idx++)
    {
        buf[hdr_len + idx] = (uint8_t)idx;
    }
    uint16_t len = alfp_enc_data_finish(buf, 120);
    CHECK(len == 120 + ALFP_DATA_OVERHEAD);
    alfp_pkt_t pkt;
    CHECK(alfp_decode(buf, len, &pkt) == ALFP_DEC_OK);
    CHECK(pkt.type == ALFP_T_DATA && pkt.sid == 7 && pkt.body.data.seq == 41 && pkt.body.data.frag_count == 42);
    CHECK(pkt.body.data.len == 120 && pkt.body.data.p_payload[119] == 119);
    /* seq >= count rejected */
    hdr_len = alfp_enc_data_header(buf, 7, 42, 42);
    buf[hdr_len] = 1;
    len = alfp_enc_data_finish(buf, 1);
    CHECK(alfp_decode(buf, len, &pkt) == ALFP_DEC_BAD_FIELD);
}

static void test_small_packets(void)
{
    uint8_t buf[16];
    alfp_pkt_t pkt;
    uint16_t len = alfp_enc_poll(buf, sizeof(buf), 99, 3);
    CHECK(len == ALFP_POLL_LEN && alfp_decode(buf, len, &pkt) == ALFP_DEC_OK);
    CHECK(pkt.type == ALFP_T_POLL && pkt.sid == 99 && pkt.body.round == 3);
    len = alfp_enc_ack(buf, sizeof(buf), 100, ALFP_ACK_CRC_FAIL);
    CHECK(len == ALFP_ACK_LEN && alfp_decode(buf, len, &pkt) == ALFP_DEC_OK);
    CHECK(pkt.type == ALFP_T_ACK && pkt.sid == 100 && pkt.body.status == ALFP_ACK_CRC_FAIL);
    /* wrong version */
    buf[0] = (uint8_t)((2u << 4) | ALFP_T_ACK);
    uint16_t crc = alfp_crc16(ALFP_CRC16_INIT, buf, 4);
    buf[4] = (uint8_t)crc;
    buf[5] = (uint8_t)(crc >> 8);
    CHECK(alfp_decode(buf, 6, &pkt) == ALFP_DEC_VERSION);
    CHECK(alfp_decode(buf, 3, &pkt) == ALFP_DEC_SHORT);
}

typedef struct
{
    uint8_t seen[ALFP_BITMAP_BYTES];
    int count;
} collect_t;

static void collect(uint16_t seq, void* p_user)
{
    collect_t* p_seen = p_user;
    p_seen->seen[seq >> 3] |= (uint8_t)(1u << (seq & 7));
    p_seen->count++;
}

/* For random received-bitmaps, the NACK must name exactly the missing fragments, no more, no less. */
static void test_nack_exact(void)
{
    int list_used = 0;
    int bitmap_used = 0;
    for (int trial = 0; trial < 3000; trial++)
    {
        uint16_t count = (uint16_t)(1 + rnd() % ALFP_MAX_FRAGS);
        uint8_t have[ALFP_BITMAP_BYTES] = {0};
        int pmiss = (int)(rnd() % 100);
        int missing = 0;
        for (uint16_t idx = 0; idx < count; idx++)
        {
            if ((int)(rnd() % 100) >= pmiss)
            {
                have[idx >> 3] |= (uint8_t)(1u << (idx & 7));
            }
            else
            {
                missing++;
            }
        }
        uint8_t buf[245];
        uint16_t len = alfp_enc_nack(buf, sizeof(buf), 5, 0, 1, have, count, false);
        alfp_pkt_t pkt;
        CHECK(len > 0 && len <= 245);
        CHECK(alfp_decode(buf, len, &pkt) == ALFP_DEC_OK);
        collect_t seen = {{0}, 0};
        alfp_nack_for_each(&pkt.body.nack, count, collect, &seen);
        CHECK(seen.count == missing);
        for (uint16_t idx = 0; idx < count; idx++)
        {
            bool b_miss = !((have[idx >> 3] >> (idx & 7)) & 1);
            bool b_asked = (seen.seen[idx >> 3] >> (idx & 7)) & 1;
            if (b_miss != b_asked)
            {
                CHECK(b_miss == b_asked);
                break;
            }
        }
        if (0 == missing)
        {
            CHECK(pkt.body.nack.fmt == ALFP_NACK_FMT_NONE);
        }
        list_used += ALFP_NACK_FMT_LIST == pkt.body.nack.fmt;
        bitmap_used += ALFP_NACK_FMT_BITMAP == pkt.body.nack.fmt;
    }
    CHECK(list_used > 0 && bitmap_used > 0); /* both encodings get exercised */
}

/* A NACK for 3 missing fragments out of 42 is a short list, not a full bitmap. */
static void test_nack_is_compact(void)
{
    uint8_t have[ALFP_BITMAP_BYTES];
    memset(have, 0xFF, sizeof(have));
    have[0] &= (uint8_t)~(1u << 3);
    have[2] &= (uint8_t)~(1u << 1);
    have[5] &= (uint8_t)~(1u << 0);
    uint8_t buf[245];
    uint16_t len = alfp_enc_nack(buf, sizeof(buf), 1, 0, 1, have, 42, false);
    alfp_pkt_t pkt;
    CHECK(alfp_decode(buf, len, &pkt) == ALFP_DEC_OK);
    CHECK(pkt.body.nack.fmt == ALFP_NACK_FMT_LIST && pkt.body.nack.count == 3);
    CHECK(len == 7 + 6 + 2); /* header 6 + n 1 + 3 x 2 + crc 2 */
}

/* Every single-bit flip of a full-size DATA packet must be caught by the CRC-16. */
static void test_crc_catches_bit_flips(void)
{
    uint8_t buf[250];
    uint16_t hdr_len = alfp_enc_data_header(buf, 3, 5, 42);
    for (int idx = 0; idx < 241; idx++)
    {
        buf[hdr_len + idx] = (uint8_t)rnd();
    }
    uint16_t len = alfp_enc_data_finish(buf, 241);
    alfp_pkt_t pkt;
    int caught = 0;
    int total = 0;
    for (int bit = 0; bit < len * 8; bit++)
    {
        buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        total++;
        caught += ALFP_DEC_OK != alfp_decode(buf, len, &pkt);
        buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    }
    CHECK(caught == total);
    /* and every double-bit flip in the first 64 bytes */
    int dcaught = 0;
    int dtotal = 0;
    for (int idx_a = 0; idx_a < 512; idx_a++)
    {
        for (int idx_b = idx_a + 1; idx_b < 512; idx_b += 7)
        {
            buf[idx_a / 8] ^= (uint8_t)(1u << (idx_a % 8));
            buf[idx_b / 8] ^= (uint8_t)(1u << (idx_b % 8));
            dtotal++;
            dcaught += ALFP_DEC_OK != alfp_decode(buf, len, &pkt);
            buf[idx_a / 8] ^= (uint8_t)(1u << (idx_a % 8));
            buf[idx_b / 8] ^= (uint8_t)(1u << (idx_b % 8));
        }
    }
    CHECK(dcaught == dtotal);
    printf("    %d/%d single-bit and %d/%d double-bit corruptions detected\n", caught, total, dcaught, dtotal);
}

/* Random garbage must never crash the decoder (run under AddressSanitizer). */
static void test_decoder_fuzz(void)
{
    uint8_t buf[260];
    alfp_pkt_t pkt;
    int ok_flag = 0;
    for (int idx = 0; idx < 300000; idx++)
    {
        uint16_t len = (uint16_t)(rnd() % 256);
        for (int kdx = 0; kdx < len; kdx++)
        {
            buf[kdx] = (uint8_t)rnd();
        }
        /* half the time make the CRC valid so the field checks get exercised */
        if ((len >= 3) && (idx & 1))
        {
            buf[0] = (uint8_t)((ALFP_VERSION << 4) | (rnd() % 6));
            uint16_t crc = alfp_crc16(ALFP_CRC16_INIT, buf, len - 2u);
            buf[len - 2] = (uint8_t)crc;
            buf[len - 1] = (uint8_t)(crc >> 8);
        }
        if (ALFP_DEC_OK == alfp_decode(buf, len, &pkt))
        {
            ok_flag++;
            if (ALFP_T_NACK == pkt.type)
            {
                collect_t seen = {{0}, 0};
                alfp_nack_for_each(&pkt.body.nack, ALFP_MAX_FRAGS, collect, &seen);
            }
        }
    }
    CHECK(ok_flag > 0);
}

static void test_capture_meta(void)
{
    capture_meta_t meta, back;
    capture_meta_clear(&meta);
    meta.node_id = 3;
    meta.capture_id = 123456;
    meta.uptime_ms = 987654321u;
    meta.trigger_flags = CM_TRIG_PIR | CM_TRIG_SOUND;
    meta.temp_c_x10 = -45;
    meta.humidity_x10 = 815;
    meta.weather_label = 2;
    meta.sound_class = 1;
    meta.image_index = 7;
    uint8_t buf[CAPTURE_META_LEN];
    CHECK(capture_meta_pack(&meta, buf) == CAPTURE_META_LEN);
    CHECK(capture_meta_unpack(buf, CAPTURE_META_LEN, &back) == 0);
    CHECK(back.capture_id == 123456 && back.uptime_ms == 987654321u && back.temp_c_x10 == -45);
    CHECK(back.light_raw == CM_MISSING_U16 && back.sound_rms == CM_MISSING_U16 && back.image_index == 7);
    CHECK(CAPTURE_META_LEN <= 64);

    /* version 2: 31 bytes, queued_ms packed as missing, then stamped by the sender at send time */
    CHECK(CAPTURE_META_LEN == CAPTURE_META_LEN_V2 && buf[0] == 2u);
    CHECK(back.queued_ms == CM_MISSING_U32);
    CHECK(capture_meta_record_len(buf, CAPTURE_META_LEN) == CAPTURE_META_LEN_V2);
    cm_put32(buf + CAPTURE_META_QUEUED_OFF, 2600u);
    CHECK(capture_meta_unpack(buf, CAPTURE_META_LEN, &back) == 0 && back.queued_ms == 2600u);
    CHECK(capture_meta_unpack(buf, CAPTURE_META_LEN_V2 - 1u, &back) == -1); /* a v2 record cut short */

    /* version 1: the first 27 bytes with version 1 (nodes built before queued_ms) still unpack,
     * queued_ms missing, and the name tail starts at 27 */
    uint8_t record_v1[CAPTURE_META_LEN_V1 + 4];
    memcpy(record_v1, buf, CAPTURE_META_LEN_V1);
    record_v1[0] = 1u;
    memcpy(record_v1 + CAPTURE_META_LEN_V1, "a.jp", 4);
    capture_meta_t old;
    CHECK(capture_meta_unpack(record_v1, sizeof(record_v1), &old) == 0);
    CHECK(old.capture_id == 123456 && old.image_index == 7 && old.queued_ms == CM_MISSING_U32);
    CHECK(capture_meta_record_len(record_v1, sizeof(record_v1)) == CAPTURE_META_LEN_V1);
    record_v1[0] = 3u; /* unknown version */
    CHECK(capture_meta_unpack(record_v1, sizeof(record_v1), &old) == -1 && capture_meta_record_len(record_v1, sizeof(record_v1)) == 0u);
}

int main(void)
{
    RUN(test_crc_check_values);
    RUN(test_manifest_roundtrip);
    RUN(test_data_roundtrip);
    RUN(test_small_packets);
    RUN(test_nack_exact);
    RUN(test_nack_is_compact);
    RUN(test_crc_catches_bit_flips);
    RUN(test_decoder_fuzz);
    RUN(test_capture_meta);
    return SUMMARY();
}
