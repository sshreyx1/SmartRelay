/*
 * alfp.h - Application-Layer Fragmentation Protocol, CSC09 trail mesh.   OWNER: Member 2
 *
 * Sends a file from a field node's microSD card to the bridge node over ONE mesh radio
 * (LoRa or WiFi, chosen once per transfer), and reassembles it on the bridge's microSD card.
 *
 * How a transfer works (see docs/DESIGN.md for the full reasoning):
 *   sender: hash file (CRC-32) -> MANIFEST -> DATA 0..n-1 -> POLL
 *   bridge: stores fragments straight to SD at seq * frag_size, tracks a bitmap
 *   bridge answers POLL with a NACK listing exactly the missing fragments
 *   sender resends only those, POLLs again ... until the bridge sends ACK(OK)
 *   the bridge also ACKs as soon as the last fragment arrives and the CRC-32 checks out
 *
 * Usage (every node, in ONE task/loop):
 *   alfp_init(&ctx, &cfg);
 *   loop:
 *     radio->poll();                                    (mesh layer)
 *     alfp_service(&ctx, radios, n_radios);             (recv + timers + pacing)
 *
 * The library is single-threaded by design: call every function from the same task.
 */
#ifndef ALFP_H
#define ALFP_H

#include <stdbool.h>
#include <stdint.h>

#include "alfp_config.h"
#include "alfp_port.h"
#include "alfp_wire.h"
#include "mesh_radio.h"

/* Per-radio timing. Chosen by radio->name ("lora" / "wifi"). See alfp_profiles.c for the numbers
 * and how they were derived from the LoRa airtime formula and ESP-NOW figures. */
typedef struct alfp_profile_s
{
    const char* radio_name;
    uint32_t gap_ms;          /* minimum wait after tx_busy() clears before the next packet (lets relays finish) */
    uint32_t gap_max_ms;      /* adaptive pacing never waits longer than this (= gap_ms disables adapting) */
    uint32_t poll_timeout_ms; /* wait for a NACK/ACK after a POLL before polling again */
    uint32_t rx_expiry_ms;    /* bridge drops an incomplete session idle this long */
    uint8_t max_rounds;       /* hard cap on rounds (file stays queued on SD if reached) */
    uint8_t max_stalled;      /* give up after this many rounds in a row that recover nothing */
    uint8_t max_polls;        /* unanswered POLLs in a row before giving up */
    uint8_t poll_copies;      /* send each POLL this many times (tiny packet; losing it costs a timeout) */
    uint8_t tail_dup;         /* when a NACK asks for this few fragments or fewer, send each twice (0 = off) */
    uint8_t radio_id;         /* ALFP_RADIO_* written into the manifest */
} alfp_profile_t;

/* Built-in profile for a radio, looked up by radio->name. */
const alfp_profile_t* alfp_default_profile(const mesh_radio_t* radio);

typedef enum
{
    ALFP_OK = 0,
    ALFP_E_BUSY,         /* a transfer is already running on this node */
    ALFP_E_OPEN,         /* cannot open the file */
    ALFP_E_EMPTY,        /* file is empty */
    ALFP_E_TOO_BIG,      /* needs more than ALFP_MAX_FRAGS fragments */
    ALFP_E_ARG,          /* bad argument (meta too long, radio NULL, ...) */
} alfp_err_t;

typedef enum
{
    ALFP_TX_DONE = 0,     /* bridge confirmed the file with a matching CRC-32 */
    ALFP_TX_GAVE_UP,      /* max_rounds or max_polls exhausted */
    ALFP_TX_REFUSED,      /* bridge answered BUSY / STORAGE / TOO_BIG */
    ALFP_TX_IO_ERROR,     /* local SD read failed */
} alfp_tx_result_t;

typedef struct
{
    alfp_tx_result_t result;
    uint8_t ack_status; /* ALFP_ACK_* when result is DONE or REFUSED */
    uint8_t why;        /* GAVE_UP reason: 1 unanswered polls, 2 no progress, 3 round cap */
    uint16_t sid;
    const char* radio;
    const char* path;
    uint32_t file_size;
    uint32_t file_crc32;     /* CRC-32 of the file as read from this node's SD card */
    uint16_t frag_count;
    uint16_t frag_size;
    uint8_t rounds;          /* 1 = everything arrived first time */
    uint16_t data_sent;      /* DATA packets including resends */
    uint16_t data_resent;
    uint16_t manifests_sent;
    uint16_t polls_sent;
    uint16_t nacks_received;
    uint32_t bytes_on_air;   /* ALFP bytes handed to the mesh (excluding mesh header) */
    uint32_t gap_ms_end;     /* adaptive pacing gap when the transfer finished */
    uint32_t started_ms;
    uint32_t finished_ms;
} alfp_tx_report_t;

