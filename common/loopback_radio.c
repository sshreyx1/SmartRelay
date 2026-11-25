/*
 * loopback_radio.c - see loopback_radio.h.
 */
#include "loopback_radio.h"

#include <string.h>

#define Q 16
#define MAXP 250

typedef struct
{
    uint8_t b[MAXP];
    uint16_t len;
    uint32_t due;
} item_t;

typedef struct
{
    item_t q[Q]; /* packets waiting to be received by this endpoint */
    uint8_t head, n;
    uint32_t busy_until;
    uint8_t id;
    mesh_stats_t st;
    float drop;
} end_t;

static struct
{
    end_t e[2];
    uint32_t (*now)(void);
    uint32_t (*rnd)(void);
    uint16_t maxp;
    uint32_t air, lat;
    float loss;
} L;

void loopback_init(uint32_t (*now_ms)(void), uint32_t (*rand32)(void), uint16_t max_payload, uint32_t airtime_ms,
                   uint32_t latency_ms)
{
    memset(&L, 0, sizeof(L));
    L.now = now_ms;
    L.rnd = rand32;
    L.maxp = max_payload > MAXP ? MAXP : max_payload;
    L.air = airtime_ms;
    L.lat = latency_ms;
    L.e[0].id = 2;
    L.e[1].id = 1;
}

void loopback_set_loss(float loss)
{
    L.loss = loss;
}

static float frand(void)
{
    return (float)(L.rnd() >> 8) / 16777216.0f;
}

static bool l_send(int me, uint8_t dst, const uint8_t* p, uint16_t len)
{
    (void)dst;
    end_t* s = &L.e[me];
    end_t* d = &L.e[1 - me];
    if (len == 0 || len > L.maxp)
    {
        return false;
    }
    uint32_t t = L.now();
    if ((int32_t)(s->busy_until - t) > 0)
    {
        return false; /* one packet in flight at a time, like a real radio queue that is full */
    }
    s->busy_until = t + L.air;
    s->st.tx++;
    if (frand() < L.loss || (d->drop > 0 && frand() < d->drop))
    {
        d->st.drop_injected++;
        return true; /* accepted, then lost in the air */
    }
    if (d->n >= Q)
    {
        d->st.rx_overflow++;
        return true;
    }
    item_t* it = &d->q[(d->head + d->n) % Q];
    memcpy(it->b, p, len);
    it->len = len;
    it->due = t + L.air + L.lat;
    d->n++;
    return true;
}

static int l_recv(int me, uint8_t* src, uint8_t* buf, uint16_t cap)
{
    end_t* e = &L.e[me];
    if (!e->n || (int32_t)(L.now() - e->q[e->head].due) < 0)
    {
        return MESH_RECV_NONE;
    }
    const item_t* it = &e->q[e->head];
    e->head = (uint8_t)((e->head + 1) % Q);
    e->n--;
    if (it->len > cap)
    {
        return MESH_RECV_BUF_TOO_SMALL;
    }
    memcpy(buf, it->b, it->len);
    *src = L.e[1 - me].id;
    e->st.rx++;
    e->st.delivered++;
    return it->len;
}

static bool l_heard(int me, uint8_t node, uint32_t* ms)
{
    if (node != L.e[1 - me].id)
    {
        return false;
    }
    *ms = L.now();
    return true;
}

static bool l_busy(int me)
{
    return (int32_t)(L.e[me].busy_until - L.now()) > 0;
}

/* DEVIATION (BARR-C 6.3.b.iv, return inside a macro): END() generates the complete callback
 * functions for one loopback endpoint, so the returns are the generated functions' own and never
 * jump out of the code that uses the macro. The only alternative is hand-copying each function.
 * Approved by: ____________________ (project manager), date: __________ */
#define END(N, I)                                                                                                      \
    static bool N##_send(uint8_t d, const uint8_t* p, uint16_t l)                                                      \
    {                                                                                                                  \
        return l_send(I, d, p, l);                                                                                     \
    }                                                                                                                  \
    static int N##_recv(uint8_t* s, uint8_t* b, uint16_t c)                                                            \
    {                                                                                                                  \
        return l_recv(I, s, b, c);                                                                                     \
    }                                                                                                                  \
    static void N##_poll(void)                                                                                         \
    {                                                                                                                  \
    }                                                                                                                  \
    static uint16_t N##_maxp(void)                                                                                     \
    {                                                                                                                  \
        return L.maxp;                                                                                                 \
    }                                                                                                                  \
    static bool N##_heard(uint8_t n, uint32_t* ms)                                                                     \
    {                                                                                                                  \
        return l_heard(I, n, ms);                                                                                      \
    }                                                                                                                  \
    static bool N##_busy(void)                                                                                         \
    {                                                                                                                  \
        return l_busy(I);                                                                                              \
    }                                                                                                                  \
    static void N##_stats(mesh_stats_t* o)                                                                             \
    {                                                                                                                  \
        *o = L.e[I].st;                                                                                                \
    }                                                                                                                  \
    static void N##_drop(float r)                                                                                      \
    {                                                                                                                  \
        L.e[I].drop = r;                                                                                               \
    }                                                                                                                  \
    const mesh_radio_t loopback_##N                                                                                    \
        = { "lora", N##_send, N##_recv, N##_poll, N##_maxp, N##_heard, N##_busy, N##_stats, N##_drop };

END(a, 0)
END(b, 1)
