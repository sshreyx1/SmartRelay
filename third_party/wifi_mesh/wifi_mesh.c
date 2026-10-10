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

typedef struct { uint8_t src; uint16_t id; uint32_t t; } dup_entry_t;

typedef struct {
    uint32_t due;                   /* relays: when to transmit        */
    uint8_t  len;                   /* whole packet incl. header        */
    uint8_t  bytes[WST_MAX_PKT];
} out_pkt_t;

typedef struct {
    uint8_t src;
    uint8_t len;                    /* payload only                     */
    uint8_t data[WIFI_MESH_MAX_PAYLOAD];
} in_pkt_t;

typedef struct {
    bool     up;
    uint8_t  me;
    uint16_t next_id;
    bool     filter_on;
    float    drop_rate;
    uint32_t next_heartbeat;

    dup_entry_t dup[WIFI_MESH_DUP_CACHE];
    uint16_t    dup_next;

    bool     heard[6];
    uint32_t heard_ms[6];

    out_pkt_t sendq[WIFI_MESH_SEND_QUEUE];     /* our own packets, FIFO */
    uint8_t   sq_head, sq_count;
    out_pkt_t relayq[WIFI_MESH_RELAY_QUEUE];   /* others' packets       */
    uint8_t   rq_count;
    in_pkt_t  recvq[WIFI_MESH_RECV_QUEUE];     /* waiting for recv()    */
    uint8_t   rcv_head, rcv_count;

    mesh_stats_t st;
    uint32_t filtered;
    uint32_t malformed;
} wm_state_t;

static wm_state_t wm_default;
static wm_state_t *S = &wm_default;

#ifdef WIFI_MESH_HOST_TEST
/* The PC simulation runs 5 nodes in one program and swaps state. */
unsigned wifi_mesh_test_state_size(void) { return sizeof(wm_state_t); }
void     wifi_mesh_test_use(void *state) { S = (wm_state_t *)state; }
#endif

/* ---------- Duplicate cache ---------- */
static bool dup_seen(uint8_t src, uint16_t id, uint32_t now) {
    for (int i = 0; i < WIFI_MESH_DUP_CACHE; i++) {
        const dup_entry_t *e = &S->dup[i];
        if (e->src == src && e->id == id && now - e->t <= WIFI_MESH_DUP_WINDOW_MS)
            return true;
    }
    return false;
}

static void dup_add(uint8_t src, uint16_t id, uint32_t now) {
    dup_entry_t *e = &S->dup[S->dup_next];
    e->src = src; e->id = id; e->t = now;
    S->dup_next = (S->dup_next + 1) % WIFI_MESH_DUP_CACHE;
}

/* ---------- Helpers ---------- */
static int node_from_mac(const uint8_t mac[6]) {
    for (int n = 1; n <= 5; n++)
        if (memcmp(WIFI_MESH_MACS[n], mac, 6) == 0) return n;
    return 0;
}

static bool queue_own(uint8_t dst, const uint8_t *payload, uint16_t len) {
    if (S->sq_count == WIFI_MESH_SEND_QUEUE) return false;
    uint32_t now = wm_now_ms();
    out_pkt_t *p = &S->sendq[(S->sq_head + S->sq_count) % WIFI_MESH_SEND_QUEUE];
    uint16_t id = S->next_id++;
    p->bytes[H_SRC]   = S->me;
    p->bytes[H_DST]   = dst;
    p->bytes[H_ID_LO] = (uint8_t)(id & 0xFF);
    p->bytes[H_ID_HI] = (uint8_t)(id >> 8);
    p->bytes[H_HOPS]  = WIFI_MESH_HOP_LIMIT;
    if (len) memcpy(&p->bytes[MESH_HEADER_LEN], payload, len);
    p->len = (uint8_t)(MESH_HEADER_LEN + len);
    p->due = now;
    S->sq_count++;
    dup_add(S->me, id, now);          /* so our own echo is never relayed */
    return true;
}

