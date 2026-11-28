/*
 * sim_mesh.h - a laptop simulation of the LoRa and WiFi flooding meshes.
 *
 * It implements mesh_radio.h exactly as Mridula's and Goutham's code will (same rules:
 * 5-byte header, hop limit 4, 30 s duplicate window, send queue 8, random relay delay,
 * drop_rate on receive, heartbeat from the bridge to 0xFE every 3 s), and adds the
 * physics that makes real meshes lose packets:
 *
 *   - airtime: LoRa from Semtech's formula (SF7/125 kHz/CR4/5), WiFi = UART + ESP-NOW frame
 *   - half duplex: a node that is transmitting cannot hear anything
 *   - collisions: two neighbours on air at once at a receiver = both lost
 *     (LoRa: no carrier sense; WiFi: ESP-NOW listens before talking)
 *   - random link loss, and optional bit corruption after the radio CRC (UART/SPI faults)
 *
 * Time is virtual (1 ms ticks), so a 60-second LoRa transfer simulates in milliseconds and
 * every run is reproducible from its seed.
 *
 * mesh_radio_t has no context pointer, so the simulator keeps a "current node": call
 * sim_set_current(i) before running node i's code. Real firmware has one node per chip and
 * does not need this.
 */
#ifndef SIM_MESH_H
#define SIM_MESH_H

#include "float_types.h"
#include <stdbool.h>
#include <stdint.h>

#include "mesh_radio.h"

#define SIM_MAX_NODES 6
#define SIM_LORA 0
#define SIM_WIFI 1
#define SIM_KINDS 2

typedef struct
{
    float32_t link_loss;        /* per-hop random loss 0..1 */
    float32_t corrupt_rate;     /* chance a delivered payload has one bit flipped (after radio CRC) */
    uint16_t relay_min_ms;
    uint16_t relay_max_ms;
    bool b_carrier_sense;
    uint16_t max_payload;   /* 250 LoRa, 245 WiFi */
    uint32_t uart_baud;     /* WiFi only: Pico <-> ESP8266 link */
} sim_kind_cfg_t;

typedef struct
{
    uint32_t seed;
    uint8_t n_nodes;   /* ids 1..n */
    uint8_t bridge_id; /* sends heartbeats */
    bool adj[SIM_KINDS][SIM_MAX_NODES + 1][SIM_MAX_NODES + 1];
    sim_kind_cfg_t kind[SIM_KINDS];
    uint32_t heartbeat_ms; /* 0 = off */
} sim_cfg_t;

/* Defaults matching the agreed contract numbers. */
void sim_default_cfg(sim_cfg_t* p_cfg, uint8_t n_nodes);
/* Topology helpers (both radios unless kind >= 0). */
void sim_link(sim_cfg_t* p_cfg, int kind, uint8_t node_a, uint8_t node_b);
void sim_topology_chain(sim_cfg_t* p_cfg, int kind);   /* 1-2-3-...-n */
void sim_topology_full(sim_cfg_t* p_cfg, int kind);    /* everyone hears everyone */
void sim_topology_trail(sim_cfg_t* p_cfg, int kind);   /* chain + skip links (redundant paths) */

void sim_init(const sim_cfg_t* p_cfg);
/* Change a link while running (a node walks out of range, a link comes back). */
void sim_set_link(int kind, uint8_t node_a, uint8_t node_b, bool b_link_up);
void sim_set_link_loss(int kind, float32_t loss);
void sim_set_corrupt_rate(int kind, float32_t rate);
void sim_set_current(uint8_t node_id);
uint32_t sim_now(void);
/* Advance the physical layer by 1 ms (finish transmissions, deliver, collide). */
void sim_tick(void);
uint32_t sim_rand(void);
float32_t sim_frand(void);

extern const mesh_radio_t g_sim_lora;
extern const mesh_radio_t g_sim_wifi;

/* Per-node stats access for experiments. */
void sim_get_stats(uint8_t node_id, int kind, mesh_stats_t* p_out);
void sim_set_drop_rate(uint8_t node_id, int kind, float32_t rate);
/* Total ms any node spent transmitting on this radio (channel usage). */
uint32_t sim_airtime_used(int kind);

/* LoRa time on air (ms) for a payload of n bytes at SF7/125 kHz/CR4/5, 8 preamble, header+CRC on. */
float64_t sim_lora_airtime_ms(uint16_t len);

#endif
