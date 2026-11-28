/*
 * alfp_wire.c - encode/decode ALFP packets. Pure functions, no state, no allocation.
 */
#include "alfp_wire.h"

#include <string.h>

#include "alfp_crc.h"

/* ---- little-endian helpers ------------------------------------------------------ */

static void put16(uint8_t* p_buf, uint16_t value)
{
    p_buf[0] = (uint8_t)value;
    p_buf[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t* p_buf, uint32_t value)
{
    p_buf[0] = (uint8_t)value;
    p_buf[1] = (uint8_t)(value >> 8);
    p_buf[2] = (uint8_t)(value >> 16);
    p_buf[3] = (uint8_t)(value >> 24);
}

static uint16_t get16(const uint8_t* p_buf)
{
    return (uint16_t)(p_buf[0] | (p_buf[1] << 8));
}

static uint32_t get32(const uint8_t* p_buf)
{
    return (uint32_t)p_buf[0] | ((uint32_t)p_buf[1] << 8) | ((uint32_t)p_buf[2] << 16) | ((uint32_t)p_buf[3] << 24);
}

static uint8_t b0(uint8_t type)
{
    return (uint8_t)((ALFP_VERSION << 4) | (type & 0x0Fu));
}

/* Appends the CRC-16 of out[0..len) at out[len], returns len + 2. */
static uint16_t seal(uint8_t* p_out, uint16_t len)
{
    put16(p_out + len, alfp_crc16(ALFP_CRC16_INIT, p_out, len));
    return (uint16_t)(len + 2u);
}

static bool bit_get(const uint8_t* p_bitmap, uint16_t idx)
{
    return (p_bitmap[idx >> 3] >> (idx & 7u)) & 1u;
}

/* ---- encoders -------------------------------------------------------------------- */

uint16_t alfp_enc_manifest(uint8_t* p_out, uint16_t cap, uint16_t sid, const alfp_manifest_t* p_manifest)
{
    uint16_t len = (uint16_t)(ALFP_MANIFEST_OVERHEAD + p_manifest->meta_len);
    if ((len > cap) || (p_manifest->meta_len > ALFP_MAX_META))
    {
        return 0;
    }
    p_out[0] = b0(ALFP_T_MANIFEST);
    put16(p_out + 1, sid);
    put32(p_out + 3, p_manifest->file_size);
    put16(p_out + 7, p_manifest->frag_count);
    put16(p_out + 9, p_manifest->frag_size);
    put32(p_out + 11, p_manifest->file_crc32);
    p_out[15] = p_manifest->radio_id;
    p_out[16] = p_manifest->meta_len;
    if (p_manifest->meta_len)
    {
        memcpy(p_out + 17, p_manifest->p_meta, p_manifest->meta_len);
    }
    return seal(p_out, (uint16_t)(17u + p_manifest->meta_len));
}

uint16_t alfp_enc_data_header(uint8_t* p_out, uint16_t sid, uint16_t seq, uint16_t count)
{
    p_out[0] = b0(ALFP_T_DATA);
    put16(p_out + 1, sid);
    put16(p_out + 3, seq);
    put16(p_out + 5, count);
    return 7u; /* payload goes at out + 7 */
}

uint16_t alfp_enc_data_finish(uint8_t* p_out, uint16_t payload_len)
{
    return seal(p_out, (uint16_t)(7u + payload_len));
}

uint16_t alfp_enc_poll(uint8_t* p_out, uint16_t cap, uint16_t sid, uint8_t round)
{
    if (cap < ALFP_POLL_LEN)
    {
        return 0;
    }
    p_out[0] = b0(ALFP_T_POLL);
    put16(p_out + 1, sid);
    p_out[3] = round;
    return seal(p_out, 4);
}

uint16_t alfp_enc_ack(uint8_t* p_out, uint16_t cap, uint16_t sid, uint8_t status)
{
    if (cap < ALFP_ACK_LEN)
    {
        return 0;
    }
    p_out[0] = b0(ALFP_T_ACK);
    put16(p_out + 1, sid);
    p_out[3] = status;
    return seal(p_out, 4);
}

uint16_t alfp_enc_nack(uint8_t* p_out, uint16_t cap, uint16_t sid, uint8_t flags, uint8_t round,
                       const uint8_t* p_have_bitmap, uint16_t frag_count, bool b_all)
{
    if (cap < ALFP_NACK_MIN)
    {
        return 0;
    }
    p_out[0] = b0(ALFP_T_NACK);
    put16(p_out + 1, sid);
    p_out[3] = flags;
    p_out[4] = round;

    if (b_all || (NULL == p_have_bitmap) || (0 == frag_count))
    {
        p_out[5] = ALFP_NACK_FMT_ALL;
        return seal(p_out, 6);
    }

    /* Find the missing range and count. */
    uint16_t n_missing = 0, first = 0, last = 0;
    for (uint16_t idx = 0; idx < frag_count; idx++)
    {
        if (!bit_get(p_have_bitmap, idx))
        {
            if (0 == n_missing)
            {
                first = idx;
            }
            last = idx;
            n_missing++;
        }
    }

    if (0 == n_missing)
    {
        p_out[5] = ALFP_NACK_FMT_NONE;
        return seal(p_out, 6);
    }

    uint16_t nbits = (uint16_t)(last - first + 1u);
    uint32_t list_body = 1u + 2u * (uint32_t)n_missing;
    uint32_t bitmap_body = 4u + (uint32_t)((nbits + 7u) / 8u);
    uint32_t room = (uint32_t)cap - 8u; /* 6 header bytes + 2 crc */

    bool b_use_list = (n_missing <= 255u) && (list_body <= bitmap_body) && (list_body <= room);
    if (b_use_list)
    {
        p_out[5] = ALFP_NACK_FMT_LIST;
        p_out[6] = (uint8_t)n_missing;
        uint16_t width = 7;
        for (uint16_t idx = first; idx <= last; idx++)
        {
            if (!bit_get(p_have_bitmap, idx))
            {
                put16(p_out + width, idx);
                width = (uint16_t)(width + 2u);
            }
        }
        return seal(p_out, width);
    }

    if (bitmap_body > room)
    {
        /* Cannot happen with ALFP_MAX_FRAGS=512 (64-byte bitmap), but stay safe: ask for everything. */
        p_out[5] = ALFP_NACK_FMT_ALL;
        return seal(p_out, 6);
    }

    p_out[5] = ALFP_NACK_FMT_BITMAP;
    put16(p_out + 6, first);
    put16(p_out + 8, nbits);
    uint16_t nbytes = (uint16_t)((nbits + 7u) / 8u);
    memset(p_out + 10, 0, nbytes);
    for (uint16_t idx = 0; idx < nbits; idx++)
    {
        if (!bit_get(p_have_bitmap, (uint16_t)(first + idx)))
        {
            p_out[10 + (idx >> 3)] |= (uint8_t)(1u << (idx & 7u));
        }
    }
    return seal(p_out, (uint16_t)(10u + nbytes));
}

/* ---- decoder --------------------------------------------------------------------- */

alfp_dec_t alfp_decode(const uint8_t* p_buf, uint16_t len, alfp_pkt_t* p_pkt)
{
    if (len < 5u) /* b0 + sid2 + crc2 is the shortest conceivable packet */
    {
        return ALFP_DEC_SHORT;
    }
    uint16_t want = get16(p_buf + len - 2u);
    if (alfp_crc16(ALFP_CRC16_INIT, p_buf, (size_t)(len - 2u)) != want)
    {
        return ALFP_DEC_CRC;
    }
    if (ALFP_VERSION != (p_buf[0] >> 4))
    {
        return ALFP_DEC_VERSION;
    }

    uint16_t body = (uint16_t)(len - 2u); /* bytes before the CRC */
    memset(p_pkt, 0, sizeof(*p_pkt));
    p_pkt->type = p_buf[0] & 0x0Fu;
    p_pkt->sid = get16(p_buf + 1);

    switch (p_pkt->type)
    {
        case ALFP_T_MANIFEST:
        {
            if (len < ALFP_MANIFEST_OVERHEAD)
            {
                return ALFP_DEC_SHORT;
            }
            alfp_manifest_t* p_manifest = &p_pkt->body.manifest;
            p_manifest->file_size = get32(p_buf + 3);
            p_manifest->frag_count = get16(p_buf + 7);
            p_manifest->frag_size = get16(p_buf + 9);
            p_manifest->file_crc32 = get32(p_buf + 11);
            p_manifest->radio_id = p_buf[15];
            p_manifest->meta_len = p_buf[16];
            p_manifest->p_meta = p_buf + 17;
            if (((uint32_t)17u + p_manifest->meta_len != body) || (p_manifest->meta_len > ALFP_MAX_META))
            {
                return ALFP_DEC_BAD_FIELD;
            }
            if ((0 == p_manifest->frag_size) || (0 == p_manifest->frag_count) || (0 == p_manifest->file_size))
            {
                return ALFP_DEC_BAD_FIELD;
            }
            /* frag_count must be exactly ceil(file_size / frag_size) */
            uint32_t expect = (p_manifest->file_size + p_manifest->frag_size - 1u) / p_manifest->frag_size;
            if (expect != p_manifest->frag_count)
            {
                return ALFP_DEC_BAD_FIELD;
            }
            return ALFP_DEC_OK;
        }
        case ALFP_T_DATA:
        {
            if (len < ALFP_DATA_OVERHEAD + 1u)
            {
                return ALFP_DEC_SHORT;
            }
            alfp_data_t* p_data = &p_pkt->body.data;
            p_data->seq = get16(p_buf + 3);
            p_data->frag_count = get16(p_buf + 5);
            p_data->p_payload = p_buf + 7;
            p_data->len = (uint16_t)(body - 7u);
            if ((0 == p_data->frag_count) || (p_data->seq >= p_data->frag_count))
            {
                return ALFP_DEC_BAD_FIELD;
            }
            return ALFP_DEC_OK;
        }
        case ALFP_T_POLL:
            if (ALFP_POLL_LEN != len)
            {
                return ALFP_DEC_SHORT;
            }
            p_pkt->body.round = p_buf[3];
            return ALFP_DEC_OK;
        case ALFP_T_ACK:
            if (ALFP_ACK_LEN != len)
            {
                return ALFP_DEC_SHORT;
            }
            p_pkt->body.status = p_buf[3];
            return ALFP_DEC_OK;
        case ALFP_T_NACK:
        {
            if (len < ALFP_NACK_MIN)
            {
                return ALFP_DEC_SHORT;
            }
            alfp_nack_t* p_nack = &p_pkt->body.nack;
            p_nack->flags = p_buf[3];
            p_nack->round = p_buf[4];
            p_nack->fmt = p_buf[5];
            switch (p_nack->fmt)
            {
                case ALFP_NACK_FMT_ALL:
                case ALFP_NACK_FMT_NONE:
                    return 6u == body ? ALFP_DEC_OK : ALFP_DEC_BAD_FIELD;
                case ALFP_NACK_FMT_LIST:
                    if (body < 7u)
                    {
                        return ALFP_DEC_SHORT;
                    }
                    p_nack->count = p_buf[6];
                    p_nack->p_list = p_buf + 7;
                    return (7u + 2u * (uint32_t)p_nack->count == body) ? ALFP_DEC_OK : ALFP_DEC_BAD_FIELD;
                case ALFP_NACK_FMT_BITMAP:
                    if (body < 10u)
                    {
                        return ALFP_DEC_SHORT;
                    }
                    p_nack->base = get16(p_buf + 6);
                    p_nack->nbits = get16(p_buf + 8);
                    p_nack->p_bitmap = p_buf + 10;
                    if ((0 == p_nack->nbits) || (10u + (uint32_t)(p_nack->nbits + 7u) / 8u != body))
                    {
                        return ALFP_DEC_BAD_FIELD;
                    }
                    return ALFP_DEC_OK;
                default:
                    return ALFP_DEC_BAD_FIELD;
            }
        }
        default:
            return ALFP_DEC_TYPE;
    }
}

void alfp_nack_for_each(const alfp_nack_t* p_nack, uint16_t frag_count, void (*p_callback)(uint16_t seq, void* p_user), void* p_user)
{
    switch (p_nack->fmt)
    {
        case ALFP_NACK_FMT_ALL:
            for (uint16_t idx = 0; idx < frag_count; idx++)
            {
                p_callback(idx, p_user);
            }
            break;
        case ALFP_NACK_FMT_LIST:
            for (uint16_t idx = 0; idx < p_nack->count; idx++)
            {
                uint16_t seq = get16(p_nack->p_list + 2u * idx);
                if (seq < frag_count)
                {
                    p_callback(seq, p_user);
                }
            }
            break;
        case ALFP_NACK_FMT_BITMAP:
            for (uint16_t idx = 0; idx < p_nack->nbits; idx++)
            {
                if ((p_nack->p_bitmap[idx >> 3] >> (idx & 7u)) & 1u)
                {
                    uint32_t seq = (uint32_t)p_nack->base + idx;
                    if (seq < frag_count)
                    {
                        p_callback((uint16_t)seq, p_user);
                    }
                }
            }
            break;
        default:
            break;
    }
}
