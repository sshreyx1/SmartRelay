/*
 * sim_mesh.c - see sim_mesh.h.
 */
#include "sim_mesh.h"

#include <math.h>
#include <string.h>

#define FRAME_MAX 260
#define TXQ_LEN 8      /* contract: send queue of 8 */
#define RELAYQ_LEN 8
#define RXQ_LEN 16
#define SEEN_LEN 128
#define SEEN_WINDOW_MS 30000u
#define HOP_LIMIT 4
#define LOG_LEN 256

typedef struct
{
    uint8_t b[FRAME_MAX];
    uint16_t len;
} frame_t;

typedef struct
{
    frame_t f;
    uint32_t ready_ms;
    bool used;
} relay_t;

typedef struct
{
    uint8_t src;
    uint8_t payload[FRAME_MAX];
    uint16_t len;
} rxitem_t;

typedef struct
{
    uint8_t src;
    uint16_t pkt_id;
    uint32_t t;
    bool used;
} seen_t;

typedef struct
{
    frame_t txq[TXQ_LEN];
    uint8_t txq_head, txq_n;
    relay_t relayq[RELAYQ_LEN];
    rxitem_t rxq[RXQ_LEN];
    uint8_t rxq_head, rxq_n;
    seen_t seen[SEEN_LEN];
    uint8_t seen_next;
    bool heard[SIM_MAX_NODES + 1];
    uint32_t heard_ms[SIM_MAX_NODES + 1];
    /* current transmission */
    bool on;
    frame_t cur;
    uint32_t start_ms, air_start_ms, end_ms;
    uint32_t backoff_until;
    uint16_t pkt_id;
    float drop_rate;
    uint32_t next_hb;
    mesh_stats_t st;
} radio_t;

typedef struct
{
    uint8_t node;
    uint32_t air_start, end;
} txlog_t;

static struct
{
    sim_cfg_t cfg;
    uint32_t now;
    uint64_t rng;
    uint8_t cur;
    radio_t r[SIM_MAX_NODES + 1][SIM_KINDS];
    txlog_t log[SIM_KINDS][LOG_LEN];
    uint16_t log_next[SIM_KINDS];
    uint32_t air_used[SIM_KINDS];
} W;

/* ---- rng (xorshift64*) ------------------------------------------------------------------ */

uint32_t sim_rand(void)
{
    W.rng ^= W.rng >> 12;
    W.rng ^= W.rng << 25;
    W.rng ^= W.rng >> 27;
    return (uint32_t)((W.rng * 2685821657736338717ULL) >> 32);
}

float sim_frand(void)
{
    return (float)(sim_rand() >> 8) / (float)(1u << 24);
}

/* ---- config ------------------------------------------------------------------------------ */

void sim_default_cfg(sim_cfg_t* cfg, uint8_t n_nodes)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->seed = 1;
    cfg->n_nodes = n_nodes;
    cfg->bridge_id = 1;
    cfg->heartbeat_ms = 3000;
    cfg->kind[SIM_LORA] = (sim_kind_cfg_t){0.0f, 0.0f, 10, 100, false, 250, 0};
    cfg->kind[SIM_WIFI] = (sim_kind_cfg_t){0.0f, 0.0f, 5, 30, true, 245, 115200};
}

void sim_link(sim_cfg_t* cfg, int kind, uint8_t a, uint8_t b)
{
    for (int k = 0; k < SIM_KINDS; k++)
    {
        if (kind < 0 || kind == k)
        {
            cfg->adj[k][a][b] = cfg->adj[k][b][a] = true;
        }
    }
}

void sim_topology_chain(sim_cfg_t* cfg, int kind)
{
    for (uint8_t i = 1; i < cfg->n_nodes; i++)
    {
        sim_link(cfg, kind, i, (uint8_t)(i + 1));
    }
}

void sim_topology_full(sim_cfg_t* cfg, int kind)
{
    for (uint8_t i = 1; i <= cfg->n_nodes; i++)
    {
        for (uint8_t j = (uint8_t)(i + 1); j <= cfg->n_nodes; j++)
        {
            sim_link(cfg, kind, i, j);
        }
    }
}

void sim_topology_trail(sim_cfg_t* cfg, int kind)
{
    sim_topology_chain(cfg, kind);
    for (uint8_t i = 1; i + 2 <= cfg->n_nodes; i += 2)
    {
        sim_link(cfg, kind, i, (uint8_t)(i + 2));
    }
}

