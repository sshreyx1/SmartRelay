/*
 * sim_mesh.c - see sim_mesh.h.
 */
#include "float_types.h"
#include "sim_mesh.h"

#include <math.h>
#include <string.h>

#define FRAME_MAX      260
#define TXQ_LEN        8 /* contract: send queue of 8 */
#define RELAYQ_LEN     8
#define RXQ_LEN        16
#define SEEN_LEN       128
#define SEEN_WINDOW_MS 30000u
#define HOP_LIMIT      4
#define LOG_LEN        256

typedef struct
{
    uint8_t  buf[FRAME_MAX];
    uint16_t len;
} frame_t;

typedef struct
{
    frame_t  frame;
    uint32_t ready_ms;
    bool     b_used;
} relay_t;

typedef struct
{
    uint8_t  src;
    uint8_t  payload[FRAME_MAX];
    uint16_t len;
} rxitem_t;

typedef struct
{
    uint8_t  src;
    uint16_t pkt_id;
    uint32_t now_ms;
    bool     b_used;
} seen_t;

typedef struct
{
    frame_t  txq[TXQ_LEN];
    uint8_t  txq_head;
    uint8_t  txq_n;
    relay_t  relayq[RELAYQ_LEN];
    rxitem_t rxq[RXQ_LEN];
    uint8_t  rxq_head;
    uint8_t  rxq_n;
    seen_t   seen[SEEN_LEN];
    uint8_t  seen_next;
    bool     heard[SIM_MAX_NODES + 1];
    uint32_t heard_ms[SIM_MAX_NODES + 1];
    /* current transmission */
    bool         b_enable;
    frame_t      cur;
    uint32_t     start_ms;
    uint32_t     air_start_ms;
    uint32_t     end_ms;
    uint32_t     backoff_until;
    uint16_t     pkt_id;
    float32_t    drop_rate;
    uint32_t     next_hb;
    mesh_stats_t stats;
} radio_t;

typedef struct
{
    uint8_t  node;
    uint32_t air_start;
    uint32_t end;
} txlog_t;

static struct
{
    sim_cfg_t cfg;
    uint32_t  now;
    uint64_t  rng;
    uint8_t   cur;
    radio_t   radios[SIM_MAX_NODES + 1][SIM_KINDS];
    txlog_t   log[SIM_KINDS][LOG_LEN];
    uint16_t  log_next[SIM_KINDS];
    uint32_t  air_used[SIM_KINDS];
} world;

/* ---- rng (xorshift64*)
 * ------------------------------------------------------------------ */

uint32_t sim_rand (void)
{
    world.rng ^= world.rng >> 12;
    world.rng ^= world.rng << 25;
    world.rng ^= world.rng >> 27;
    return (uint32_t)((world.rng * 2685821657736338717ULL) >> 32);
}

float32_t sim_frand (void)
{
    return (float32_t)(sim_rand() >> 8) / (float32_t)(1u << 24);
}

/* ---- config
 * -----------------------------------------------------------------------------
 */

void sim_default_cfg (sim_cfg_t* p_cfg, uint8_t n_nodes)
{
    memset(p_cfg, 0, sizeof(*p_cfg));
    p_cfg->seed         = 1;
    p_cfg->n_nodes      = n_nodes;
    p_cfg->bridge_id    = 1;
    p_cfg->heartbeat_ms = 3000;
    p_cfg->kind[SIM_LORA] =
        (sim_kind_cfg_t){0.0f, 0.0f, 10, 100, false, 250, 0};
    p_cfg->kind[SIM_WIFI] =
        (sim_kind_cfg_t){0.0f, 0.0f, 5, 30, true, 245, 115200};
}

void sim_link (sim_cfg_t* p_cfg, int kind, uint8_t node_a, uint8_t node_b)
{
    for (int kdx = 0; kdx < SIM_KINDS; kdx++)
    {
        if ((kind < 0) || (kind == kdx))
        {
            p_cfg->adj[kdx][node_a][node_b] = p_cfg->adj[kdx][node_b][node_a] =
                true;
        }
    }
}

