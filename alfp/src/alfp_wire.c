/*
 * alfp_wire.c - encode/decode ALFP packets. Pure functions, no state, no allocation.
 */
#include "alfp_wire.h"

#include <string.h>

#include "alfp_crc.h"

/* ---- little-endian helpers ------------------------------------------------------ */

static void put16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t b0(uint8_t type)
{
    return (uint8_t)((ALFP_VERSION << 4) | (type & 0x0Fu));
}

/* Appends the CRC-16 of out[0..len) at out[len], returns len + 2. */
static uint16_t seal(uint8_t* out, uint16_t len)
{
    put16(out + len, alfp_crc16(ALFP_CRC16_INIT, out, len));
    return (uint16_t)(len + 2u);
}

static bool bit_get(const uint8_t* bm, uint16_t i)
{
    return (bm[i >> 3] >> (i & 7u)) & 1u;
}

/* ---- encoders -------------------------------------------------------------------- */

uint16_t alfp_enc_manifest(uint8_t* out, uint16_t cap, uint16_t sid, const alfp_manifest_t* m)
{
    uint16_t len = (uint16_t)(ALFP_MANIFEST_OVERHEAD + m->meta_len);
    if (len > cap || m->meta_len > ALFP_MAX_META)
    {
        return 0;
    }
    out[0] = b0(ALFP_T_MANIFEST);
    put16(out + 1, sid);
    put32(out + 3, m->file_size);
    put16(out + 7, m->frag_count);
    put16(out + 9, m->frag_size);
    put32(out + 11, m->file_crc32);
    out[15] = m->radio_id;
    out[16] = m->meta_len;
    if (m->meta_len)
    {
        memcpy(out + 17, m->meta, m->meta_len);
    }
    return seal(out, (uint16_t)(17u + m->meta_len));
}

uint16_t alfp_enc_data_header(uint8_t* out, uint16_t sid, uint16_t seq, uint16_t count)
{
    out[0] = b0(ALFP_T_DATA);
    put16(out + 1, sid);
    put16(out + 3, seq);
    put16(out + 5, count);
    return 7u; /* payload goes at out + 7 */
}

uint16_t alfp_enc_data_finish(uint8_t* out, uint16_t payload_len)
{
    return seal(out, (uint16_t)(7u + payload_len));
}

uint16_t alfp_enc_poll(uint8_t* out, uint16_t cap, uint16_t sid, uint8_t round)
{
    if (cap < ALFP_POLL_LEN)
    {
        return 0;
    }
    out[0] = b0(ALFP_T_POLL);
    put16(out + 1, sid);
    out[3] = round;
    return seal(out, 4);
}

uint16_t alfp_enc_ack(uint8_t* out, uint16_t cap, uint16_t sid, uint8_t status)
{
    if (cap < ALFP_ACK_LEN)
    {
        return 0;
    }
    out[0] = b0(ALFP_T_ACK);
    put16(out + 1, sid);
    out[3] = status;
    return seal(out, 4);
}

uint16_t alfp_enc_nack(uint8_t* out, uint16_t cap, uint16_t sid, uint8_t flags, uint8_t round,
                       const uint8_t* have_bitmap, uint16_t frag_count, bool all)
{
    if (cap < ALFP_NACK_MIN)
    {
        return 0;
    }
    out[0] = b0(ALFP_T_NACK);
    put16(out + 1, sid);
    out[3] = flags;
    out[4] = round;

    if (all || have_bitmap == NULL || frag_count == 0)
    {
        out[5] = ALFP_NACK_FMT_ALL;
        return seal(out, 6);
    }

    /* Find the missing range and count. */
    uint16_t n_missing = 0, first = 0, last = 0;
    for (uint16_t i = 0; i < frag_count; i++)
    {
        if (!bit_get(have_bitmap, i))
        {
            if (n_missing == 0)
            {
                first = i;
            }
            last = i;
            n_missing++;
        }
    }

    if (n_missing == 0)
    {
        out[5] = ALFP_NACK_FMT_NONE;
        return seal(out, 6);
    }

    uint16_t nbits = (uint16_t)(last - first + 1u);
    uint32_t list_body = 1u + 2u * (uint32_t)n_missing;
    uint32_t bitmap_body = 4u + (uint32_t)((nbits + 7u) / 8u);
    uint32_t room = (uint32_t)cap - 8u; /* 6 header bytes + 2 crc */

    bool use_list = (n_missing <= 255u) && (list_body <= bitmap_body) && (list_body <= room);
    if (use_list)
    {
        out[5] = ALFP_NACK_FMT_LIST;
        out[6] = (uint8_t)n_missing;
        uint16_t w = 7;
        for (uint16_t i = first; i <= last; i++)
        {
            if (!bit_get(have_bitmap, i))
            {
                put16(out + w, i);
                w = (uint16_t)(w + 2u);
            }
        }
        return seal(out, w);
    }

    if (bitmap_body > room)
    {
        /* Cannot happen with ALFP_MAX_FRAGS=512 (64-byte bitmap), but stay safe: ask for everything. */
        out[5] = ALFP_NACK_FMT_ALL;
        return seal(out, 6);
    }

    out[5] = ALFP_NACK_FMT_BITMAP;
    put16(out + 6, first);
    put16(out + 8, nbits);
    uint16_t nbytes = (uint16_t)((nbits + 7u) / 8u);
    memset(out + 10, 0, nbytes);
    for (uint16_t i = 0; i < nbits; i++)
    {
        if (!bit_get(have_bitmap, (uint16_t)(first + i)))
        {
            out[10 + (i >> 3)] |= (uint8_t)(1u << (i & 7u));
        }
    }
    return seal(out, (uint16_t)(10u + nbytes));
}

