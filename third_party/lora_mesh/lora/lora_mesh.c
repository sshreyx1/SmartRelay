/*
 * lora_mesh.c - Flooding mesh over LoRa (CSC09, Member 1).
 *
 * Packet on air: [src][dst][pkt_id hi][pkt_id lo][hops][payload...]
 *
 * Receive path:  CRC-checked by the radio -> optional drop_rate -> duplicate check on
 *                (src, pkt_id) -> deliver if dst is us or broadcast -> relay with
 *                hops-1 after a random delay if dst is not us and hops > 0.
 * Transmit path: poll() starts one transmission at a time; due relays go first.
 *
 * Best effort only: no ACKs or retries here, that is the fragmentation layer's job.
 */
#include <string.h>
#include "pico/stdlib.h"
#include "pico/rand.h"
#include "lora_radio.h"
#include "lora_mesh.h"

#define MAX_PAYLOAD ( LORA_MAX_PACKET_LEN - MESH_HEADER_LEN )  // 250

typedef struct
{
    uint8_t  len;  // full packet length including header
    uint8_t  data[LORA_MAX_PACKET_LEN];
    uint32_t due_ms;  // relays only: earliest transmit time
} packet_t;

typedef struct
{
    uint8_t  src;
    uint8_t  len;   // payload length
    uint8_t  hops;  // links travelled, 1 = heard straight from src
    uint8_t  data[MAX_PAYLOAD];
} delivery_t;

typedef struct
{
    bool     b_used;
    uint8_t  src;
    uint16_t pkt_id;
    uint32_t seen_ms;
} seen_t;

// Simple fixed-size ring buffer bookkeeping
typedef struct
{
    uint8_t head;
    uint8_t count;
} ring_t;

static uint8_t      self_id;
static uint16_t     next_pkt_id;
static mesh_stats_t stats;
static uint32_t     drop_threshold;  // drop_rate scaled to 0..UINT32_MAX

static packet_t   txq[LORA_MESH_TXQ_LEN];
static ring_t     txq_ring;
static packet_t   relayq[LORA_MESH_RELAYQ_LEN];  // unordered; each slot has its own due time
static bool       relay_used[LORA_MESH_RELAYQ_LEN];
static delivery_t rxq[LORA_MESH_RXQ_LEN];
static ring_t     rxq_ring;
static seen_t     seen[LORA_MESH_DEDUP_SLOTS];

static bool     heard[LORA_MESH_MAX_NODE_ID + 1];
static uint32_t heard_ms[LORA_MESH_MAX_NODE_ID + 1];

static enum { IN_FLIGHT_NONE, IN_FLIGHT_OWN, IN_FLIGHT_RELAY } in_flight;

static uint32_t heartbeat_interval_ms;
static uint32_t heartbeat_next_ms;

static int8_t last_rssi_dbm;
static int8_t last_snr_db;

static uint8_t rx_buf[LORA_MAX_PACKET_LEN];

// Route of the packet recv() returned last (lora_mesh_last_recv_info), and relays per source
static bool     b_last_recv_valid;
static uint8_t  last_recv_hops;
static uint16_t relayed_from[6];

static uint32_t now_ms( void )
{
    return to_ms_since_boot( get_absolute_time( ) );
}

static inline uint8_t ring_slot( const ring_t* p_ring, uint8_t offset, uint8_t size )
{
    return ( uint8_t ) ( ( p_ring->head + offset ) % size );
}

/* --- Duplicate cache ---------------------------------------------------- */

// Returns true if (src, pkt_id) was seen inside the window; otherwise records it.
static bool seen_check_and_mark( uint8_t src, uint16_t pkt_id, uint32_t now )
{
    int free_slot   = -1;
    int oldest_slot = 0;

    for( int idx = 0; idx < LORA_MESH_DEDUP_SLOTS; idx++ )
    {
        seen_t* p_entry = &seen[idx];
        if( p_entry->b_used && ( now - p_entry->seen_ms ) >= LORA_MESH_DEDUP_WINDOW_MS )
        {
            p_entry->b_used = false;  // expired
        }
        if( !p_entry->b_used )
        {
            if( free_slot < 0 )
            {
                free_slot = idx;
            }
            continue;
        }
        if( p_entry->src == src && p_entry->pkt_id == pkt_id )
        {
            return true;
        }
        if( ( now - p_entry->seen_ms ) > ( now - seen[oldest_slot].seen_ms ) || !seen[oldest_slot].b_used )
        {
            oldest_slot = idx;
        }
    }

    // Full cache: overwrite the oldest entry
    seen_t* p_slot  = &seen[free_slot >= 0 ? free_slot : oldest_slot];
    p_slot->b_used    = true;
    p_slot->src     = src;
    p_slot->pkt_id  = pkt_id;
    p_slot->seen_ms = now;
    return false;
}

