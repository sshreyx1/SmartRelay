/*
 * wifi_mesh.c - WiFi flooding mesh (implements mesh_radio_t).
 *
 * How flooding works here:
 *   Sending:   give the packet a new ID, remember the ID, broadcast it.
 *   Receiving: 1. ignore it if it came from a WST that isn't our neighbour
 *              2. maybe drop it on purpose (drop_rate, for the demo)
 *              3. ignore it if we've already seen (src, pkt_id) in 30 s
 *              4. note that we heard from src            (last_heard)
 *              5. if it's for us or for everyone, hand it up (recv)
 *              6. if it's not only for us, rebroadcast it after a random
 *                 5-30 ms wait, with hops - 1  (unless hops is already 0)
 *   Node 1 (the bridge) also sends a tiny heartbeat every 3 s so every node
 *   can tell whether the bridge is reachable over WiFi.
 *
 * Nothing here blocks: send() only queues, poll() does the work.
 */
#include "wifi_mesh.h"
#include "wifi_mesh_config.h"
#include "wst_link.h"

#include <string.h>

/* ---------- Time and random numbers ---------- */
#ifdef WIFI_MESH_HOST_TEST
uint32_t wm_now_ms(void);   /* provided by the PC simulation */
uint32_t wm_rand(void);
#else
#include "pico/stdlib.h"
#include "pico/rand.h"
static uint32_t wm_now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
static uint32_t wm_rand(void)   { return get_rand_32(); }
#endif

/* ---------- Packet header ---------- */
#define H_SRC   0
#define H_DST   1
#define H_ID_LO 2
#define H_ID_HI 3
#define H_HOPS  4

typedef struct { uint8_t src; uint16_t msg_id; uint32_t seen_ms; } dup_entry_t;

typedef struct {
    uint32_t due;                   /* relays: when to transmit        */
    uint8_t  len;                   /* whole packet incl. header        */
    uint8_t  bytes[WST_MAX_PKT];
} out_pkt_t;

typedef struct {
    uint8_t src;
    uint8_t len;                    /* payload only                     */
    uint8_t hops;                   /* links travelled, 1 = direct      */
    uint8_t prev;                   /* node we heard it from, 0 = ?     */
    uint8_t data[WIFI_MESH_MAX_PAYLOAD];
} in_pkt_t;

typedef struct {
    bool     b_link_up;
    uint8_t  self_id;
    uint16_t next_id;
    bool     b_filter_on;
    float    drop_rate;
    uint32_t next_heartbeat;

    dup_entry_t dup[WIFI_MESH_DUP_CACHE];
    uint16_t    dup_next;

    bool     heard[6];
    uint32_t heard_ms[6];

    out_pkt_t sendq[WIFI_MESH_SEND_QUEUE];     /* our own packets, FIFO */
    uint8_t   sq_head;
    uint8_t sq_count;
    out_pkt_t relayq[WIFI_MESH_RELAY_QUEUE];   /* others' packets       */
    uint8_t   rq_count;
    in_pkt_t  recvq[WIFI_MESH_RECV_QUEUE];     /* waiting for recv()    */
    uint8_t   rcv_head;
    uint8_t rcv_count;

    mesh_stats_t stats;
    uint32_t filtered;
    uint32_t malformed;

    /* route of the packet recv() returned last (wifi_mesh_last_recv_info) */
    bool     b_last_valid;
    uint8_t  last_hops;
    uint8_t last_prev;
    uint16_t relayed_from[6];   /* relays queued, per original source */
} wm_state_t;

static wm_state_t wm_default;
static wm_state_t *p_state = &wm_default;

#ifdef WIFI_MESH_HOST_TEST
/* The PC simulation runs 5 nodes in one program and swaps state. */
unsigned wifi_mesh_test_state_size(void) { return sizeof(wm_state_t); }
void     wifi_mesh_test_use(void *state) { S = (wm_state_t *)state; }
#endif

/* ---------- Duplicate cache ---------- */
static bool dup_seen(uint8_t src, uint16_t msg_id, uint32_t now) {
    for (int idx = 0; idx < WIFI_MESH_DUP_CACHE; idx++) {
        const dup_entry_t *p_entry = &p_state->dup[idx];
        if ((p_entry->src == src) && (p_entry->msg_id == msg_id) && (now - p_entry->seen_ms <= WIFI_MESH_DUP_WINDOW_MS))
            return true;
    }
    return false;
}

