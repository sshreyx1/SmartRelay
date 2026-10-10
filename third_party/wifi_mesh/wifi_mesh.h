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
extern const mesh_radio_t wifi_mesh;

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
void wifi_mesh_set_topology_filter(bool on);

/* Packets ignored by the topology filter (for debugging). */
uint32_t wifi_mesh_filtered_count(void);

#endif