static void handle_packet(const wst_rx_pkt_t *rx) {
    uint32_t now = wm_now_ms();

    /* 1. Sanity check */
    if (rx->len < MESH_HEADER_LEN) { S->malformed++; return; }
    uint8_t  src  = rx->data[H_SRC];
    uint8_t  dst  = rx->data[H_DST];
    uint16_t id   = (uint16_t)(rx->data[H_ID_LO] | (rx->data[H_ID_HI] << 8));
    uint8_t  hops = rx->data[H_HOPS];
    bool dst_ok = (dst >= 1 && dst <= 5) || dst == MESH_BROADCAST || dst == MESH_INTERNAL;
    if (src < 1 || src > 5 || !dst_ok) { S->malformed++; return; }

    /* 2. Topology filter: only listen to neighbours */
    if (S->filter_on) {
        int from = node_from_mac(rx->mac);
        if (from == 0 || !(WIFI_MESH_NEIGHBOURS[S->me] & (1u << from))) {
            S->filtered++;
            return;
        }
    }

    /* 3. Simulated packet loss (demo) */
    if (S->drop_rate > 0.0f &&
        (float)(wm_rand() % 10000) < S->drop_rate * 10000.0f) {
        S->st.drop_injected++;
        return;
    }

    S->st.rx++;

    /* 4. Already seen it (or it's our own echo)? */
    if (src == S->me || dup_seen(src, id, now)) { S->st.dup_dropped++; return; }
    dup_add(src, id, now);

    /* 5. We heard from src */
    S->heard[src] = true;
    S->heard_ms[src] = now;

    /* 6. Deliver if it's for us or for everyone */
    uint8_t plen = (uint8_t)(rx->len - MESH_HEADER_LEN);
    if (dst == S->me || dst == MESH_BROADCAST) {
        if (S->rcv_count == WIFI_MESH_RECV_QUEUE) {
            S->st.rx_overflow++;
        } else {
            in_pkt_t *q = &S->recvq[(S->rcv_head + S->rcv_count) % WIFI_MESH_RECV_QUEUE];
            q->src = src;
            q->len = plen;
            memcpy(q->data, &rx->data[MESH_HEADER_LEN], plen);
            S->rcv_count++;
            S->st.delivered++;
        }
    }

    /* 7. Relay unless it was only for us */
    if (dst == S->me) return;
    if (hops == 0) { S->st.hop_dropped++; return; }
    if (S->rq_count == WIFI_MESH_RELAY_QUEUE) { S->st.relay_overflow++; return; }
    out_pkt_t *r = &S->relayq[S->rq_count++];
    memcpy(r->bytes, rx->data, rx->len);
    r->bytes[H_HOPS] = hops - 1;
    r->len = rx->len;
    uint32_t span = WIFI_MESH_RELAY_DELAY_MAX - WIFI_MESH_RELAY_DELAY_MIN + 1;
    r->due = now + WIFI_MESH_RELAY_DELAY_MIN + wm_rand() % span;
}

/* Hand the next packet to the WST, if the WST is free. */
static void transmit_next(void) {
    if (wst_link_tx_busy()) return;
    uint32_t now = wm_now_ms();

    /* Relays first (oldest due time), so the mesh keeps flowing */
    int best = -1;
    for (int i = 0; i < S->rq_count; i++)
        if ((int32_t)(now - S->relayq[i].due) >= 0 &&
            (best < 0 || (int32_t)(S->relayq[i].due - S->relayq[best].due) < 0))
            best = i;
    if (best >= 0) {
        wst_link_send(S->relayq[best].bytes, S->relayq[best].len);
        S->st.relayed++;
        S->relayq[best] = S->relayq[--S->rq_count];   /* remove (order doesn't matter) */
        return;
    }

    if (S->sq_count) {
        out_pkt_t *p = &S->sendq[S->sq_head];
        wst_link_send(p->bytes, p->len);
        if (p->bytes[H_DST] != MESH_INTERNAL) S->st.tx++;
        S->sq_head = (S->sq_head + 1) % WIFI_MESH_SEND_QUEUE;
        S->sq_count--;
    }
}