/* ---- decoder --------------------------------------------------------------------- */

alfp_dec_t alfp_decode(const uint8_t* buf, uint16_t len, alfp_pkt_t* p)
{
    if (len < 5u) /* b0 + sid2 + crc2 is the shortest conceivable packet */
    {
        return ALFP_DEC_SHORT;
    }
    uint16_t want = get16(buf + len - 2u);
    if (alfp_crc16(ALFP_CRC16_INIT, buf, (size_t)(len - 2u)) != want)
    {
        return ALFP_DEC_CRC;
    }
    if ((buf[0] >> 4) != ALFP_VERSION)
    {
        return ALFP_DEC_VERSION;
    }

    uint16_t body = (uint16_t)(len - 2u); /* bytes before the CRC */
    memset(p, 0, sizeof(*p));
    p->type = buf[0] & 0x0Fu;
    p->sid = get16(buf + 1);

    switch (p->type)
    {
        case ALFP_T_MANIFEST:
        {
            if (len < ALFP_MANIFEST_OVERHEAD)
            {
                return ALFP_DEC_SHORT;
            }
            alfp_manifest_t* m = &p->u.manifest;
            m->file_size = get32(buf + 3);
            m->frag_count = get16(buf + 7);
            m->frag_size = get16(buf + 9);
            m->file_crc32 = get32(buf + 11);
            m->radio_id = buf[15];
            m->meta_len = buf[16];
            m->meta = buf + 17;
            if ((uint32_t)17u + m->meta_len != body || m->meta_len > ALFP_MAX_META)
            {
                return ALFP_DEC_BAD_FIELD;
            }
            if (m->frag_size == 0 || m->frag_count == 0 || m->file_size == 0)
            {
                return ALFP_DEC_BAD_FIELD;
            }
            /* frag_count must be exactly ceil(file_size / frag_size) */
            uint32_t expect = (m->file_size + m->frag_size - 1u) / m->frag_size;
            if (expect != m->frag_count)
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
            alfp_data_t* d = &p->u.data;
            d->seq = get16(buf + 3);
            d->frag_count = get16(buf + 5);
            d->payload = buf + 7;
            d->len = (uint16_t)(body - 7u);
            if (d->frag_count == 0 || d->seq >= d->frag_count)
            {
                return ALFP_DEC_BAD_FIELD;
            }
            return ALFP_DEC_OK;
        }
        case ALFP_T_POLL:
            if (len != ALFP_POLL_LEN)
            {
                return ALFP_DEC_SHORT;
            }
            p->u.round = buf[3];
            return ALFP_DEC_OK;
        case ALFP_T_ACK:
            if (len != ALFP_ACK_LEN)
            {
                return ALFP_DEC_SHORT;
            }
            p->u.status = buf[3];
            return ALFP_DEC_OK;
        case ALFP_T_NACK:
        {
            if (len < ALFP_NACK_MIN)
            {
                return ALFP_DEC_SHORT;
            }
            alfp_nack_t* n = &p->u.nack;
            n->flags = buf[3];
            n->round = buf[4];
            n->fmt = buf[5];
            switch (n->fmt)
            {
                case ALFP_NACK_FMT_ALL:
                case ALFP_NACK_FMT_NONE:
                    return body == 6u ? ALFP_DEC_OK : ALFP_DEC_BAD_FIELD;
                case ALFP_NACK_FMT_LIST:
                    if (body < 7u)
                    {
                        return ALFP_DEC_SHORT;
                    }
                    n->n = buf[6];
                    n->list = buf + 7;
                    return (7u + 2u * (uint32_t)n->n == body) ? ALFP_DEC_OK : ALFP_DEC_BAD_FIELD;
                case ALFP_NACK_FMT_BITMAP:
                    if (body < 10u)
                    {
                        return ALFP_DEC_SHORT;
                    }
                    n->base = get16(buf + 6);
                    n->nbits = get16(buf + 8);
                    n->bitmap = buf + 10;
                    if (n->nbits == 0 || 10u + (uint32_t)(n->nbits + 7u) / 8u != body)
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

void alfp_nack_for_each(const alfp_nack_t* n, uint16_t frag_count, void (*fn)(uint16_t seq, void* user), void* user)
{
    switch (n->fmt)
    {
        case ALFP_NACK_FMT_ALL:
            for (uint16_t i = 0; i < frag_count; i++)
            {
                fn(i, user);
            }
            break;
        case ALFP_NACK_FMT_LIST:
            for (uint16_t i = 0; i < n->n; i++)
            {
                uint16_t seq = get16(n->list + 2u * i);
                if (seq < frag_count)
                {
                    fn(seq, user);
                }
            }
            break;
        case ALFP_NACK_FMT_BITMAP:
            for (uint16_t i = 0; i < n->nbits; i++)
            {
                if ((n->bitmap[i >> 3] >> (i & 7u)) & 1u)
                {
                    uint32_t seq = (uint32_t)n->base + i;
                    if (seq < frag_count)
                    {
                        fn((uint16_t)seq, user);
                    }
                }
            }
            break;
        default:
            break;
    }
}