static void dup_add(uint8_t src, uint16_t msg_id, uint32_t now) {
    dup_entry_t *p_entry = &p_state->dup[p_state->dup_next];
    p_entry->src = src; p_entry->msg_id = msg_id; p_entry->seen_ms = now;
    p_state->dup_next = (p_state->dup_next + 1) % WIFI_MESH_DUP_CACHE;
}

/* ---------- Helpers ---------- */
static int node_from_mac(const uint8_t mac[6]) {
    for (int node = 1; node <= 5; node++)
        if (0 == memcmp(wifi_mesh_macs[node], mac, 6)) return node;
    return 0;
}

static bool queue_own(uint8_t dst, const uint8_t *p_payload, uint16_t len) {
    if (WIFI_MESH_SEND_QUEUE == p_state->sq_count) return false;
    uint32_t now = wm_now_ms();
    out_pkt_t *p_slot = &p_state->sendq[(p_state->sq_head + p_state->sq_count) % WIFI_MESH_SEND_QUEUE];
    uint16_t msg_id = p_state->next_id++;
    p_slot->bytes[H_SRC]   = p_state->self_id;
    p_slot->bytes[H_DST]   = dst;
    p_slot->bytes[H_ID_LO] = (uint8_t)(msg_id & 0xFF);
    p_slot->bytes[H_ID_HI] = (uint8_t)(msg_id >> 8);
    p_slot->bytes[H_HOPS]  = WIFI_MESH_HOP_LIMIT;
    if (len) memcpy(&p_slot->bytes[MESH_HEADER_LEN], p_payload, len);
    p_slot->len = (uint8_t)(MESH_HEADER_LEN + len);
    p_slot->due = now;
    p_state->sq_count++;
    dup_add(p_state->self_id, msg_id, now);          /* so our own echo is never relayed */
    return true;
}

static void handle_packet(const wst_rx_pkt_t *p_rx_pkt) {
    uint32_t now = wm_now_ms();

    /* 1. Sanity check */
    if (p_rx_pkt->len < MESH_HEADER_LEN) { p_state->malformed++; return; }
    uint8_t  src  = p_rx_pkt->data[H_SRC];
    uint8_t  dst  = p_rx_pkt->data[H_DST];
    uint16_t msg_id   = (uint16_t)(p_rx_pkt->data[H_ID_LO] | (p_rx_pkt->data[H_ID_HI] << 8));
    uint8_t  hops = p_rx_pkt->data[H_HOPS];
    bool b_dst_ok = ((dst >= 1) && (dst <= 5)) || (MESH_BROADCAST == dst) || (MESH_INTERNAL == dst);
    if ((src < 1) || (src > 5) || (!b_dst_ok)) { p_state->malformed++; return; }

    /* 2. Topology filter: only listen to neighbours */
    if (p_state->b_filter_on) {
        int from = node_from_mac(p_rx_pkt->mac);
        if ((0 == from) || (!(wifi_mesh_neighbours[p_state->self_id] & (1u << from)))) {
            p_state->filtered++;
            return;
        }
    }

    /* 3. Simulated packet loss (demo) */
    if ((p_state->drop_rate > 0.0f) &&
        ((float)(wm_rand() % 10000) < p_state->drop_rate * 10000.0f)) {
        p_state->stats.drop_injected++;
        return;
    }

    p_state->stats.rx_count++;

    /* 4. Already seen it (or it's our own echo)? */
    if ((src == p_state->self_id) || (dup_seen(src, msg_id, now))) { p_state->stats.dup_dropped++; return; }
    dup_add(src, msg_id, now);

    /* 5. We heard from src */
    p_state->heard[src] = true;
    p_state->heard_ms[src] = now;

    /* 6. Deliver if it's for us or for everyone */
    uint8_t plen = (uint8_t)(p_rx_pkt->len - MESH_HEADER_LEN);
    if ((dst == p_state->self_id) || (MESH_BROADCAST == dst)) {
        if (WIFI_MESH_RECV_QUEUE == p_state->rcv_count) {
            p_state->stats.rx_overflow++;
        } else {
            in_pkt_t *p_queue = &p_state->recvq[(p_state->rcv_head + p_state->rcv_count) % WIFI_MESH_RECV_QUEUE];
            p_queue->src = src;
            p_queue->len = plen;
            p_queue->hops = hops <= WIFI_MESH_HOP_LIMIT          /* 0 = out of range */
                    ? (uint8_t)(WIFI_MESH_HOP_LIMIT - hops + 1) : 0;
            p_queue->prev = (uint8_t)node_from_mac(p_rx_pkt->mac);
            memcpy(p_queue->data, &p_rx_pkt->data[MESH_HEADER_LEN], plen);
            p_state->rcv_count++;
            p_state->stats.delivered++;
        }
    }

    /* 7. Relay unless it was only for us */
    if (dst == p_state->self_id) return;
    if (0 == hops) { p_state->stats.hop_dropped++; return; }
    if (WIFI_MESH_RELAY_QUEUE == p_state->rq_count) { p_state->stats.relay_overflow++; return; }
    out_pkt_t *p_slot = &p_state->relayq[p_state->rq_count++];
    memcpy(p_slot->bytes, p_rx_pkt->data, p_rx_pkt->len);
    p_slot->bytes[H_HOPS] = hops - 1;
    p_slot->len = p_rx_pkt->len;
    p_state->relayed_from[src]++;
    uint32_t span = WIFI_MESH_RELAY_DELAY_MAX - WIFI_MESH_RELAY_DELAY_MIN + 1;
    p_slot->due = now + WIFI_MESH_RELAY_DELAY_MIN + wm_rand() % span;
}

