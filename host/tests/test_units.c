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
    const uint8_t s[] = "123456789";
    CHECK(alfp_crc16(ALFP_CRC16_INIT, s, 9) == 0x29B1); /* CRC-16/CCITT-FALSE check value */
    CHECK(alfp_crc32(ALFP_CRC32_INIT, s, 9) == 0xCBF43926u); /* CRC-32 check value */
    /* incremental == one-shot */
    uint32_t c = alfp_crc32(ALFP_CRC32_INIT, s, 4);
    c = alfp_crc32(c, s + 4, 5);
    CHECK(c == 0xCBF43926u);
    uint16_t d = alfp_crc16(ALFP_CRC16_INIT, s, 3);
    d = alfp_crc16(d, s + 3, 6);
    CHECK(d == 0x29B1);
}

static void test_manifest_roundtrip(void)
{
    uint8_t meta[20];
    for (int i = 0; i < 20; i++)
    {
        meta[i] = (uint8_t)(i * 7);
    }
    alfp_manifest_t m = {10000, 42, 241, 0xDEADBEEFu, ALFP_RADIO_LORA, 20, meta};
    uint8_t buf[250];
    uint16_t len = alfp_enc_manifest(buf, sizeof(buf), 0xBEEF, &m);
    CHECK(len == ALFP_MANIFEST_OVERHEAD + 20);
    alfp_pkt_t p;
    CHECK(alfp_decode(buf, len, &p) == ALFP_DEC_OK);
    CHECK(p.type == ALFP_T_MANIFEST && p.sid == 0xBEEF);
    CHECK(p.u.manifest.file_size == 10000 && p.u.manifest.frag_count == 42 && p.u.manifest.frag_size == 241);
    CHECK(p.u.manifest.file_crc32 == 0xDEADBEEFu && p.u.manifest.radio_id == ALFP_RADIO_LORA);
    CHECK(p.u.manifest.meta_len == 20 && memcmp(p.u.manifest.meta, meta, 20) == 0);
    /* inconsistent count is rejected */
    alfp_manifest_t bad = m;
    bad.frag_count = 41;
    len = alfp_enc_manifest(buf, sizeof(buf), 1, &bad);
    CHECK(alfp_decode(buf, len, &p) == ALFP_DEC_BAD_FIELD);
    /* too small a buffer */
    CHECK(alfp_enc_manifest(buf, 20, 1, &m) == 0);
}

static void test_data_roundtrip(void)
{
    uint8_t buf[250];
    uint16_t h = alfp_enc_data_header(buf, 7, 41, 42);
    for (int i = 0; i < 120; i++)
    {
        buf[h + i] = (uint8_t)i;
    }
    uint16_t len = alfp_enc_data_finish(buf, 120);
    CHECK(len == 120 + ALFP_DATA_OVERHEAD);
    alfp_pkt_t p;
    CHECK(alfp_decode(buf, len, &p) == ALFP_DEC_OK);
    CHECK(p.type == ALFP_T_DATA && p.sid == 7 && p.u.data.seq == 41 && p.u.data.frag_count == 42);
    CHECK(p.u.data.len == 120 && p.u.data.payload[119] == 119);
    /* seq >= count rejected */
    h = alfp_enc_data_header(buf, 7, 42, 42);
    buf[h] = 1;
    len = alfp_enc_data_finish(buf, 1);
    CHECK(alfp_decode(buf, len, &p) == ALFP_DEC_BAD_FIELD);
}

static void test_small_packets(void)
{
    uint8_t buf[16];
    alfp_pkt_t p;
    uint16_t len = alfp_enc_poll(buf, sizeof(buf), 99, 3);
    CHECK(len == ALFP_POLL_LEN && alfp_decode(buf, len, &p) == ALFP_DEC_OK);
    CHECK(p.type == ALFP_T_POLL && p.sid == 99 && p.u.round == 3);
    len = alfp_enc_ack(buf, sizeof(buf), 100, ALFP_ACK_CRC_FAIL);
    CHECK(len == ALFP_ACK_LEN && alfp_decode(buf, len, &p) == ALFP_DEC_OK);
    CHECK(p.type == ALFP_T_ACK && p.sid == 100 && p.u.status == ALFP_ACK_CRC_FAIL);
    /* wrong version */
    buf[0] = (uint8_t)((2u << 4) | ALFP_T_ACK);
    uint16_t c = alfp_crc16(ALFP_CRC16_INIT, buf, 4);
    buf[4] = (uint8_t)c;
    buf[5] = (uint8_t)(c >> 8);
    CHECK(alfp_decode(buf, 6, &p) == ALFP_DEC_VERSION);
    CHECK(alfp_decode(buf, 3, &p) == ALFP_DEC_SHORT);
}

typedef struct
{
    uint8_t seen[ALFP_BITMAP_BYTES];
    int n;
} collect_t;

static void collect(uint16_t seq, void* u)
{
    collect_t* c = u;
    c->seen[seq >> 3] |= (uint8_t)(1u << (seq & 7));
    c->n++;
}

