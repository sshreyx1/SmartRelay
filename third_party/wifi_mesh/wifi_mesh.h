/*
 * wifi_mesh.h - WiFi flooding mesh for CSC09 SmartRelay (Goutham).
 *
 * Exposes `wifi_mesh`, a mesh_radio_t (see Mridula's mesh_radio.h), so the
 * fragmentation layer can use WiFi exactly like LoRa.
 *
 *   Fragmentation (Shreya)
 *        |  send / recv / poll ...      <- mesh_radio_t
 *   wifi_mesh.c  (this)                 <- flooding, duplicates, hop limit
 *        |  wst_link_send / recv
 *   wst_link.c                          <- frames over UART to the WST
 *        |
 *   WST (ESP-NOW broadcast)
 *
 * Over-the-air packet = 5-byte header + payload (max 245 bytes):
 *   [src][dst][pkt_id lo][pkt_id hi][hops][payload...]
 *   pkt_id is little-endian; hops = relays still allowed.
 */
#ifndef WIFI_MESH_H
#define WIFI_MESH_H

#include <stdbool.h>
#include <stdint.h>
#include "mesh_radio.h"

#ifndef MESH_INTERNAL
#define MESH_INTERNAL 0xFE   /* relayed + updates last_heard, never delivered */
#endif

#define WIFI_MESH_MAX_PAYLOAD 245   /* 250 ESP-NOW bytes - 5 header bytes */

/* The radio object the fragmentation layer uses. */
extern const mesh_radio_t g_wifi_mesh;

/* Start the mesh. Call once, before anything else.
 *   node_id 1-5 : use this ID.
 *   node_id 0   : work it out from this WST's MAC (wifi_mesh_config.h).
 * Starts wst_link too. May wait up to 2 s for the WST to report its MAC.
 * Returns false if the ID is invalid or the MAC is not in the table.     */
bool wifi_mesh_init(uint8_t node_id);

/* This node's ID (0 if not started). */
uint8_t wifi_mesh_node_id(void);

/* Topology filter: ON (default) = only accept packets from neighbours in
 * wifi_mesh_config.h, to force multi-hop in a small room.               */
void wifi_mesh_set_topology_filter(bool b_enable);

/* Packets ignored by the topology filter (for debugging). */
uint32_t wifi_mesh_filtered_count(void);

/* Route of the packet recv() returned most recently (not part of mesh_radio_t):
 *   hops     links it travelled (1 = heard straight from its source)
 *   prev_hop node we heard it from, from the sender WST's MAC (0 = not in the table)
 * Returns false (and zeros) if recv() has not returned a packet yet.            */
bool wifi_mesh_last_recv_info(uint8_t *p_hops, uint8_t *p_prev_hop);

/* Packets from node src (1-5) this node has queued for relaying since boot
 * (wraps at 65536; use differences).                                        */
uint16_t wifi_mesh_relayed_from(uint8_t src);

#endif