void sim_topology_chain (sim_cfg_t* p_cfg, int kind)
{
    for (uint8_t idx = 1; idx < p_cfg->n_nodes; idx++)
    {
        sim_link(p_cfg, kind, idx, (uint8_t)(idx + 1));
    }
}

void sim_topology_full (sim_cfg_t* p_cfg, int kind)
{
    for (uint8_t idx = 1; idx <= p_cfg->n_nodes; idx++)
    {
        for (uint8_t jdx = (uint8_t)(idx + 1); jdx <= p_cfg->n_nodes; jdx++)
        {
            sim_link(p_cfg, kind, idx, jdx);
        }
    }
}

void sim_topology_trail (sim_cfg_t* p_cfg, int kind)
{
    sim_topology_chain(p_cfg, kind);
    for (uint8_t idx = 1; idx + 2 <= p_cfg->n_nodes; idx += 2)
    {
        sim_link(p_cfg, kind, idx, (uint8_t)(idx + 2));
    }
}

void sim_init (const sim_cfg_t* p_cfg)
{
    memset(&world, 0, sizeof(world));
    world.cfg = *p_cfg;
    world.rng =
        0x9E3779B97F4A7C15ULL ^ ((uint64_t)p_cfg->seed * 0xBF58476D1CE4E5B9ULL);
    if (!world.rng)
    {
        world.rng = 1;
    }
    for (int idx = 0; idx < 4; idx++)
    {
        sim_rand();
    }
    for (uint8_t node = 1; node <= p_cfg->n_nodes; node++)
    {
        for (int kdx = 0; kdx < SIM_KINDS; kdx++)
        {
            world.radios[node][kdx].pkt_id = (uint16_t)sim_rand();
            /* first heartbeat shortly after boot */
            world.radios[node][kdx].next_hb = 100u + node;
        }
    }
    world.cur = 1;
}

void sim_set_link (int kind, uint8_t node_a, uint8_t node_b, bool b_link_up)
{
    world.cfg.adj[kind][node_a][node_b] = world.cfg.adj[kind][node_b][node_a] =
        b_link_up;
}

void sim_set_link_loss (int kind, float32_t loss)
{
    world.cfg.kind[kind].link_loss = loss;
}

void sim_set_corrupt_rate (int kind, float32_t rate)
{
    world.cfg.kind[kind].corrupt_rate = rate;
}

void sim_set_current (uint8_t node_id)
{
    world.cur = node_id;
}

uint32_t sim_now (void)
{
    return world.now;
}

/* ---- airtime
 * -----------------------------------------------------------------------------
 */

float64_t sim_lora_airtime_ms (uint16_t len)
{
    const int       spread = 7, coding_rate = 1, implicit_hdr = 0, low_rate = 0,
                    crc = 1, npre = 8;
    const float64_t bandwidth = 125000.0;
    float64_t       time_spec = (float64_t)(1 << spread) / bandwidth * 1000.0;
    float64_t       tpre      = (npre + 4.25) * time_spec;
    float64_t       num =
        8.0 * len - 4.0 * spread + 28 + 16 * crc - 20 * implicit_hdr;
    float64_t den = 4.0 * (spread - 2 * low_rate);
    float64_t sym = ceil(num / den) * (coding_rate + 4);
    if (sym < 0)
    {
        sym = 0;
    }
    return tpre + (8 + sym) * time_spec;
}

static void airtime (int kind, uint16_t frame_len, uint32_t* p_uart_ms,
                     uint32_t* p_air_ms)
{
    if (SIM_LORA == kind)
    {
        *p_uart_ms = 0;
        *p_air_ms  = (uint32_t)ceil(sim_lora_airtime_ms(frame_len));
    }
    else
    {
        /* Pico -> ESP8266: [0xAA][len][payload][crc8] at 10 bits per byte. */
        float64_t uniform = (frame_len + 3.0) * 10.0 /
                            (float64_t)world.cfg.kind[kind].uart_baud * 1000.0;
        *p_uart_ms        = (uint32_t)ceil(uniform);
        /* ESP-NOW at 1 Mbit/s plus preamble/MAC overhead (~0.5 ms). */
        *p_air_ms = (uint32_t)ceil(frame_len * 8.0 / 1000.0 + 0.5);
    }
}