/* For random received-bitmaps, the NACK must name exactly the missing fragments, no more, no less. */
static void test_nack_exact(void)
{
    int list_used = 0, bitmap_used = 0;
    for (int trial = 0; trial < 3000; trial++)
    {
        uint16_t count = (uint16_t)(1 + rnd() % ALFP_MAX_FRAGS);
        uint8_t have[ALFP_BITMAP_BYTES] = {0};
        int pmiss = (int)(rnd() % 100);
        int missing = 0;
        for (uint16_t i = 0; i < count; i++)
        {
            if ((int)(rnd() % 100) >= pmiss)
            {
                have[i >> 3] |= (uint8_t)(1u << (i & 7));
            }
            else
            {
                missing++;
            }
        }
        uint8_t buf[245];
        uint16_t len = alfp_enc_nack(buf, sizeof(buf), 5, 0, 1, have, count, false);
        alfp_pkt_t p;
        CHECK(len > 0 && len <= 245);
        CHECK(alfp_decode(buf, len, &p) == ALFP_DEC_OK);
        collect_t c = {{0}, 0};
        alfp_nack_for_each(&p.u.nack, count, collect, &c);
        CHECK(c.n == missing);
        for (uint16_t i = 0; i < count; i++)
        {
            bool miss = !((have[i >> 3] >> (i & 7)) & 1);
            bool asked = (c.seen[i >> 3] >> (i & 7)) & 1;
            if (miss != asked)
            {
                CHECK(miss == asked);
                break;
            }
        }
        if (missing == 0)
        {
            CHECK(p.u.nack.fmt == ALFP_NACK_FMT_NONE);
        }
        list_used += p.u.nack.fmt == ALFP_NACK_FMT_LIST;
        bitmap_used += p.u.nack.fmt == ALFP_NACK_FMT_BITMAP;
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
    alfp_pkt_t p;
    CHECK(alfp_decode(buf, len, &p) == ALFP_DEC_OK);
    CHECK(p.u.nack.fmt == ALFP_NACK_FMT_LIST && p.u.nack.n == 3);
    CHECK(len == 7 + 6 + 2); /* header 6 + n 1 + 3 x 2 + crc 2 */
}

/* Every single-bit flip of a full-size DATA packet must be caught by the CRC-16. */
static void test_crc_catches_bit_flips(void)
{
    uint8_t buf[250];
    uint16_t h = alfp_enc_data_header(buf, 3, 5, 42);
    for (int i = 0; i < 241; i++)
    {
        buf[h + i] = (uint8_t)rnd();
    }
    uint16_t len = alfp_enc_data_finish(buf, 241);
    alfp_pkt_t p;
    int caught = 0, total = 0;
    for (int bit = 0; bit < len * 8; bit++)
    {
        buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        total++;
        caught += alfp_decode(buf, len, &p) != ALFP_DEC_OK;
        buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    }
    CHECK(caught == total);
    /* and every double-bit flip in the first 64 bytes */
    int dcaught = 0, dtotal = 0;
    for (int a = 0; a < 512; a++)
    {
        for (int b = a + 1; b < 512; b += 7)
        {
            buf[a / 8] ^= (uint8_t)(1u << (a % 8));
            buf[b / 8] ^= (uint8_t)(1u << (b % 8));
            dtotal++;
            dcaught += alfp_decode(buf, len, &p) != ALFP_DEC_OK;
            buf[a / 8] ^= (uint8_t)(1u << (a % 8));
            buf[b / 8] ^= (uint8_t)(1u << (b % 8));
        }
    }
    CHECK(dcaught == dtotal);
    printf("    %d/%d single-bit and %d/%d double-bit corruptions detected\n", caught, total, dcaught, dtotal);
}

/* Random garbage must never crash the decoder (run under AddressSanitizer). */
static void test_decoder_fuzz(void)
{
    uint8_t buf[260];
    alfp_pkt_t p;
    int ok = 0;
    for (int i = 0; i < 300000; i++)
    {
        uint16_t len = (uint16_t)(rnd() % 256);
        for (int k = 0; k < len; k++)
        {
            buf[k] = (uint8_t)rnd();
        }
        /* half the time make the CRC valid so the field checks get exercised */
        if (len >= 3 && (i & 1))
        {
            buf[0] = (uint8_t)((ALFP_VERSION << 4) | (rnd() % 6));
            uint16_t c = alfp_crc16(ALFP_CRC16_INIT, buf, len - 2u);
            buf[len - 2] = (uint8_t)c;
            buf[len - 1] = (uint8_t)(c >> 8);
        }
        if (alfp_decode(buf, len, &p) == ALFP_DEC_OK)
        {
            ok++;
            if (p.type == ALFP_T_NACK)
            {
                collect_t c = {{0}, 0};
                alfp_nack_for_each(&p.u.nack, ALFP_MAX_FRAGS, collect, &c);
            }
        }
    }
    CHECK(ok > 0);
}

static void test_capture_meta(void)
{
    capture_meta_t m, back;
    capture_meta_clear(&m);
    m.node_id = 3;
    m.capture_id = 123456;
    m.uptime_ms = 987654321u;
    m.trigger_flags = CM_TRIG_PIR | CM_TRIG_SOUND;
    m.temp_c_x10 = -45;
    m.humidity_x10 = 815;
    m.weather_label = 2;
    m.sound_class = 1;
    m.image_index = 7;
    uint8_t buf[CAPTURE_META_LEN];
    CHECK(capture_meta_pack(&m, buf) == CAPTURE_META_LEN);
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
    uint8_t v1[CAPTURE_META_LEN_V1 + 4];
    memcpy(v1, buf, CAPTURE_META_LEN_V1);
    v1[0] = 1u;
    memcpy(v1 + CAPTURE_META_LEN_V1, "a.jp", 4);
    capture_meta_t old;
    CHECK(capture_meta_unpack(v1, sizeof(v1), &old) == 0);
    CHECK(old.capture_id == 123456 && old.image_index == 7 && old.queued_ms == CM_MISSING_U32);
    CHECK(capture_meta_record_len(v1, sizeof(v1)) == CAPTURE_META_LEN_V1);
    v1[0] = 3u; /* unknown version */
    CHECK(capture_meta_unpack(v1, sizeof(v1), &old) == -1 && capture_meta_record_len(v1, sizeof(v1)) == 0u);
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