/* --- Queues --------------------------------------------------------------- */

static bool enqueue_own( uint8_t dst, const uint8_t* p_payload, uint16_t len )
{
    if( txq_ring.count >= LORA_MESH_TXQ_LEN )
    {
        return false;
    }
    packet_t* p_slot = &txq[ring_slot( &txq_ring, txq_ring.count, LORA_MESH_TXQ_LEN )];

    const uint16_t pkt_id = next_pkt_id++;
    p_slot->data[0]            = self_id;
    p_slot->data[1]            = dst;
    p_slot->data[2]            = ( uint8_t ) ( pkt_id >> 8 );
    p_slot->data[3]            = ( uint8_t ) pkt_id;
    p_slot->data[4]            = LORA_MESH_HOP_LIMIT;
    if( len > 0 )
    {
        memcpy( &p_slot->data[MESH_HEADER_LEN], p_payload, len );
    }
    p_slot->len = ( uint8_t ) ( MESH_HEADER_LEN + len );
    txq_ring.count++;

    // So our own packet echoing back from a neighbour is dropped as a duplicate
    seen_check_and_mark( self_id, pkt_id, now_ms( ) );
    return true;
}

static bool enqueue_relay( const uint8_t* p_pkt, uint8_t len, uint32_t now )
{
    for( int idx = 0; idx < LORA_MESH_RELAYQ_LEN; idx++ )
    {
        if( !relay_used[idx] )
        {
            packet_t* p_slot = &relayq[idx];
            memcpy( p_slot->data, p_pkt, len );
            p_slot->data[4]--;  // one hop used
            p_slot->len = len;
            const uint32_t span =
                LORA_MESH_RELAY_DELAY_MAX_MS - LORA_MESH_RELAY_DELAY_MIN_MS + 1;
            p_slot->due_ms     = now + LORA_MESH_RELAY_DELAY_MIN_MS + ( get_rand_32( ) % span );
            relay_used[idx] = true;
            return true;
        }
    }
    stats.relay_overflow++;
    return false;
}

static void deliver( uint8_t src, const uint8_t* p_payload, uint8_t len, uint8_t hops )
{
    if( rxq_ring.count >= LORA_MESH_RXQ_LEN )
    {
        stats.rx_overflow++;
        return;
    }
    delivery_t* p_item = &rxq[ring_slot( &rxq_ring, rxq_ring.count, LORA_MESH_RXQ_LEN )];
    p_item->src        = src;
    p_item->len        = len;
    p_item->hops       = hops;
    memcpy( p_item->data, p_payload, len );
    rxq_ring.count++;
    stats.delivered++;
}

/* --- Receive handling ------------------------------------------------------- */

