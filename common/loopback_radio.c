/*
 * loopback_radio.c - see loopback_radio.h.
 */
#include "float_types.h"
#include "loopback_radio.h"

#include <string.h>

#define Q    16
#define MAXP 250

typedef struct
{
    uint8_t  buf[MAXP];
    uint16_t len;
    uint32_t due;
} item_t;

typedef struct
{
    item_t       queue[Q]; /* packets waiting to be received by this endpoint */
    uint8_t      head;
    uint8_t      count;
    uint32_t     busy_until;
    uint8_t      node_id;
    mesh_stats_t stats;
    float32_t    drop;
} end_t;

static struct
{
    end_t ends[2];
    uint32_t (*p_now)(void);
    uint32_t (*p_rnd)(void);
    uint16_t  maxp;
    uint32_t  air;
    uint32_t  lat;
    float32_t loss;
} loopback;

void loopback_init (uint32_t (*p_now_ms)(void), uint32_t (*p_rand32)(void),
                    uint16_t max_payload, uint32_t airtime_ms,
                    uint32_t latency_ms)
{
    memset(&loopback, 0, sizeof(loopback));
    loopback.p_now           = p_now_ms;
    loopback.p_rnd           = p_rand32;
    loopback.maxp            = max_payload > MAXP ? MAXP : max_payload;
    loopback.air             = airtime_ms;
    loopback.lat             = latency_ms;
    loopback.ends[0].node_id = 2;
    loopback.ends[1].node_id = 1;
}

void loopback_set_loss (float32_t loss)
{
    loopback.loss = loss;
}

static float32_t frand (void)
{
    return (float32_t)(loopback.p_rnd() >> 8) / 16777216.0f;
}

static bool l_send (int self_id, uint8_t dst, const uint8_t* p_buf,
                    uint16_t len)
{
    (void)dst;
    end_t* p_self_end = &loopback.ends[self_id];
    end_t* p_dest     = &loopback.ends[1 - self_id];
    if ((0 == len) || (len > loopback.maxp))
    {
        return false;
    }
    uint32_t now_ms = loopback.p_now();
    if ((int32_t)(p_self_end->busy_until - now_ms) > 0)
    {
        /* one packet in flight at a time, like a real radio queue that is
         * full */
        return false;
    }
    p_self_end->busy_until = now_ms + loopback.air;
    p_self_end->stats.tx_count++;
    if ((frand() < loopback.loss) ||
        ((p_dest->drop > 0) && (frand() < p_dest->drop)))
    {
        p_dest->stats.drop_injected++;
        return true; /* accepted, then lost in the air */
    }
    if (p_dest->count >= Q)
    {
        p_dest->stats.rx_overflow++;
        return true;
    }
    item_t* p_item = &p_dest->queue[(p_dest->head + p_dest->count) % Q];
    memcpy(p_item->buf, p_buf, len);
    p_item->len = len;
    p_item->due = now_ms + loopback.air + loopback.lat;
    p_dest->count++;
    return true;
}

static int l_recv (int self_id, uint8_t* p_src, uint8_t* p_buf, uint16_t cap)
{
    end_t* p_self_end = &loopback.ends[self_id];
    if ((!p_self_end->count) ||
        ((int32_t)(loopback.p_now() - p_self_end->queue[p_self_end->head].due) <
         0))
    {
        return MESH_RECV_NONE;
    }
    const item_t* p_item = &p_self_end->queue[p_self_end->head];
    p_self_end->head     = (uint8_t)((p_self_end->head + 1) % Q);
    p_self_end->count--;
    if (p_item->len > cap)
    {
        return MESH_RECV_BUF_TOO_SMALL;
    }
    memcpy(p_buf, p_item->buf, p_item->len);
    *p_src = loopback.ends[1 - self_id].node_id;
    p_self_end->stats.rx_count++;
    p_self_end->stats.delivered++;
    return p_item->len;
}

static bool l_heard (int self_id, uint8_t node, uint32_t* p_msec)
{
    if (node != loopback.ends[1 - self_id].node_id)
    {
        return false;
    }
    *p_msec = loopback.p_now();
    return true;
}

static bool l_busy (int self_id)
{
    return (int32_t)(loopback.ends[self_id].busy_until - loopback.p_now()) > 0;
}

/* DEVIATION (BARR-C 6.3.b.iv, return inside a macro): END() generates the
 * complete callback functions for one loopback endpoint, so the returns are the
 * generated functions' own and never jump out of the code that uses the macro.
 * The only alternative is hand-copying each function.
 * Approved by: Senthil Shreya (project manager), 2026-11-28 */
#define END(N, I)                                                              \
    static bool N##_send(uint8_t dst, const uint8_t* p_buf, uint16_t len)      \
    {                                                                          \
        return l_send(I, dst, p_buf, len);                                     \
    }                                                                          \
    static int N##_recv(uint8_t* p_src, uint8_t* p_buf, uint16_t cap)          \
    {                                                                          \
        return l_recv(I, p_src, p_buf, cap);                                   \
    }                                                                          \
    static void N##_poll(void)                                                 \
    {                                                                          \
    }                                                                          \
    static uint16_t N##_maxp(void)                                             \
    {                                                                          \
        return loopback.maxp;                                                  \
    }                                                                          \
    static bool N##_heard(uint8_t node, uint32_t* p_msec)                      \
    {                                                                          \
        return l_heard(I, node, p_msec);                                       \
    }                                                                          \
    static bool N##_busy(void)                                                 \
    {                                                                          \
        return l_busy(I);                                                      \
    }                                                                          \
    static void N##_stats(mesh_stats_t* p_out)                                 \
    {                                                                          \
        *p_out = loopback.ends[I].stats;                                       \
    }                                                                          \
    static void N##_drop(float32_t rate)                                       \
    {                                                                          \
        loopback.ends[I].drop = rate;                                          \
    }                                                                          \
    const mesh_radio_t g_loopback_##N = {"lora",   N##_send,  N##_recv,        \
                                         N##_poll, N##_maxp,  N##_heard,       \
                                         N##_busy, N##_stats, N##_drop};

END(a, 0)
END(b, 1)

/*** end of file ***/