typedef struct
{
    uint8_t src;
    uint16_t sid;
    const char* radio;
    const char* path;       /* final file on the bridge's SD card */
    uint32_t file_size;
    uint16_t frag_count;
    uint32_t file_crc32;
    const uint8_t* meta;
    uint8_t meta_len;
    uint16_t fragments_received; /* unique */
    uint16_t duplicates;
    uint16_t nacks_sent;
    uint32_t first_ms;
    uint32_t done_ms;
} alfp_rx_report_t;

typedef struct
{
    uint32_t pkts_in, pkts_bad_crc, pkts_bad_other;
    uint32_t tx_ok, tx_failed;
    uint32_t rx_ok, rx_crc_fail, rx_expired, rx_refused_busy;
    uint32_t data_dup, data_orphan;
    uint32_t send_blocked; /* radio queue full, retried later */
} alfp_stats_t;

/* ---- internal state (exposed only so it can be statically allocated) ------------------ */

typedef enum
{
    TX_IDLE = 0,
    TX_HASHING,
    TX_SENDING,
    TX_WAITING,
} alfp_tx_state_t;

typedef struct
{
    alfp_tx_state_t state;
    const mesh_radio_t* radio;
    const alfp_profile_t* prof;
    uint8_t dst;
    int fh;
    char path[ALFP_PATH_MAX];
    uint8_t meta[ALFP_MAX_META];
    uint8_t meta_len;
    uint16_t sid;
    uint32_t file_size;
    uint16_t frag_count;
    uint16_t frag_size;
    uint32_t crc32;
    uint32_t hash_off;
    uint8_t todo[ALFP_BITMAP_BYTES]; /* fragments still to send this round */
    uint16_t todo_next;              /* scan position in todo */
    bool send_manifest;
    uint8_t polls_left; /* copies of the POLL still to send */
    uint16_t last_missing; /* fragments the previous NACK asked for */
    uint8_t copies;        /* copies of each fragment this round (1 or 2) */
    uint8_t copy_left;     /* extra copies of the current fragment still to send */
    uint16_t sent_round;   /* DATA packets sent in the current round */
    uint32_t gap_ms;       /* current adaptive gap */
    uint8_t stalled;       /* rounds in a row with no progress */
    uint8_t round;
    uint8_t polls_unanswered;
    uint32_t next_tx_ms;
    uint32_t wait_until_ms;
    bool was_busy;
    alfp_tx_report_t rep;
} alfp_tx_t;

typedef enum
{
    RX_FREE = 0,
    RX_ACTIVE,
    RX_VERIFYING,
} alfp_rx_state_t;

typedef struct
{
    alfp_rx_state_t state;
    const mesh_radio_t* radio;
    const alfp_profile_t* prof;
    uint8_t src;
    uint16_t sid;
    int fh;
    char tmp_path[ALFP_PATH_MAX];
    bool have_manifest;
    alfp_manifest_t m;
    uint8_t meta[ALFP_MAX_META];
    uint16_t frag_count;
    uint16_t frag_size;
    uint8_t have[ALFP_BITMAP_BYTES];
    uint16_t n_have;
    uint32_t verify_off;
    uint32_t verify_crc;
    uint32_t last_ms;
    /* replies waiting for room in the radio's send queue */
    bool pending_nack;
    uint8_t pending_round;
    bool pending_ack;
    uint8_t pending_status;
    alfp_rx_report_t rep;
} alfp_rx_t;

typedef struct
{
    uint8_t src;
    uint16_t sid;
    uint8_t status;
    bool used;
} alfp_done_t;

/* Pacing learned per radio, carried from one transfer to the next. */
typedef struct
{
    const mesh_radio_t* radio;
    uint32_t gap_ms;
} alfp_pace_t;

typedef struct alfp_ctx_s alfp_ctx_t;

