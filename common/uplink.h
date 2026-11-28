/*
 * uplink.h - bridge node -> laptop over USB serial.   OWNER: Member 2
 *
 * When the bridge has reassembled and verified a file, it streams it to the laptop as text so it
 * survives any serial terminal and can be mixed with ordinary log lines:
 *
 *   @ALFP FILE v1 src=3 sid=00042 size=8000 crc=1a2b3c4d radio=lora frags=34 first=1234567 done=1241670
 *         nacks=1 dups=2 meta=01030000...       (one line; first/done/nacks/dups added later, key=value,
 *                                                so older parsers just ignore them)
 *   @D <base64, 64 chars per line>
 *   ...
 *   @ALFP END crc=1a2b3c4d
 *
 * tools/bridge_receiver.py reads this, re-checks the CRC-32 and saves image + metadata JSON for the
 * backend. Portable C: the Pico passes a putchar-style function, the laptop tests pass a buffer.
 *
 * Two ways to send:
 *   uplink_send_file()  the whole file in one call (self-test / photo-test images, host tests).
 *   uplink_queue_*()    the node firmware: on_rx_done only copies the report into a small queue,
 *                       and the comms loop sends a few lines per pass (uplink_queue_service), so the
 *                       radios are still polled while a file goes to the laptop. Before this, the
 *                       whole file was streamed from inside on_rx_done (~200 SD reads and ~200 log
 *                       lines for one 9.6 KB image) and the bridge stopped polling both radios for
 *                       3-4 s every time a file finished. Both produce exactly the same text.
 */
#ifndef UPLINK_H
#define UPLINK_H

#include <stdbool.h>
#include <stdint.h>

#include "alfp.h"

typedef void (*uplink_write_fn)(const char* p_str, void* p_user);

/* Stream the finished file described by r. Returns 0 on success, -1 if the file cannot be read. */
int uplink_send_file(const alfp_storage_t* p_store, const alfp_rx_report_t* p_rep, uplink_write_fn p_out, void* p_user);

/* Bytes of file per "@D" line (64 base64 characters). */
#define UPLINK_LINE_BYTES 48u
/* Bytes read from the SD card at a time: 8 lines per read instead of 1. */
#define UPLINK_CHUNK_BYTES (8u * UPLINK_LINE_BYTES)
/* Files that can wait to go to the laptop. Twice the bridge's receive sessions, so even every
 * session finishing at once, twice over, before one file has gone out still fits. */
#ifndef UPLINK_QUEUE_DEPTH
#define UPLINK_QUEUE_DEPTH (2u * ALFP_RX_SESSIONS)
#endif

typedef struct
{
    /* a private copy of the alfp_rx_report_t fields the stream needs (the report's pointers are
     * only valid during on_rx_done) */
    uint8_t src;
    uint16_t sid;
    char radio[8];
    char path[ALFP_PATH_MAX + 8];
    uint32_t file_size;
    uint16_t frag_count;
    uint32_t file_crc32;
    uint8_t meta[ALFP_MAX_META];
    uint8_t meta_len;
    /* radio-side timing and repair counts, bridge clock (ms since boot) */
    uint32_t first_ms, done_ms;
    uint16_t nacks, dups;
} uplink_job_t;

typedef struct
{
    const alfp_storage_t* p_store;
    uplink_job_t jobs[UPLINK_QUEUE_DEPTH];
    uint8_t head, count;
    /* progress through jobs[head] */
    bool b_started;
    int handle;        /* jobs[head]'s file, open from its header line to its END line; -1 = none */
    uint32_t off; /* bytes of the file already sent as @D lines */
    uint32_t crc; /* running CRC-32 of those bytes */
    uint8_t buf[UPLINK_CHUNK_BYTES];
    uint16_t buf_len, buf_pos;
    /* counters for the 's' stats line */
    uint32_t sent_ok, sent_bad, queue_full;
    uint8_t max_waiting;
} uplink_queue_t;

void uplink_queue_init(uplink_queue_t* p_queue, const alfp_storage_t* p_store);

/* Copy r into the queue. Returns false (and counts it) if the queue is full; the caller should then
 * fall back to uplink_send_file() so the file is never lost. */
bool uplink_queue_add(uplink_queue_t* p_queue, const alfp_rx_report_t* p_rep);

/* Send at most max_lines lines of the waiting files (header, @D and END lines all count as one).
 * Returns the number of lines written; 0 when nothing is waiting or max_lines is 0. */
unsigned uplink_queue_service(uplink_queue_t* p_queue, unsigned max_lines, uplink_write_fn p_out, void* p_user);

/* Files waiting, including the one part-way through. */
unsigned uplink_queue_waiting(const uplink_queue_t* p_queue);

#endif
