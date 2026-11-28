/*
 * mesh_radio.h - Shared radio interface for CSC09 trail-mesh (C version of the
 * "Radio Interface Contract - LoRa & WiFi Mesh <-> Fragmentation").
 *
 * The LoRa mesh (lora_mesh.c) and the WiFi mesh (wifi_mesh.c) each export one
 * mesh_radio_t with exactly these functions, so the fragmentation layer can run a
 * transfer over either radio without knowing which one it is.
 *
 * Rules both meshes follow (see the contract for the full list):
 *   - Byte-exact, best effort: no mesh-level ACKs, retries or ordering.
 *   - Corrupt packets are dropped, never delivered.
 *   - send()/recv() never block for more than a few ms; transmitting happens in poll().
 *   - Radio problems are counted in stats, never fatal.
 */
#ifndef MESH_RADIO_H
#define MESH_RADIO_H

#include "float_types.h"
#include <stdbool.h>
#include <stdint.h>

#define MESH_BROADCAST   0xFF  // dst value meaning "every node"
#define MESH_HEADER_LEN  5     // src(1) dst(1) pkt_id(2) hops(1), same layout on both radios

// recv() return codes (a value >= 0 is the payload length)
#define MESH_RECV_NONE          (-1)  // nothing waiting
#define MESH_RECV_BUF_TOO_SMALL (-2)  // packet discarded: buf_size < payload length

typedef struct mesh_stats_s
{
    uint32_t tx_count;             // packets this node created and transmitted
    uint32_t rx_count;             // valid packets heard over the air (before duplicate check)
    uint32_t delivered;      // packets handed up through recv()
    uint32_t relayed;        // other nodes' packets rebroadcast by this node
    uint32_t dup_dropped;    // packets already seen within the duplicate window
    uint32_t hop_dropped;    // packets that needed relaying but had hops == 0
    uint32_t tx_fail;        // transmissions the radio failed to complete
    uint32_t rx_error;       // CRC/header errors and malformed packets
    uint32_t rx_overflow;    // delivered packets lost because the recv queue was full
    uint32_t relay_overflow; // relays skipped because the relay queue was full
    uint32_t drop_injected;  // packets discarded on purpose by drop_rate (demo)
} mesh_stats_t;

typedef struct mesh_radio_s
{
    const char* p_name;  // "lora" or "wifi"

    // Queue payload for flooding to node dst (1-5 or MESH_BROADCAST).
    // Returns false if the queue is full, len is 0, or len > max_payload(). Returns quickly.
    bool ( *p_send )( uint8_t dst, const uint8_t* p_payload, uint16_t len );

    // Copy ONE packet addressed to this node or to MESH_BROADCAST into buf.
    // Returns the payload length (>= 0), MESH_RECV_NONE, or MESH_RECV_BUF_TOO_SMALL.
    // *src is the node that created the packet, not the last relay. Never blocks.
    int ( *p_recv )( uint8_t* p_src, uint8_t* p_buf, uint16_t buf_size );

    // Service the radio: read incoming packets, relay, transmit queued packets.
    // Call from the main loop every few ms.
    void ( *p_poll )( void );

    // Bytes available to the caller per packet (radio limit minus mesh header). Constant after init.
    uint16_t ( *p_max_payload )( void );

    // Time (ms since boot) we last received ANY packet created by node_id.
    // Returns false if never heard.
    bool ( *p_last_heard )( uint8_t node_id, uint32_t* p_ms_since_boot );

    // True while the send queue is non-empty or the radio is mid-transmission.
    bool ( *p_tx_busy )( void );

    // Copy the counters since boot.
    void ( *p_get_stats )( mesh_stats_t* p_out );

    // Demo only: fraction (0.0-1.0) of received packets to discard on purpose.
    void ( *p_set_drop_rate )( float32_t rate );
} mesh_radio_t;

#endif  // MESH_RADIO_H
