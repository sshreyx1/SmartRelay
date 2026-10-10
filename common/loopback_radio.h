/*
 * loopback_radio.h - two in-memory mesh_radio_t endpoints wired to each other, with loss.
 *
 * Used by the on-Pico self-test (firmware/selftest): the whole ALFP pipeline, including the
 * real microSD card, runs on ONE Pico with no radio hardware at all. Endpoint A is a field node
 * (id 2), endpoint B is the bridge (id 1).
 */
#ifndef LOOPBACK_RADIO_H
#define LOOPBACK_RADIO_H

#include <stdint.h>

#include "mesh_radio.h"

extern const mesh_radio_t loopback_a;
extern const mesh_radio_t loopback_b;

/* now_ms / rand32 supplied by the platform; airtime_ms models how long tx_busy() stays true. */
void loopback_init(uint32_t (*now_ms)(void), uint32_t (*rand32)(void), uint16_t max_payload, uint32_t airtime_ms,
                   uint32_t latency_ms);
void loopback_set_loss(float loss); /* both directions */

#endif
