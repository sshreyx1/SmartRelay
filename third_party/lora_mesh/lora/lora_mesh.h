/*
 * lora_mesh.h - Flooding mesh over LoRa (CSC09, Member 1).
 *
 * Implements the shared mesh_radio_t interface (mesh/mesh_radio.h). The
 * fragmentation layer should normally use `lora_mesh_radio` rather than calling
 * the functions below directly, so it can swap in the WiFi mesh unchanged.
 */
#ifndef LORA_MESH_H
#define LORA_MESH_H

#include "mesh_radio.h"

// Tunable constants
#define LORA_MESH_HOP_LIMIT          4      // starting hops; relay only while > 0
#define LORA_MESH_DEDUP_WINDOW_MS    30000  // remember (src, pkt_id) this long
#define LORA_MESH_DEDUP_SLOTS        128
#define LORA_MESH_RELAY_DELAY_MIN_MS 10     // random wait before rebroadcasting
#define LORA_MESH_RELAY_DELAY_MAX_MS 100
#define LORA_MESH_TXQ_LEN            8      // own packets waiting to send (contract: >= 4)
#define LORA_MESH_RELAYQ_LEN         8
#define LORA_MESH_RXQ_LEN            8      // packets waiting for recv()
#define LORA_MESH_MAX_NODE_ID        16     // node IDs 1..16 tracked by last_heard

// Set up the LR1121 and the mesh. node_id is 1-5 (A-E). Returns false if the radio does not answer.
bool lora_mesh_init( uint8_t node_id );

bool     lora_mesh_send( uint8_t dst, const uint8_t* payload, uint16_t len );
int      lora_mesh_recv( uint8_t* src, uint8_t* buf, uint16_t buf_size );
void     lora_mesh_poll( void );
uint16_t lora_mesh_max_payload( void );
bool     lora_mesh_last_heard( uint8_t node_id, uint32_t* ms_since_boot );
bool     lora_mesh_tx_busy( void );
void     lora_mesh_get_stats( mesh_stats_t* out );
void     lora_mesh_set_drop_rate( float rate );

// Proposal for the "who sends the bridge heartbeat" open question: the bridge node
// calls this once (e.g. 5000 ms). It floods an empty broadcast packet, which updates
// every node's last_heard() but is never delivered through recv(). 0 = off (default).
void lora_mesh_set_heartbeat( uint32_t interval_ms );

// RSSI/SNR of the last packet heard directly (any hop), for debugging.
void lora_mesh_last_link_quality( int8_t* rssi_dbm, int8_t* snr_db );

extern const mesh_radio_t lora_mesh_radio;

#endif  // LORA_MESH_H