/* ---- queues
 * -----------------------------------------------------------------------------
 */

static bool txq_push (radio_t* p_radio, const frame_t* p_frame)
{
    if (p_radio->txq_n >= TXQ_LEN)
    {
        return false;
    }
    p_radio->txq[(p_radio->txq_head + p_radio->txq_n) % TXQ_LEN] = *p_frame;
    p_radio->txq_n++;
    return true;
}

static bool seen_check_add (radio_t* p_radio, uint8_t src, uint16_t msg_id)
{
    for (int idx = 0; idx < SEEN_LEN; idx++)
    {
        seen_t* p_entry = &p_radio->seen[idx];
        if ((p_entry->b_used) && (p_entry->src == src) &&
            (p_entry->pkt_id == msg_id) &&
            (world.now - p_entry->now_ms <= SEEN_WINDOW_MS))
        {
            return true;
        }
    }
    seen_t* p_entry    = &p_radio->seen[p_radio->seen_next];
    p_radio->seen_next = (uint8_t)((p_radio->seen_next + 1) % SEEN_LEN);
    p_entry->b_used    = true;
    p_entry->src       = src;
    p_entry->pkt_id    = msg_id;
    p_entry->now_ms    = world.now;
    return false;
}

/* A frame arrived intact at node n. Mesh rules from the contract. */
static void on_frame (uint8_t node, int kind, const frame_t* p_frame)
{
    radio_t* p_radio = &world.radios[node][kind];
    uint8_t  src     = p_frame->buf[0];
    uint8_t  dst     = p_frame->buf[1];
    uint8_t  hops    = p_frame->buf[4];
    uint16_t msg_id  = (uint16_t)(p_frame->buf[2] | (p_frame->buf[3] << 8));
    p_radio->stats.rx_count++;
    if ((src == node) || (seen_check_add(p_radio, src, msg_id)))
    {
        p_radio->stats.dup_dropped++;
        return;
    }
    if (src <= SIM_MAX_NODES)
    {
        p_radio->heard[src]    = true;
        p_radio->heard_ms[src] = world.now;
    }
    if ((dst == node) || (MESH_BROADCAST == dst))
    {
        if (p_radio->rxq_n >= RXQ_LEN)
        {
            p_radio->stats.rx_overflow++;
        }
        else
        {
            rxitem_t* p_item =
                &p_radio->rxq[(p_radio->rxq_head + p_radio->rxq_n) % RXQ_LEN];
            p_item->src = src;
            p_item->len = (uint16_t)(p_frame->len - 5);
            memcpy(p_item->payload, p_frame->buf + 5, p_item->len);
            if ((world.cfg.kind[kind].corrupt_rate > 0) &&
                (sim_frand() < world.cfg.kind[kind].corrupt_rate) &&
                (p_item->len))
            {
                uint32_t bit = sim_rand() % ((uint32_t)p_item->len * 8u);
                p_item->payload[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            }
            p_radio->rxq_n++;
            p_radio->stats.delivered++;
        }
    }
    if (dst != node)
    {
        if (0 == hops)
        {
            p_radio->stats.hop_dropped++;
            return;
        }
        for (int idx = 0; idx < RELAYQ_LEN; idx++)
        {
            if (!p_radio->relayq[idx].b_used)
            {
                relay_t* p_queue                 = &p_radio->relayq[idx];
                p_queue->b_used                  = true;
                p_queue->frame                   = *p_frame;
                p_queue->frame.buf[4]            = (uint8_t)(hops - 1);
                const sim_kind_cfg_t* p_kind_cfg = &world.cfg.kind[kind];
                p_queue->ready_ms =
                    world.now + p_kind_cfg->relay_min_ms +
                    sim_rand() % (uint32_t)(p_kind_cfg->relay_max_ms -
                                            p_kind_cfg->relay_min_ms + 1);
                return;
            }
        }
        p_radio->stats.relay_overflow++;
    }
}

static bool overlaps (uint32_t start_a, uint32_t end_a, uint32_t start_b,
                      uint32_t end_b)
{
    return (start_a < end_b) && (start_b < end_a);
}

/* Transmission from node j finished: decide who heard it. */
static void finish_tx (uint8_t jdx, int kind)
{
    radio_t* p_radio_j = &world.radios[jdx][kind];
    uint32_t start_ms  = p_radio_j->air_start_ms;
    uint32_t end_ms    = p_radio_j->end_ms;
    for (uint8_t idx = 1; idx <= world.cfg.n_nodes; idx++)
    {
        if ((idx == jdx) || (!world.cfg.adj[kind][idx][jdx]))
        {
            continue;
        }
        bool b_lost = false;
        /* half duplex + collisions: any overlapping air time at i itself or
         * from another neighbour of i */
        for (int kdx = 0; (kdx < LOG_LEN) && (!b_lost); kdx++)
        {
            const txlog_t* p_log_entry = &world.log[kind][kdx];
            if ((0 == p_log_entry->end) || (p_log_entry->node == jdx))
            {
                continue;
            }
            if (((p_log_entry->node == idx) ||
                 (world.cfg.adj[kind][idx][p_log_entry->node])) &&
                (overlaps(start_ms, end_ms, p_log_entry->air_start,
                          p_log_entry->end)))
            {
                b_lost = true;
            }
        }
        /* i was busy pushing its own frame over UART/air even if not yet on
         * air */
        radio_t* p_radio_i = &world.radios[idx][kind];
        if ((!b_lost) && (p_radio_i->b_enable) &&
            (overlaps(start_ms, end_ms, p_radio_i->air_start_ms,
                      p_radio_i->end_ms)))
        {
            b_lost = true;
        }
        if (b_lost)
        {
            p_radio_i->stats.rx_error++;
            continue;
        }
        if (sim_frand() < world.cfg.kind[kind].link_loss)
        {
            continue;
        }
        if ((p_radio_i->drop_rate > 0) && (sim_frand() < p_radio_i->drop_rate))
        {
            p_radio_i->stats.drop_injected++;
            continue;
        }
        on_frame(idx, kind, &p_radio_j->cur);
    }
}

static bool channel_busy_near (uint8_t node, int kind)
{
    for (uint8_t kdx = 1; kdx <= world.cfg.n_nodes; kdx++)
    {
        if ((kdx != node) && (world.cfg.adj[kind][node][kdx]))
        {
            radio_t* p_radio = &world.radios[kdx][kind];
            if ((p_radio->b_enable) && (world.now >= p_radio->air_start_ms) &&
                (world.now < p_radio->end_ms))
            {
                return true;
            }
        }
    }
    return false;
}

static void start_tx (uint8_t node, int kind, const frame_t* p_frame,
                      bool b_own)
{
    radio_t* p_radio = &world.radios[node][kind];
    uint32_t uart_ms;
    uint32_t on_air_ms;
    airtime(kind, p_frame->len, &uart_ms, &on_air_ms);
    p_radio->b_enable      = true;
    p_radio->cur           = *p_frame;
    p_radio->start_ms      = world.now;
    p_radio->air_start_ms  = world.now + uart_ms;
    p_radio->end_ms        = world.now + uart_ms + on_air_ms;
    txlog_t* p_log_entry   = &world.log[kind][world.log_next[kind]];
    world.log_next[kind]   = (uint16_t)((world.log_next[kind] + 1) % LOG_LEN);
    p_log_entry->node      = node;
    p_log_entry->air_start = p_radio->air_start_ms;
    p_log_entry->end       = p_radio->end_ms;
    world.air_used[kind] += on_air_ms;
    if (b_own)
    {
        p_radio->stats.tx_count++;
    }
    else
    {
        p_radio->stats.relayed++;
    }
}

/* Mesh poll for node n (what lora_mesh.c / wifi_mesh.c poll() does). */
static void mesh_poll (uint8_t node, int kind)
{
    radio_t* p_radio = &world.radios[node][kind];
    if ((world.cfg.heartbeat_ms) && (node == world.cfg.bridge_id) &&
        ((int32_t)(world.now - p_radio->next_hb) >= 0))
    {
        frame_t heartbeat = {{node, 0xFE, (uint8_t)p_radio->pkt_id,
                              (uint8_t)(p_radio->pkt_id >> 8), HOP_LIMIT, 'h'},
                             6};
        p_radio->pkt_id++;
        txq_push(p_radio, &heartbeat);
        p_radio->next_hb = world.now + world.cfg.heartbeat_ms;
    }
    if ((p_radio->b_enable) || (world.now < p_radio->backoff_until))
    {
        return;
    }
    /* relays first (keeps the flood moving), then our own queue */
    int pick = -1;
    for (int idx = 0; idx < RELAYQ_LEN; idx++)
    {
        if ((p_radio->relayq[idx].b_used) &&
            ((int32_t)(world.now - p_radio->relayq[idx].ready_ms) >= 0))
        {
            if ((pick < 0) || ((int32_t)(p_radio->relayq[idx].ready_ms -
                                         p_radio->relayq[pick].ready_ms) < 0))
            {
                pick = idx;
            }
        }
    }
    const frame_t* p_frame = NULL;
    bool           b_own   = false;
    if (pick >= 0)
    {
        p_frame = &p_radio->relayq[pick].frame;
    }
    else if (p_radio->txq_n)
    {
        p_frame = &p_radio->txq[p_radio->txq_head];
        b_own   = true;
    }
    else
    {
        /* nothing to relay and nothing of our own queued: f stays NULL */
    }
    if (!p_frame)
    {
        return;
    }
    if ((world.cfg.kind[kind].b_carrier_sense) &&
        (channel_busy_near(node, kind)))
    {
        p_radio->backoff_until = world.now + 1 + sim_rand() % 3;
        return;
    }
    start_tx(node, kind, p_frame, b_own);
    if (b_own)
    {
        p_radio->txq_head = (uint8_t)((p_radio->txq_head + 1) % TXQ_LEN);
        p_radio->txq_n--;
    }
    else
    {
        p_radio->relayq[pick].b_used = false;
    }
}

void sim_tick (void)
{
    world.now++;
    for (uint8_t node = 1; node <= world.cfg.n_nodes; node++)
    {
        for (int kdx = 0; kdx < SIM_KINDS; kdx++)
        {
            radio_t* p_radio = &world.radios[node][kdx];
            if ((p_radio->b_enable) && (world.now >= p_radio->end_ms))
            {
                p_radio->b_enable = false;
                finish_tx(node, kdx);
            }
        }
    }
}

/* ---- mesh_radio_t for the current node
 * ------------------------------------------------- */

static bool m_send (int kind, uint8_t dst, const uint8_t* p_buf, uint16_t len)
{
    radio_t* p_radio = &world.radios[world.cur][kind];
    if ((0 == len) || (len > world.cfg.kind[kind].max_payload))
    {
        return false;
    }
    frame_t frame;
    frame.buf[0] = world.cur;
    frame.buf[1] = dst;
    frame.buf[2] = (uint8_t)p_radio->pkt_id;
    frame.buf[3] = (uint8_t)(p_radio->pkt_id >> 8);
    frame.buf[4] = HOP_LIMIT;
    memcpy(frame.buf + 5, p_buf, len);
    frame.len = (uint16_t)(len + 5);
    if (!txq_push(p_radio, &frame))
    {
        return false;
    }
    p_radio->pkt_id++;
    return true;
}

static int m_recv (int kind, uint8_t* p_src, uint8_t* p_buf, uint16_t cap)
{
    radio_t* p_radio = &world.radios[world.cur][kind];
    if (!p_radio->rxq_n)
    {
        return MESH_RECV_NONE;
    }
    rxitem_t* p_item  = &p_radio->rxq[p_radio->rxq_head];
    p_radio->rxq_head = (uint8_t)((p_radio->rxq_head + 1) % RXQ_LEN);
    p_radio->rxq_n--;
    if (p_item->len > cap)
    {
        return MESH_RECV_BUF_TOO_SMALL;
    }
    *p_src = p_item->src;
    memcpy(p_buf, p_item->payload, p_item->len);
    return p_item->len;
}

static bool m_last_heard (int kind, uint8_t node, uint32_t* p_msec)
{
    radio_t* p_radio = &world.radios[world.cur][kind];
    if ((node > SIM_MAX_NODES) || (!p_radio->heard[node]))
    {
        return false;
    }
    *p_msec = p_radio->heard_ms[node];
    return true;
}

static bool m_tx_busy (int kind)
{
    radio_t* p_radio = &world.radios[world.cur][kind];
    return (p_radio->txq_n > 0) || (p_radio->b_enable);
}

/* DEVIATION (BARR-C 6.3.b.iv, return inside a macro): DEFINE_RADIO() generates
 * the complete mesh_radio_t callback functions for one simulated radio (host
 * simulator only), so the returns are the generated functions' own and never
 * jump out of the code that uses the macro.
 * Approved by: Senthil Shreya (project manager), 2026-11-28 */
#define DEFINE_RADIO(NAME, KIND, STR)                                          \
    static bool NAME##_send(uint8_t dst, const uint8_t* p_buf, uint16_t len)   \
    {                                                                          \
        return m_send(KIND, dst, p_buf, len);                                  \
    }                                                                          \
    static int NAME##_recv(uint8_t* p_src, uint8_t* p_buf, uint16_t cap)       \
    {                                                                          \
        return m_recv(KIND, p_src, p_buf, cap);                                \
    }                                                                          \
    static void NAME##_poll(void)                                              \
    {                                                                          \
        mesh_poll(world.cur, KIND);                                            \
    }                                                                          \
    static uint16_t NAME##_maxp(void)                                          \
    {                                                                          \
        return world.cfg.kind[KIND].max_payload;                               \
    }                                                                          \
    static bool NAME##_heard(uint8_t node, uint32_t* p_msec)                   \
    {                                                                          \
        return m_last_heard(KIND, node, p_msec);                               \
    }                                                                          \
    static bool NAME##_busy(void)                                              \
    {                                                                          \
        return m_tx_busy(KIND);                                                \
    }                                                                          \
    static void NAME##_stats(mesh_stats_t* p_out)                              \
    {                                                                          \
        *p_out = world.radios[world.cur][KIND].stats;                          \
    }                                                                          \
    static void NAME##_drop(float32_t rate)                                    \
    {                                                                          \
        world.radios[world.cur][KIND].drop_rate = rate;                        \
    }                                                                          \
    const mesh_radio_t g_sim_##NAME = {                                        \
        STR,          NAME##_send, NAME##_recv,  NAME##_poll, NAME##_maxp,     \
        NAME##_heard, NAME##_busy, NAME##_stats, NAME##_drop};

DEFINE_RADIO(lora, SIM_LORA, "lora")
DEFINE_RADIO(wifi, SIM_WIFI, "wifi")

void sim_get_stats (uint8_t node_id, int kind, mesh_stats_t* p_out)
{
    *p_out = world.radios[node_id][kind].stats;
}

void sim_set_drop_rate (uint8_t node_id, int kind, float32_t rate)
{
    world.radios[node_id][kind].drop_rate = rate;
}

uint32_t sim_airtime_used (int kind)
{
    return world.air_used[kind];
}

/*** end of file ***/

