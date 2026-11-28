/*
 * radios.h - the one place where the real LoRa and WiFi meshes are plugged in.
 *
 * A radio that is not linked is an "offline" stub: it never hears the bridge and refuses to send,
 * so captures simply wait in the SD outbox. Nothing else changes when the real code arrives.
 *
 * LoRa (Mridula, third_party/lora_mesh): g_lora_mesh_radio, lora_mesh_init(), lora_mesh_set_heartbeat().
 *   Linked by default. Build with -DUSE_LORA_MESH=OFF for the offline stub.
 * WiFi (Goutham, third_party/wifi_mesh): wifi_mesh, wifi_mesh_init(), ESP-01 on UART0 GP16/GP17.
 *   Linked by default. Build with -DUSE_WIFI_MESH=OFF for the offline stub.
 */
#ifndef RADIOS_H
#define RADIOS_H

#include <stdbool.h>
#include <stdint.h>

#include "mesh_radio.h"

#define RADIOS_BRIDGE_ID 1

/* Starts both radios. Returns true if at least one works. Must run AFTER the SD card is mounted
 * (the SD card has to be in SPI mode before LoRa traffic appears on the shared bus). */
bool radios_init(uint8_t node_id);
bool radios_lora_ok(void);
bool radios_wifi_ok(void);
void radios_set_wifi_topology_filter(bool b_enable);
const mesh_radio_t* radio_lora(void);
const mesh_radio_t* radio_wifi(void);

/* Which ESP-01 (WST) is in this Pico's socket, as found by radios_init(): 1-5 = WST-n from
 * wifi_mesh_config.h, 0 = answered but not in the table, -1 = no answer (or WiFi not built).
 * mac (may be NULL) gets its MAC when the result is >= 0. Read-only after radios_init(). */
int radios_wst_owner(uint8_t mac[6]);

/* Route of the packet r->recv() returned most recently (TRANSFER_VISIBILITY.md 3.1):
 *   *hops = links travelled (1 = direct, 0 = unknown), *prev = node it was heard from (0 = unknown;
 *   always 0 on LoRa, whose header has no relay field). Returns false for the offline stubs. */
bool radios_last_recv_info(const mesh_radio_t* p_radio, uint8_t* p_hops, uint8_t* p_prev);

/* Packets from node src that this node has queued for relaying on r since boot (wraps at 65536).
 * 0 for the offline stubs. */
uint16_t radios_relayed_from(const mesh_radio_t* p_radio, uint8_t src);

#endif