void sim_init(const sim_cfg_t* cfg)
{
    memset(&W, 0, sizeof(W));
    W.cfg = *cfg;
    W.rng = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)cfg->seed * 0xBF58476D1CE4E5B9ULL);
    if (!W.rng)
    {
        W.rng = 1;
    }
    for (int i = 0; i < 4; i++)
    {
        sim_rand();
    }
    for (uint8_t n = 1; n <= cfg->n_nodes; n++)
    {
        for (int k = 0; k < SIM_KINDS; k++)
        {
            W.r[n][k].pkt_id = (uint16_t)sim_rand();
            W.r[n][k].next_hb = 100u + n; /* first heartbeat shortly after boot */
        }
    }
    W.cur = 1;
}

void sim_set_link(int kind, uint8_t a, uint8_t b, bool up)
{
    W.cfg.adj[kind][a][b] = W.cfg.adj[kind][b][a] = up;
}

void sim_set_link_loss(int kind, float loss)
{
    W.cfg.kind[kind].link_loss = loss;
}

void sim_set_corrupt_rate(int kind, float rate)
{
    W.cfg.kind[kind].corrupt_rate = rate;
}

void sim_set_current(uint8_t node_id)
{
    W.cur = node_id;
}

uint32_t sim_now(void)
{
    return W.now;
}

/* ---- airtime ----------------------------------------------------------------------------- */

double sim_lora_airtime_ms(uint16_t n)
{
    const int sf = 7, cr = 1, h = 0, de = 0, crc = 1, npre = 8;
    const double bw = 125000.0;
    double ts = (double)(1 << sf) / bw * 1000.0;
    double tpre = (npre + 4.25) * ts;
    double num = 8.0 * n - 4.0 * sf + 28 + 16 * crc - 20 * h;
    double den = 4.0 * (sf - 2 * de);
    double sym = ceil(num / den) * (cr + 4);
    if (sym < 0)
    {
        sym = 0;
    }
    return tpre + (8 + sym) * ts;
}

static void airtime(int kind, uint16_t frame_len, uint32_t* uart_ms, uint32_t* air_ms)
{
    if (kind == SIM_LORA)
    {
        *uart_ms = 0;
        *air_ms = (uint32_t)ceil(sim_lora_airtime_ms(frame_len));
    }
    else
    {
        /* Pico -> ESP8266: [0xAA][len][payload][crc8] at 10 bits per byte. */
        double u = (frame_len + 3.0) * 10.0 / (double)W.cfg.kind[kind].uart_baud * 1000.0;
        *uart_ms = (uint32_t)ceil(u);
        /* ESP-NOW at 1 Mbit/s plus preamble/MAC overhead (~0.5 ms). */
        *air_ms = (uint32_t)ceil(frame_len * 8.0 / 1000.0 + 0.5);
    }
}

/* ---- queues ------------------------------------------------------------------------------ */

static bool txq_push(radio_t* r, const frame_t* f)
{
    if (r->txq_n >= TXQ_LEN)
    {
        return false;
    }
    r->txq[(r->txq_head + r->txq_n) % TXQ_LEN] = *f;
    r->txq_n++;
    return true;
}

static bool seen_check_add(radio_t* r, uint8_t src, uint16_t id)
{
    for (int i = 0; i < SEEN_LEN; i++)
    {
        seen_t* s = &r->seen[i];
        if (s->used && s->src == src && s->pkt_id == id && W.now - s->t <= SEEN_WINDOW_MS)
        {
            return true;
        }
    }
    seen_t* s = &r->seen[r->seen_next];
    r->seen_next = (uint8_t)((r->seen_next + 1) % SEEN_LEN);
    s->used = true;
    s->src = src;
    s->pkt_id = id;
    s->t = W.now;
    return false;
}