static void handle_packet( const uint8_t* p_pkt, uint8_t len, uint32_t now )
{
    if( len < MESH_HEADER_LEN )
    {
        stats.rx_error++;
        return;
    }

    // Demo packet-loss injection: behave as if this packet was never heard
    if( drop_threshold != 0 && get_rand_32( ) < drop_threshold )
    {
        stats.drop_injected++;
        return;
    }

    const uint8_t  src    = p_pkt[0];
    const uint8_t  dst    = p_pkt[1];
    const uint16_t pkt_id = ( uint16_t ) ( ( p_pkt[2] << 8 ) | p_pkt[3] );
    const uint8_t  hops   = p_pkt[4];

    if( src == 0 || src == MESH_BROADCAST )
    {
        stats.rx_error++;
        return;
    }

    stats.rx_count++;
    if( src <= LORA_MESH_MAX_NODE_ID )
    {
        heard[src]    = true;
        heard_ms[src] = now;
    }

    if( seen_check_and_mark( src, pkt_id, now ) )
    {
        stats.dup_dropped++;
        return;
    }

    const uint8_t payload_len = ( uint8_t ) ( len - MESH_HEADER_LEN );

    // Deliver rule: only packets for us or broadcast. Empty packets are heartbeats: not delivered.
    if( ( dst == self_id || dst == MESH_BROADCAST ) && payload_len > 0 )
    {
        // links travelled (1 = direct); 0 = unknown if the hops byte is out of range
        const uint8_t links = hops <= LORA_MESH_HOP_LIMIT ? ( uint8_t ) ( LORA_MESH_HOP_LIMIT - hops + 1 ) : 0;
        deliver( src, &p_pkt[MESH_HEADER_LEN], payload_len, links );
    }

    // Relay rule: anything not addressed only to us, while hops remain
    if( dst != self_id )
    {
        if( hops > 0 )
        {
            if( enqueue_relay( p_pkt, len, now ) && src < sizeof( relayed_from ) / sizeof( relayed_from[0] ) )
            {
                relayed_from[src]++;
            }
        }
        else
        {
            stats.hop_dropped++;
        }
    }
}

/* --- Transmit scheduling ---------------------------------------------------- */

static void start_next_tx( uint32_t now )
{
    if( lora_radio_tx_active( ) )
    {
        return;
    }

    // Due relays first (oldest due time), so the flood keeps moving
    int best = -1;
    for( int idx = 0; idx < LORA_MESH_RELAYQ_LEN; idx++ )
    {
        if( relay_used[idx] && ( int32_t ) ( now - relayq[idx].due_ms ) >= 0 &&
            ( best < 0 || ( int32_t ) ( relayq[idx].due_ms - relayq[best].due_ms ) < 0 ) )
        {
            best = idx;
        }
    }
    if( best >= 0 )
    {
        relay_used[best] = false;
        if( lora_radio_start_tx( relayq[best].data, relayq[best].len ) )
        {
            in_flight = IN_FLIGHT_RELAY;
        }
        else
        {
            stats.tx_fail++;
        }
        return;
    }

    if( txq_ring.count > 0 )
    {
        packet_t* p_slot   = &txq[txq_ring.head];
        txq_ring.head = ring_slot( &txq_ring, 1, LORA_MESH_TXQ_LEN );
        txq_ring.count--;
        if( lora_radio_start_tx( p_slot->data, p_slot->len ) )
        {
            in_flight = IN_FLIGHT_OWN;
        }
        else
        {
            stats.tx_fail++;
        }
    }
}

/* --- Public interface ------------------------------------------------------ */

bool lora_mesh_init( uint8_t node_id )
{
    memset( &stats, 0, sizeof( stats ) );
    memset( seen, 0, sizeof( seen ) );
    memset( relay_used, 0, sizeof( relay_used ) );
    memset( heard, 0, sizeof( heard ) );
    memset( relayed_from, 0, sizeof( relayed_from ) );
    b_last_recv_valid = false;
    txq_ring  = ( ring_t ){ 0 };
    rxq_ring  = ( ring_t ){ 0 };
    in_flight = IN_FLIGHT_NONE;

    self_id = node_id;
    // Random start so packet IDs after a reboot don't collide with neighbours' duplicate caches
    next_pkt_id = ( uint16_t ) get_rand_32( );
    return lora_radio_init( );
}

bool lora_mesh_send( uint8_t dst, const uint8_t* p_payload, uint16_t len )
{
    if( len == 0 || len > MAX_PAYLOAD || p_payload == NULL )
    {
        return false;
    }
    return enqueue_own( dst, p_payload, len );
}

int lora_mesh_recv( uint8_t* p_src, uint8_t* p_buf, uint16_t buf_size )
{
    if( rxq_ring.count == 0 )
    {
        return MESH_RECV_NONE;
    }
    const delivery_t* p_item = &rxq[rxq_ring.head];
    rxq_ring.head       = ring_slot( &rxq_ring, 1, LORA_MESH_RXQ_LEN );
    rxq_ring.count--;

    if( p_item->len > buf_size )
    {
        return MESH_RECV_BUF_TOO_SMALL;
    }
    memcpy( p_buf, p_item->data, p_item->len );
    *p_src            = p_item->src;
    b_last_recv_valid = true;
    last_recv_hops  = p_item->hops;
    return p_item->len;
}