/* ---------- mesh_radio_t functions ---------- */
static bool wm_send(uint8_t dst, const uint8_t *payload, uint16_t len) {
    if (!S->up) return false;
    if (len == 0 || len > WIFI_MESH_MAX_PAYLOAD || payload == NULL) return false;
    if (!((dst >= 1 && dst <= 5) || dst == MESH_BROADCAST) || dst == S->me) return false;
    return queue_own(dst, payload, len);
}

static int wm_recv(uint8_t *src, uint8_t *buf, uint16_t buf_size) {
    if (S->rcv_count == 0) return MESH_RECV_NONE;
    in_pkt_t *q = &S->recvq[S->rcv_head];
    S->rcv_head = (S->rcv_head + 1) % WIFI_MESH_RECV_QUEUE;
    S->rcv_count--;
    if (buf_size < q->len) return MESH_RECV_BUF_TOO_SMALL;
    if (src) *src = q->src;
    memcpy(buf, q->data, q->len);
    return q->len;
}

static void wm_poll(void) {
    if (!S->up) return;
    wst_link_poll();

    wst_rx_pkt_t rx;
    while (wst_link_recv(&rx)) handle_packet(&rx);

    uint32_t now = wm_now_ms();
    if (S->me == WIFI_MESH_BRIDGE_NODE && (int32_t)(now - S->next_heartbeat) >= 0) {
        queue_own(MESH_INTERNAL, NULL, 0);     /* if queue is full, skip this beat */
        S->next_heartbeat = now + WIFI_MESH_HEARTBEAT_MS;
    }

    transmit_next();
    wst_link_poll();                            /* push the new bytes out now */
}

static uint16_t wm_max_payload(void) { return WIFI_MESH_MAX_PAYLOAD; }

static bool wm_last_heard(uint8_t node_id, uint32_t *ms) {
    if (node_id < 1 || node_id > 5 || !S->heard[node_id]) return false;
    if (ms) *ms = S->heard_ms[node_id];
    return true;
}

static bool wm_tx_busy(void) {
    return S->sq_count > 0 || S->rq_count > 0 || wst_link_tx_busy();
}

static void wm_get_stats(mesh_stats_t *out) {
    wst_link_stats_t l;
    wst_link_get_stats(&l);
    *out = S->st;
    out->tx_fail  = l.tx_fail + l.tx_timeout;
    out->rx_error = S->malformed + l.rx_crc_err + l.rx_queue_full + l.uart_overflow;
}

static void wm_set_drop_rate(float rate) {
    if (rate < 0.0f) rate = 0.0f;
    if (rate > 1.0f) rate = 1.0f;
    S->drop_rate = rate;
}

const mesh_radio_t wifi_mesh = {
    .name          = "wifi",
    .send          = wm_send,
    .recv          = wm_recv,
    .poll          = wm_poll,
    .max_payload   = wm_max_payload,
    .last_heard    = wm_last_heard,
    .tx_busy       = wm_tx_busy,
    .get_stats     = wm_get_stats,
    .set_drop_rate = wm_set_drop_rate,
};

/* ---------- Extra functions ---------- */
bool wifi_mesh_init(uint8_t node_id) {
    memset(S, 0, sizeof *S);
    S->filter_on = true;
    S->next_id = (uint16_t)wm_rand();    /* random start: a reboot won't reuse
                                            IDs other nodes still remember */
    wst_link_init();

    if (node_id == 0) {                  /* work it out from our WST's MAC */
        uint8_t mac[6];
        uint32_t start = wm_now_ms();
        while (!wst_link_get_mac(mac)) {
            wst_link_poll();
            if (wm_now_ms() - start > 2000) return false;
        }
        node_id = (uint8_t)node_from_mac(mac);
    }
    if (node_id < 1 || node_id > 5) return false;

    S->me = node_id;
    S->next_heartbeat = wm_now_ms();
    S->up = true;
    return true;
}

uint8_t  wifi_mesh_node_id(void)               { return S->up ? S->me : 0; }
void     wifi_mesh_set_topology_filter(bool on) { S->filter_on = on; }
uint32_t wifi_mesh_filtered_count(void)         { return S->filtered; }