/* A frame arrived intact at node n. Mesh rules from the contract. */
static void on_frame(uint8_t n, int kind, const frame_t* f)
{
    radio_t* r = &W.r[n][kind];
    uint8_t src = f->b[0], dst = f->b[1], hops = f->b[4];
    uint16_t id = (uint16_t)(f->b[2] | (f->b[3] << 8));
    r->st.rx++;
    if (src == n || seen_check_add(r, src, id))
    {
        r->st.dup_dropped++;
        return;
    }
    if (src <= SIM_MAX_NODES)
    {
        r->heard[src] = true;
        r->heard_ms[src] = W.now;
    }
    if (dst == n || dst == MESH_BROADCAST)
    {
        if (r->rxq_n >= RXQ_LEN)
        {
            r->st.rx_overflow++;
        }
        else
        {
            rxitem_t* it = &r->rxq[(r->rxq_head + r->rxq_n) % RXQ_LEN];
            it->src = src;
            it->len = (uint16_t)(f->len - 5);
            memcpy(it->payload, f->b + 5, it->len);
            if (W.cfg.kind[kind].corrupt_rate > 0 && sim_frand() < W.cfg.kind[kind].corrupt_rate && it->len)
            {
                uint32_t bit = sim_rand() % ((uint32_t)it->len * 8u);
                it->payload[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            }
            r->rxq_n++;
            r->st.delivered++;
        }
    }
    if (dst != n)
    {
        if (hops == 0)
        {
            r->st.hop_dropped++;
            return;
        }
        for (int i = 0; i < RELAYQ_LEN; i++)
        {
            if (!r->relayq[i].used)
            {
                relay_t* q = &r->relayq[i];
                q->used = true;
                q->f = *f;
                q->f.b[4] = (uint8_t)(hops - 1);
                const sim_kind_cfg_t* kc = &W.cfg.kind[kind];
                q->ready_ms = W.now + kc->relay_min_ms + sim_rand() % (uint32_t)(kc->relay_max_ms - kc->relay_min_ms + 1);
                return;
            }
        }
        r->st.relay_overflow++;
    }
}

static bool overlaps(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1)
{
    return a0 < b1 && b0 < a1;
}

/* Transmission from node j finished: decide who heard it. */
static void finish_tx(uint8_t j, int kind)
{
    radio_t* rj = &W.r[j][kind];
    uint32_t s = rj->air_start_ms, e = rj->end_ms;
    for (uint8_t i = 1; i <= W.cfg.n_nodes; i++)
    {
        if (i == j || !W.cfg.adj[kind][i][j])
        {
            continue;
        }
        bool lost = false;
        /* half duplex + collisions: any overlapping air time at i itself or from another neighbour of i */
        for (int k = 0; k < LOG_LEN && !lost; k++)
        {
            const txlog_t* l = &W.log[kind][k];
            if (l->end == 0 || l->node == j)
            {
                continue;
            }
            if ((l->node == i || W.cfg.adj[kind][i][l->node]) && overlaps(s, e, l->air_start, l->end))
            {
                lost = true;
            }
        }
        /* i was busy pushing its own frame over UART/air even if not yet on air */
        radio_t* ri = &W.r[i][kind];
        if (!lost && ri->on && overlaps(s, e, ri->air_start_ms, ri->end_ms))
        {
            lost = true;
        }
        if (lost)
        {
            ri->st.rx_error++;
            continue;
        }
        if (sim_frand() < W.cfg.kind[kind].link_loss)
        {
            continue;
        }
        if (ri->drop_rate > 0 && sim_frand() < ri->drop_rate)
        {
            ri->st.drop_injected++;
            continue;
        }
        on_frame(i, kind, &rj->cur);
    }
}

static bool channel_busy_near(uint8_t n, int kind)
{
    for (uint8_t k = 1; k <= W.cfg.n_nodes; k++)
    {
        if (k != n && W.cfg.adj[kind][n][k])
        {
            radio_t* r = &W.r[k][kind];
            if (r->on && W.now >= r->air_start_ms && W.now < r->end_ms)
            {
                return true;
            }
        }
    }
    return false;
}

static void start_tx(uint8_t n, int kind, const frame_t* f, bool own)
{
    radio_t* r = &W.r[n][kind];
    uint32_t u, a;
    airtime(kind, f->len, &u, &a);
    r->on = true;
    r->cur = *f;
    r->start_ms = W.now;
    r->air_start_ms = W.now + u;
    r->end_ms = W.now + u + a;
    txlog_t* l = &W.log[kind][W.log_next[kind]];
    W.log_next[kind] = (uint16_t)((W.log_next[kind] + 1) % LOG_LEN);
    l->node = n;
    l->air_start = r->air_start_ms;
    l->end = r->end_ms;
    W.air_used[kind] += a;
    if (own)
    {
        r->st.tx++;
    }
    else
    {
        r->st.relayed++;
    }
}

/* Mesh poll for node n (what lora_mesh.c / wifi_mesh.c poll() does). */
static void mesh_poll(uint8_t n, int kind)
{
    radio_t* r = &W.r[n][kind];
    if (W.cfg.heartbeat_ms && n == W.cfg.bridge_id && (int32_t)(W.now - r->next_hb) >= 0)
    {
        frame_t hb = {{n, 0xFE, (uint8_t)r->pkt_id, (uint8_t)(r->pkt_id >> 8), HOP_LIMIT, 'h'}, 6};
        r->pkt_id++;
        txq_push(r, &hb);
        r->next_hb = W.now + W.cfg.heartbeat_ms;
    }
    if (r->on || W.now < r->backoff_until)
    {
        return;
    }
    /* relays first (keeps the flood moving), then our own queue */
    int pick = -1;
    for (int i = 0; i < RELAYQ_LEN; i++)
    {
        if (r->relayq[i].used && (int32_t)(W.now - r->relayq[i].ready_ms) >= 0)
        {
            if (pick < 0 || (int32_t)(r->relayq[i].ready_ms - r->relayq[pick].ready_ms) < 0)
            {
                pick = i;
            }
        }
    }
    const frame_t* f = NULL;
    bool own = false;
    if (pick >= 0)
    {
        f = &r->relayq[pick].f;
    }
    else if (r->txq_n)
    {
        f = &r->txq[r->txq_head];
        own = true;
    }
    if (!f)
    {
        return;
    }
    if (W.cfg.kind[kind].carrier_sense && channel_busy_near(n, kind))
    {
        r->backoff_until = W.now + 1 + sim_rand() % 3;
        return;
    }
    start_tx(n, kind, f, own);
    if (own)
    {
        r->txq_head = (uint8_t)((r->txq_head + 1) % TXQ_LEN);
        r->txq_n--;
    }
    else
    {
        r->relayq[pick].used = false;
    }
}

void sim_tick(void)
{
    W.now++;
    for (uint8_t n = 1; n <= W.cfg.n_nodes; n++)
    {
        for (int k = 0; k < SIM_KINDS; k++)
        {
            radio_t* r = &W.r[n][k];
            if (r->on && W.now >= r->end_ms)
            {
                r->on = false;
                finish_tx(n, k);
            }
        }
    }
}

/* ---- mesh_radio_t for the current node ------------------------------------------------- */

static bool m_send(int kind, uint8_t dst, const uint8_t* p, uint16_t len)
{
    radio_t* r = &W.r[W.cur][kind];
    if (len == 0 || len > W.cfg.kind[kind].max_payload)
    {
        return false;
    }
    frame_t f;
    f.b[0] = W.cur;
    f.b[1] = dst;
    f.b[2] = (uint8_t)r->pkt_id;
    f.b[3] = (uint8_t)(r->pkt_id >> 8);
    f.b[4] = HOP_LIMIT;
    memcpy(f.b + 5, p, len);
    f.len = (uint16_t)(len + 5);
    if (!txq_push(r, &f))
    {
        return false;
    }
    r->pkt_id++;
    return true;
}

static int m_recv(int kind, uint8_t* src, uint8_t* buf, uint16_t cap)
{
    radio_t* r = &W.r[W.cur][kind];
    if (!r->rxq_n)
    {
        return MESH_RECV_NONE;
    }
    rxitem_t* it = &r->rxq[r->rxq_head];
    r->rxq_head = (uint8_t)((r->rxq_head + 1) % RXQ_LEN);
    r->rxq_n--;
    if (it->len > cap)
    {
        return MESH_RECV_BUF_TOO_SMALL;
    }
    *src = it->src;
    memcpy(buf, it->payload, it->len);
    return it->len;
}

static bool m_last_heard(int kind, uint8_t node, uint32_t* ms)
{
    radio_t* r = &W.r[W.cur][kind];
    if (node > SIM_MAX_NODES || !r->heard[node])
    {
        return false;
    }
    *ms = r->heard_ms[node];
    return true;
}

static bool m_tx_busy(int kind)
{
    radio_t* r = &W.r[W.cur][kind];
    return r->txq_n > 0 || r->on;
}

#define DEFINE_RADIO(NAME, KIND, STR)                                                                   \
    static bool NAME##_send(uint8_t d, const uint8_t* p, uint16_t l) { return m_send(KIND, d, p, l); } \
    static int NAME##_recv(uint8_t* s, uint8_t* b, uint16_t c) { return m_recv(KIND, s, b, c); }       \
    static void NAME##_poll(void) { mesh_poll(W.cur, KIND); }                                           \
    static uint16_t NAME##_maxp(void) { return W.cfg.kind[KIND].max_payload; }                          \
    static bool NAME##_heard(uint8_t n, uint32_t* ms) { return m_last_heard(KIND, n, ms); }             \
    static bool NAME##_busy(void) { return m_tx_busy(KIND); }                                           \
    static void NAME##_stats(mesh_stats_t* o) { *o = W.r[W.cur][KIND].st; }                             \
    static void NAME##_drop(float r) { W.r[W.cur][KIND].drop_rate = r; }                                \
    const mesh_radio_t sim_##NAME = {STR,           NAME##_send,  NAME##_recv,  NAME##_poll, NAME##_maxp, \
                                     NAME##_heard,  NAME##_busy,  NAME##_stats, NAME##_drop};

DEFINE_RADIO(lora, SIM_LORA, "lora")
DEFINE_RADIO(wifi, SIM_WIFI, "wifi")

void sim_get_stats(uint8_t node_id, int kind, mesh_stats_t* out)
{
    *out = W.r[node_id][kind].st;
}

void sim_set_drop_rate(uint8_t node_id, int kind, float rate)
{
    W.r[node_id][kind].drop_rate = rate;
}

uint32_t sim_airtime_used(int kind)
{
    return W.air_used[kind];
}