void lora_mesh_poll( void )
{
    uint32_t now = now_ms( );

    // Handle a few radio events per call, without ever spinning here
    for( int idx = 0; idx < 4; idx++ )
    {
        uint8_t                  len;
        int8_t                   rssi, snr;
        const lora_radio_event_t evt = lora_radio_service( rx_buf, &len, &rssi, &snr );
        if( evt == LORA_EVT_NONE )
        {
            break;
        }
        switch( evt )
        {
        case LORA_EVT_RX_DONE:
            last_rssi_dbm = rssi;
            last_snr_db   = snr;
            handle_packet( rx_buf, len, now );
            break;
        case LORA_EVT_RX_ERROR:
            stats.rx_error++;
            break;
        case LORA_EVT_TX_DONE:
            if( in_flight == IN_FLIGHT_OWN )
            {
                stats.tx_count++;
            }
            else if( in_flight == IN_FLIGHT_RELAY )
            {
                stats.relayed++;
            }
            else
            {
                // IN_FLIGHT_NONE: a TX_DONE with nothing in flight, nothing to count
            }
            in_flight = IN_FLIGHT_NONE;
            break;
        case LORA_EVT_TX_FAIL:
            stats.tx_fail++;
            in_flight = IN_FLIGHT_NONE;
            break;
        default:
            break;
        }
    }

    now = now_ms( );
    if( heartbeat_interval_ms != 0 && ( int32_t ) ( now - heartbeat_next_ms ) >= 0 )
    {
        heartbeat_next_ms = now + heartbeat_interval_ms;
        enqueue_own( MESH_BROADCAST, NULL, 0 );  // empty = heartbeat; skipped if queue is full
    }

    start_next_tx( now );
}

uint16_t lora_mesh_max_payload( void )
{
    return MAX_PAYLOAD;
}

bool lora_mesh_last_heard( uint8_t node_id, uint32_t* p_ms_since_boot )
{
    if( node_id > LORA_MESH_MAX_NODE_ID || !heard[node_id] )
    {
        return false;
    }
    *p_ms_since_boot = heard_ms[node_id];
    return true;
}

bool lora_mesh_tx_busy( void )
{
    return txq_ring.count > 0 || lora_radio_tx_active( );
}

void lora_mesh_get_stats( mesh_stats_t* p_out )
{
    *p_out = stats;
}

void lora_mesh_set_drop_rate( float rate )
{
    if( rate <= 0.0f )
    {
        drop_threshold = 0;
    }
    else if( rate >= 1.0f )
    {
        drop_threshold = UINT32_MAX;
    }
    else
    {
        drop_threshold = ( uint32_t ) ( rate * 4294967296.0f );
    }
}

void lora_mesh_set_heartbeat( uint32_t interval_ms )
{
    heartbeat_interval_ms = interval_ms;
    heartbeat_next_ms     = now_ms( );
}

void lora_mesh_last_link_quality( int8_t* p_rssi_dbm, int8_t* p_snr_db )
{
    *p_rssi_dbm = last_rssi_dbm;
    *p_snr_db   = last_snr_db;
}

bool lora_mesh_last_recv_info( uint8_t* p_hops, uint8_t* p_prev_hop )
{
    if( p_hops != NULL )
    {
        *p_hops = b_last_recv_valid ? last_recv_hops : 0;
    }
    if( p_prev_hop != NULL )
    {
        *p_prev_hop = 0;  // unknown: the LoRa header has no relay field
    }
    return b_last_recv_valid;
}

uint16_t lora_mesh_relayed_from( uint8_t src )
{
    return src < sizeof( relayed_from ) / sizeof( relayed_from[0] ) ? relayed_from[src] : 0;
}

const mesh_radio_t g_lora_mesh_radio = {
    .p_name          = "lora",
    .p_send          = lora_mesh_send,
    .p_recv          = lora_mesh_recv,
    .p_poll          = lora_mesh_poll,
    .p_max_payload   = lora_mesh_max_payload,
    .p_last_heard    = lora_mesh_last_heard,
    .p_tx_busy       = lora_mesh_tx_busy,
    .p_get_stats     = lora_mesh_get_stats,
    .p_set_drop_rate = lora_mesh_set_drop_rate,
};
