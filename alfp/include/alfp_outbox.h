/*
 * alfp_outbox.h - store-and-forward queue on the microSD card.   OWNER: Member 2
 *
 * A capture that cannot be sent right now (no radio can reach the bridge, the bridge is busy,
 * or a transfer gave up) is not lost: it stays on the SD card as a small job file and is sent
 * later, oldest first.
 *
 *   outbox/q00000017.job  =  [ 'Q' '1' ][ path_len ][ path ][ meta_len ][ meta ]
 *
 * The job points at the image file (for us, one of the sample images standing in for a camera)
 * and carries the capture metadata. Job files survive a reboot, so a node that loses power with
 * a backlog still sends it afterwards.
 *
 * Radio choice happens once per ATTEMPT, just before alfp_send_file(): each transfer is committed
 * to one radio from its first packet to its last; only a fresh attempt may pick the other radio.
 */
#ifndef ALFP_OUTBOX_H
#define ALFP_OUTBOX_H

#include <stdbool.h>
#include <stdint.h>

#include "alfp.h"

#ifndef ALFP_OUTBOX_MAX
#define ALFP_OUTBOX_MAX 64u /* retention cap: when full, the OLDEST job is dropped */
#endif

typedef struct
{
    alfp_ctx_t* alfp;
    const mesh_radio_t* const* radios; /* preference order, e.g. {wifi, lora} */
    uint8_t n_radios;
    uint8_t bridge_id;
    uint32_t fresh_ms; /* bridge must have been heard this recently (10 s agreed) */
    const char* dir;
    /* state */
    uint32_t next_seq;
    uint16_t count;
    uint32_t active_seq; /* job being sent, 0 = none */
    uint32_t retry_at_ms;
    uint32_t backoff_ms;
    uint32_t dropped_full;
    uint32_t sent_ok;
    uint32_t no_radio_waits;
} alfp_outbox_t;

/* Scans dir to recover jobs left from before a reboot. */
void alfp_outbox_init(alfp_outbox_t* ob, alfp_ctx_t* alfp, const mesh_radio_t* const* radios, uint8_t n_radios,
                      uint8_t bridge_id, uint32_t fresh_ms, const char* dir);

/* Queue a capture. Returns false only if the SD card write fails. */
bool alfp_outbox_add(alfp_outbox_t* ob, const char* image_path, const uint8_t* meta, uint8_t meta_len);

/* Start the next transfer if the sender is idle and a radio can reach the bridge. Call every loop. */
void alfp_outbox_pump(alfp_outbox_t* ob);

/* Call from the alfp on_tx_done callback. */
void alfp_outbox_on_tx_done(alfp_outbox_t* ob, const alfp_tx_report_t* r);

uint16_t alfp_outbox_count(const alfp_outbox_t* ob);

#endif
