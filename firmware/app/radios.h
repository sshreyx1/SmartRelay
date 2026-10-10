/*
 * radios.h - the one place where the real LoRa and WiFi meshes are plugged in.
 *
 * A radio that is not linked is an "offline" stub: it never hears the bridge and refuses to send,
 * so captures simply wait in the SD outbox. Nothing else changes when the real code arrives.
 *
 * LoRa (Mridula, third_party/lora_mesh): lora_mesh_radio, lora_mesh_init(), lora_mesh_set_heartbeat().
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
void radios_set_wifi_topology_filter(bool on);
const mesh_radio_t* radio_lora(void);
const mesh_radio_t* radio_wifi(void);

#endif