/* Hand the next packet to the WST, if the WST is free. */
static void transmit_next(void) {
    if (wst_link_tx_busy()) return;
    uint32_t now = wm_now_ms();

    /* Relays first (oldest due time), so the mesh keeps flowing */
    int best = -1;
    for (int idx = 0; idx < p_state->rq_count; idx++)
        if (((int32_t)(now - p_state->relayq[idx].due) >= 0) &&
            ((best < 0) || ((int32_t)(p_state->relayq[idx].due - p_state->relayq[best].due) < 0)))
            best = idx;
    if (best >= 0) {
        wst_link_send(p_state->relayq[best].bytes, p_state->relayq[best].len);
        p_state->stats.relayed++;
        p_state->relayq[best] = p_state->relayq[--p_state->rq_count];   /* remove (order doesn't matter) */
        return;
    }

    if (p_state->sq_count) {
        out_pkt_t *p_slot = &p_state->sendq[p_state->sq_head];
        wst_link_send(p_slot->bytes, p_slot->len);
        if (MESH_INTERNAL != p_slot->bytes[H_DST]) p_state->stats.tx_count++;
        p_state->sq_head = (p_state->sq_head + 1) % WIFI_MESH_SEND_QUEUE;
        p_state->sq_count--;
    }
}

/* ---------- mesh_radio_t functions ---------- */
static bool wm_send(uint8_t dst, const uint8_t *p_payload, uint16_t len) {
    if (!p_state->b_link_up) return false;
    if ((0 == len) || (len > WIFI_MESH_MAX_PAYLOAD) || (NULL == p_payload)) return false;
    if ((!(((dst >= 1) && (dst <= 5)) || (MESH_BROADCAST == dst))) || (dst == p_state->self_id)) return false;
    return queue_own(dst, p_payload, len);
}

static int wm_recv(uint8_t *p_src, uint8_t *p_buf, uint16_t buf_size) {
    if (0 == p_state->rcv_count) return MESH_RECV_NONE;
    in_pkt_t *p_queue = &p_state->recvq[p_state->rcv_head];
    p_state->rcv_head = (p_state->rcv_head + 1) % WIFI_MESH_RECV_QUEUE;
    p_state->rcv_count--;
    if (buf_size < p_queue->len) return MESH_RECV_BUF_TOO_SMALL;
    p_state->b_last_valid = true;
    p_state->last_hops  = p_queue->hops;
    p_state->last_prev  = p_queue->prev;
    if (p_src) *p_src = p_queue->src;
    memcpy(p_buf, p_queue->data, p_queue->len);
    return p_queue->len;
}