typedef struct
{
    uint8_t node_id;   /* 1-5 */
    uint8_t bridge_id; /* node that reassembles (1 = A in our deployment) */
    const alfp_storage_t* storage;
    const alfp_platform_t* platform;
    const char* inbox_dir; /* bridge: finished files go here, e.g. "inbox" */
    const char* tmp_dir;   /* bridge: partial files, e.g. "rx" */
    void (*on_tx_done)(alfp_ctx_t* ctx, const alfp_tx_report_t* r);
    void (*on_rx_done)(alfp_ctx_t* ctx, const alfp_rx_report_t* r);
    void* user;
    /* Optional: replace the built-in timing (matched by radio_name). NULL = built-in. */
    const alfp_profile_t* profiles;
    uint8_t n_profiles;
    /* Experiments only: behave like a naive protocol that resends the WHOLE file after any loss.
     * Used to measure what selective retransmission saves. Never set in firmware. */
    bool baseline_resend_all;
} alfp_cfg_t;

struct alfp_ctx_s
{
    alfp_cfg_t cfg;
    alfp_tx_t tx;
    alfp_rx_t rx[ALFP_RX_SESSIONS];
    alfp_done_t done[ALFP_DONE_MEMORY];
    uint8_t done_next;
    uint16_t next_sid;
    alfp_pace_t pace[2];
    uint8_t pkt[ALFP_MAX_PKT];
    uint8_t io[ALFP_HASH_CHUNK];
    alfp_stats_t stats;
};

/* ---- API -------------------------------------------------------------------------------- */

/* Timing used by this context for a radio (override from cfg, else built-in). */
const alfp_profile_t* alfp_profile_for(const alfp_ctx_t* ctx, const mesh_radio_t* radio);

void alfp_init(alfp_ctx_t* ctx, const alfp_cfg_t* cfg);

/* Start sending path (a file on this node's SD card) to dst over radio. meta is copied.
 * Returns ALFP_OK if the transfer started; the result arrives later via cfg.on_tx_done. */
alfp_err_t alfp_send_file(alfp_ctx_t* ctx, const mesh_radio_t* radio, uint8_t dst, const char* path,
                          const uint8_t* meta, uint8_t meta_len);

bool alfp_tx_active(const alfp_ctx_t* ctx);

/* Feed one packet that radio->recv() returned. */
void alfp_handle_packet(alfp_ctx_t* ctx, const mesh_radio_t* radio, uint8_t src, const uint8_t* buf,
                        uint16_t len);

/* Timers, pacing, hashing, verification. Call every few ms. */
void alfp_step(alfp_ctx_t* ctx);

/* Convenience: drain every radio's recv queue into alfp_handle_packet, then alfp_step. */
void alfp_service(alfp_ctx_t* ctx, const mesh_radio_t* const* radios, uint8_t n_radios);

/* Radio selection (once per transfer): first radio in preference order whose mesh has heard
 * the bridge within fresh_ms. Returns NULL when none has, so the capture stays queued on SD. */
const mesh_radio_t* alfp_select_radio(const mesh_radio_t* const* by_preference, uint8_t n, uint8_t bridge_id,
                                      uint32_t now_ms, uint32_t fresh_ms);

/* Size-based radio choice (additive; alfp_select_radio() above is unchanged and still used when
 * size_threshold is 0). Below size_threshold bytes, prefers the "lora" entry in by_preference and
 * falls back to "wifi" if LoRa's bridge entry is not fresh (within fresh_ms); at or above the
 * threshold, prefers "wifi" and falls back to "lora". Returns NULL if neither is fresh, same as
 * alfp_select_radio(). by_preference does not need "lora"/"wifi" in any particular order; radios
 * are matched by name. See docs/DESIGN.md "Radio choice by size". */
const mesh_radio_t* alfp_select_radio_for_size(const mesh_radio_t* const* by_preference, uint8_t n,
                                               uint8_t bridge_id, uint32_t now_ms, uint32_t fresh_ms,
                                               uint32_t size_bytes, uint32_t size_threshold);

/* True if chosen is the radio alfp_select_radio_for_size() would have preferred by size alone
 * (i.e. it was not a freshness fallback). Used only to log "reason=size" vs "reason=fallback". */
bool alfp_radio_was_size_preferred(const mesh_radio_t* const* by_preference, uint8_t n, uint32_t size_bytes,
                                   uint32_t size_threshold, const mesh_radio_t* chosen);

const char* alfp_tx_result_str(alfp_tx_result_t r);

#endif