static void wm_poll(void) {
    if (!p_state->b_link_up) return;
    wst_link_poll();

    wst_rx_pkt_t rx_pkt;
    while (wst_link_recv(&rx_pkt)) handle_packet(&rx_pkt);

    uint32_t now = wm_now_ms();
    if ((WIFI_MESH_BRIDGE_NODE == p_state->self_id) && ((int32_t)(now - p_state->next_heartbeat) >= 0)) {
        queue_own(MESH_INTERNAL, NULL, 0);     /* if queue is full, skip this beat */
        p_state->next_heartbeat = now + WIFI_MESH_HEARTBEAT_MS;
    }

    transmit_next();
    wst_link_poll();                            /* push the new bytes out now */
}

static uint16_t wm_max_payload(void) { return WIFI_MESH_MAX_PAYLOAD; }

static bool wm_last_heard(uint8_t node_id, uint32_t *p_msec) {
    if ((node_id < 1) || (node_id > 5) || (!p_state->heard[node_id])) return false;
    if (p_msec) *p_msec = p_state->heard_ms[node_id];
    return true;
}

static bool wm_tx_busy(void) {
    return (p_state->sq_count > 0) || (p_state->rq_count > 0) || (wst_link_tx_busy());
}

static void wm_get_stats(mesh_stats_t *p_out) {
    wst_link_stats_t link_stats;
    wst_link_get_stats(&link_stats);
    *p_out = p_state->stats;
    p_out->tx_fail  = link_stats.tx_fail + link_stats.tx_timeout;
    p_out->rx_error = p_state->malformed + link_stats.rx_crc_err + link_stats.rx_queue_full + link_stats.uart_overflow;
}

static void wm_set_drop_rate(float rate) {
    if (rate < 0.0f) rate = 0.0f;
    if (rate > 1.0f) rate = 1.0f;
    p_state->drop_rate = rate;
}

const mesh_radio_t g_wifi_mesh = {
    .p_name          = "wifi",
    .p_send          = wm_send,
    .p_recv          = wm_recv,
    .p_poll          = wm_poll,
    .p_max_payload   = wm_max_payload,
    .p_last_heard    = wm_last_heard,
    .p_tx_busy       = wm_tx_busy,
    .p_get_stats     = wm_get_stats,
    .p_set_drop_rate = wm_set_drop_rate,
};

/* ---------- Extra functions ---------- */
bool wifi_mesh_init(uint8_t node_id) {
    memset(p_state, 0, sizeof *p_state);
    p_state->b_filter_on = true;
    p_state->next_id = (uint16_t)wm_rand();    /* random start: a reboot won't reuse
                                            IDs other nodes still remember */
    wst_link_init();

    if (0 == node_id) {                  /* work it out from our WST's MAC */
        uint8_t mac[6];
        uint32_t start = wm_now_ms();
        while (!wst_link_get_mac(mac)) {
            wst_link_poll();
            if (wm_now_ms() - start > 2000) return false;
        }
        node_id = (uint8_t)node_from_mac(mac);
    }
    if ((node_id < 1) || (node_id > 5)) return false;

    p_state->self_id = node_id;
    p_state->next_heartbeat = wm_now_ms();
    p_state->b_link_up = true;
    return true;
}

uint8_t  wifi_mesh_node_id(void)               { return p_state->b_link_up ? p_state->self_id : 0; }
void     wifi_mesh_set_topology_filter(bool b_enable) { p_state->b_filter_on = b_enable; }
uint32_t wifi_mesh_filtered_count(void)         { return p_state->filtered; }

bool wifi_mesh_last_recv_info(uint8_t *p_hops, uint8_t *p_prev_hop) {
    if (p_hops)     *p_hops     = p_state->b_last_valid ? p_state->last_hops : 0;
    if (p_prev_hop) *p_prev_hop = p_state->b_last_valid ? p_state->last_prev : 0;
    return p_state->b_last_valid;
}

uint16_t wifi_mesh_relayed_from(uint8_t src) {
    return src <= 5 ? p_state->relayed_from[src] : 0;
}
