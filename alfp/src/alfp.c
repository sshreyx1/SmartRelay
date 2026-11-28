/*
 * alfp.c - the sender and receiver state machines.
 *
 * Sender (field node), one transfer at a time:
 *   IDLE -> HASHING (CRC-32 of the file, 512 bytes per step)
 *        -> SENDING  (manifest, then every fragment in 'todo', then a POLL;
 *                    paced by the profile)
 *        -> WAITING  (for NACK/ACK; timeout re-sends just the POLL)
 *        -> NACK: todo = what the bridge is missing, round++, back to SENDING
 *        -> ACK(OK): done.   Too many rounds or unanswered polls: give up (file
 *                            stays on SD).
 *
 * Receiver (bridge), up to ALFP_RX_SESSIONS at once, keyed by (source node,
 * session id):
 *   first MANIFEST or DATA opens a session and a .part file on SD
 *   each DATA is CRC-checked, then written straight to SD at seq * frag_size
 *   (no RAM copy of the image)
 *   a bitmap records which fragments have arrived; duplicates are counted and
 *   ignored
 *   POLL is answered with a NACK built from the bitmap (list or bitmap,
 *   whichever is shorter)
 *   when every fragment is in: verify CRC-32 from SD, rename to inbox/<xx>/,
 *   ACK, remember the result
 */
#include "alfp.h"

#include <stdio.h>
#include <string.h>

#include "alfp_crc.h"

/* ---- small helpers
 * --------------------------------------------------------------------- */

static uint32_t now (alfp_ctx_t* p_ctx)
{
    return p_ctx->cfg.p_platform->p_now_ms(p_ctx->cfg.p_platform->p_ctx);
}

static bool reached (uint32_t t_now, uint32_t deadline)
{
    return (int32_t)(t_now - deadline) >= 0; /* wrap-safe */
}

static void logf_ (alfp_ctx_t* p_ctx, const char* p_msg)
{
    if (p_ctx->cfg.p_platform->p_log)
    {
        p_ctx->cfg.p_platform->p_log(p_ctx->cfg.p_platform->p_ctx, p_msg);
    }
}

static const alfp_storage_t* st (alfp_ctx_t* p_ctx)
{
    return p_ctx->cfg.p_storage;
}

/* Optional progress callbacks (alfp_cfg_t.on_rx_fail ...): report only, never
 * change state. */
static void ev_rx_fail (alfp_ctx_t* p_ctx, uint8_t src, uint16_t sid,
                        const char* p_why)
{
    if (p_ctx->cfg.p_on_rx_fail)
    {
        p_ctx->cfg.p_on_rx_fail(p_ctx, src, sid, p_why);
    }
}

static void bset (uint8_t* p_bitmap, uint16_t idx)
{
    p_bitmap[idx >> 3] = (uint8_t)(p_bitmap[idx >> 3] | (1u << (idx & 7u)));
}

static void bclr (uint8_t* p_bitmap, uint16_t idx)
{
    p_bitmap[idx >> 3] = (uint8_t)(p_bitmap[idx >> 3] & ~(1u << (idx & 7u)));
}

static bool bget (const uint8_t* p_bitmap, uint16_t idx)
{
    return (p_bitmap[idx >> 3] >> (idx & 7u)) & 1u;
}

static void bfill (uint8_t* p_bitmap, uint16_t chunk_len)
{
    memset(p_bitmap, 0, ALFP_BITMAP_BYTES);
    for (uint16_t idx = 0; idx < chunk_len; idx++)
    {
        bset(p_bitmap, idx);
    }
}

static uint16_t frag_len (uint32_t file_size, uint16_t frag_size, uint16_t seq)
{
    uint32_t off  = (uint32_t)seq * frag_size;
    uint32_t left = file_size - off;
    return (uint16_t)(left < frag_size ? left : frag_size);
}

static alfp_pace_t* pace_for(alfp_ctx_t* p_ctx, const mesh_radio_t* p_radio);
static void         pace_update(alfp_ctx_t* p_ctx, uint16_t missing);

/* ---- init
 * -----------------------------------------------------------------------------
 */

/* Follow-up F9: a transfer cut off by a reboot or reflash leaves its
 * "<tmp_dir>/nS_SSSSS.part" behind, and nothing else ever deletes it (the
 * session it belonged to is gone). At start-up no session is open, so every
 * .part file in tmp_dir is stale. Names are collected first and removed
 * afterwards, a batch at a time, so the directory is never changed while it is
 * being listed. */
#define STALE_BATCH 8u
typedef struct
{
    char     names[STALE_BATCH][24];
    unsigned len;
    bool     b_more;
} stale_list_t;

static void stale_part_cb (const char* p_name, void* p_user)
{
    stale_list_t* p_stale = (stale_list_t*)p_user;
    size_t        len     = strlen(p_name);
    if ((len < 6u) || (0 != strcmp(p_name + len - 5u, ".part")) ||
        (len >= sizeof(p_stale->names[0])))
    {
        return;
    }
    if (p_stale->len < STALE_BATCH)
    {
        memcpy(p_stale->names[p_stale->len++], p_name, len + 1u);
    }
    else
    {
        p_stale->b_more = true;
    }
}

static void remove_stale_parts (alfp_ctx_t* p_ctx)
{
    if ((!st(p_ctx)->p_list) || (!st(p_ctx)->p_remove))
    {
        return;
    }
    unsigned removed = 0;
    for (unsigned pass = 0; pass < 64u; pass++)
    {
        stale_list_t stale;
        memset(&stale, 0, sizeof(stale));
        st(p_ctx)->p_list(st(p_ctx)->p_ctx, p_ctx->cfg.p_tmp_dir, stale_part_cb,
                          &stale);
        unsigned gone = 0;
        for (unsigned idx = 0; idx < stale.len; idx++)
        {
            char path[ALFP_PATH_MAX + 24];
            snprintf(path, sizeof(path), "%s/%s", p_ctx->cfg.p_tmp_dir,
                     stale.names[idx]);
            if (st(p_ctx)->p_remove(st(p_ctx)->p_ctx, path) >= 0)
            {
                gone++;
            }
        }
        removed += gone;
        if ((!stale.b_more) || (0u == gone))
        {
            break;
        }
    }
    if (removed && (p_ctx->cfg.p_platform->p_log))
    {
        char line[64];
        snprintf(line, sizeof(line),
                 "alfp: removed %u stale .part file(s) from %s/", removed,
                 p_ctx->cfg.p_tmp_dir);
        p_ctx->cfg.p_platform->p_log(p_ctx->cfg.p_platform->p_ctx, line);
    }
}

void alfp_init (alfp_ctx_t* p_ctx, const alfp_cfg_t* p_cfg)
{
    memset(p_ctx, 0, sizeof(*p_ctx));
    p_ctx->cfg = *p_cfg;
    if (!p_ctx->cfg.p_inbox_dir)
    {
        p_ctx->cfg.p_inbox_dir = "inbox";
    }
    if (!p_ctx->cfg.p_tmp_dir)
    {
        p_ctx->cfg.p_tmp_dir = "rx";
    }
    p_ctx->tx_session.file_handle = -1;
    for (unsigned idx = 0; idx < ALFP_RX_SESSIONS; idx++)
    {
        p_ctx->rx_sessions[idx].file_handle = -1;
    }
    /* Random start so a rebooted node does not reuse a session id the bridge
     * still remembers. */
    p_ctx->next_sid =
        (uint16_t)p_cfg->p_platform->p_rand32(p_cfg->p_platform->p_ctx);
    if ((p_ctx->cfg.node_id == p_ctx->cfg.bridge_id) && (st(p_ctx)->p_mkdir))
    {
        st(p_ctx)->p_mkdir(st(p_ctx)->p_ctx, p_ctx->cfg.p_inbox_dir);
        st(p_ctx)->p_mkdir(st(p_ctx)->p_ctx, p_ctx->cfg.p_tmp_dir);
    }
    if (p_ctx->cfg.node_id == p_ctx->cfg.bridge_id)
    {
        remove_stale_parts(p_ctx);
    }
}

/* =============================================================================
 * SENDER
 * =============================================================================
 */

bool alfp_tx_active (const alfp_ctx_t* p_ctx)
{
    return TX_IDLE != p_ctx->tx_session.state;
}

static void tx_finish (alfp_ctx_t* p_ctx, alfp_tx_result_t res,
                       uint8_t ack_status)
{
    alfp_tx_t* p_xfer = &p_ctx->tx_session;
    if (p_xfer->file_handle >= 0)
    {
        st(p_ctx)->p_close(st(p_ctx)->p_ctx, p_xfer->file_handle);
        p_xfer->file_handle = -1;
    }
    p_xfer->rep.result     = res;
    p_xfer->rep.ack_status = ack_status;
    p_xfer->rep.rounds     = p_xfer->round;
    p_xfer->rep.gap_ms_end = p_xfer->gap_ms;
    p_xfer->rep.file_crc32 = TX_HASHING == p_xfer->state ? 0 : p_xfer->crc32;
    if ((ALFP_TX_DONE == res) && (1 == p_xfer->round) && (p_xfer->p_radio))
    {
        /* a clean first round: allow the gap to shrink */
        pace_update(p_ctx, 0);
    }
    p_xfer->rep.finished_ms = now(p_ctx);
    p_xfer->state           = TX_IDLE;
    if (ALFP_TX_DONE == res)
    {
        p_ctx->stats.tx_ok++;
    }
    else
    {
        p_ctx->stats.tx_failed++;
    }
    if (p_ctx->cfg.p_on_tx_done)
    {
        /* callback may start the next transfer */
        alfp_tx_report_t rep = p_xfer->rep;
        p_ctx->cfg.p_on_tx_done(p_ctx, &rep);
    }
}

alfp_err_t alfp_send_file (alfp_ctx_t* p_ctx, const mesh_radio_t* p_radio,
                           uint8_t dst, const char* p_path,
                           const uint8_t* p_meta, uint8_t meta_len)
{
    alfp_tx_t* p_xfer = &p_ctx->tx_session;
    if (TX_IDLE != p_xfer->state)
    {
        return ALFP_E_BUSY;
    }
    if ((!p_radio) || (!p_path) || (meta_len > ALFP_MAX_META) ||
        (meta_len && (!p_meta)) || (strlen(p_path) >= ALFP_PATH_MAX))
    {
        return ALFP_E_ARG;
    }
    uint16_t meta_path = p_radio->p_max_payload();
    if (meta_path > ALFP_MAX_PKT)
    {
        meta_path = ALFP_MAX_PKT;
    }
    if ((meta_path < ALFP_MANIFEST_OVERHEAD + meta_len) ||
        (meta_path <= ALFP_DATA_OVERHEAD))
    {
        return ALFP_E_ARG;
    }

    int file_handle = st(p_ctx)->p_open_read(st(p_ctx)->p_ctx, p_path);
    if (file_handle < 0)
    {
        return ALFP_E_OPEN;
    }
    int32_t size = st(p_ctx)->p_size(st(p_ctx)->p_ctx, file_handle);
    if (size <= 0)
    {
        st(p_ctx)->p_close(st(p_ctx)->p_ctx, file_handle);
        return size < 0 ? ALFP_E_OPEN : ALFP_E_EMPTY;
    }
    uint16_t fsize = (uint16_t)(meta_path - ALFP_DATA_OVERHEAD);
    uint32_t count = ((uint32_t)size + fsize - 1u) / fsize;
    if (count > ALFP_MAX_FRAGS)
    {
        st(p_ctx)->p_close(st(p_ctx)->p_ctx, file_handle);
        return ALFP_E_TOO_BIG;
    }

    memset(p_xfer, 0, sizeof(*p_xfer));
    p_xfer->p_radio     = p_radio;
    p_xfer->p_prof      = alfp_profile_for(p_ctx, p_radio);
    p_xfer->dst         = dst;
    p_xfer->file_handle = file_handle;
    strcpy(p_xfer->path, p_path);
    if (meta_len)
    {
        memcpy(p_xfer->meta, p_meta, meta_len);
    }
    p_xfer->meta_len   = meta_len;
    p_xfer->sid        = p_ctx->next_sid++;
    p_xfer->file_size  = (uint32_t)size;
    p_xfer->frag_size  = fsize;
    p_xfer->frag_count = (uint16_t)count;
    p_xfer->crc32      = ALFP_CRC32_INIT;
    p_xfer->hash_off   = 0;
    p_xfer->state      = TX_HASHING;
    {
        alfp_pace_t* p_pace = pace_for(p_ctx, p_radio);
        bool b_adaptive = p_xfer->p_prof->gap_max_ms > p_xfer->p_prof->gap_ms;
        p_xfer->gap_ms  = (b_adaptive && (p_pace->gap_ms))
                              ? p_pace->gap_ms
                              : p_xfer->p_prof->gap_ms;
    }

    p_xfer->rep.sid        = p_xfer->sid;
    p_xfer->rep.p_radio    = p_radio->p_name;
    p_xfer->rep.p_path     = p_xfer->path;
    p_xfer->rep.file_size  = p_xfer->file_size;
    p_xfer->rep.frag_count = p_xfer->frag_count;
    p_xfer->rep.frag_size  = p_xfer->frag_size;
    p_xfer->rep.started_ms = now(p_ctx);
    return ALFP_OK;
}

/* Try to hand one packet to the radio. Returns true if accepted. */
static alfp_pace_t* pace_for (alfp_ctx_t* p_ctx, const mesh_radio_t* p_radio)
{
    for (unsigned idx = 0; idx < 2; idx++)
    {
        if (p_ctx->pace[idx].p_radio == p_radio)
        {
            return &p_ctx->pace[idx];
        }
    }
    for (unsigned idx = 0; idx < 2; idx++)
    {
        if (!p_ctx->pace[idx].p_radio)
        {
            p_ctx->pace[idx].p_radio = p_radio;
            p_ctx->pace[idx].gap_ms  = 0;
            return &p_ctx->pace[idx];
        }
    }
    return &p_ctx->pace[0];
}

/* Adaptive pacing, applied once per round from the NACK:
 *   more than 25% of this round's fragments missing -> double the gap (flood
 *   colliding with itself)
 *   under 5% missing                                -> shrink it by a quarter
 * bounded by the profile's [gap_ms, gap_max_ms]. Random link loss also raises
 * it, which costs time but never correctness. */
static void pace_update (alfp_ctx_t* p_ctx, uint16_t missing)
{
    alfp_tx_t* p_xfer = &p_ctx->tx_session;
    if (!p_xfer->sent_round)
    {
        return;
    }
    uint32_t gap_low  = p_xfer->p_prof->gap_ms;
    uint32_t gap_high = p_xfer->p_prof->gap_max_ms > gap_low
                            ? p_xfer->p_prof->gap_max_ms
                            : gap_low;
    uint32_t loss_pct = (uint32_t)missing * 100u / p_xfer->sent_round;
    if (loss_pct > 25u)
    {
        p_xfer->gap_ms =
            p_xfer->gap_ms * 2u > gap_high ? gap_high : p_xfer->gap_ms * 2u;
    }
    else if (loss_pct < 5u)
    {
        p_xfer->gap_ms = p_xfer->gap_ms - p_xfer->gap_ms / 4u < gap_low
                             ? gap_low
                             : p_xfer->gap_ms - p_xfer->gap_ms / 4u;
    }
    else
    {
        /* 5-25% loss: keep the current gap. */
    }
    pace_for(p_ctx, p_xfer->p_radio)->gap_ms = p_xfer->gap_ms;
}

static bool tx_put (alfp_ctx_t* p_ctx, const uint8_t* p_pkt, uint16_t len)
{
    alfp_tx_t* p_xfer = &p_ctx->tx_session;
    if (!p_xfer->p_radio->p_send(p_xfer->dst, p_pkt, len))
    {
        p_ctx->stats.send_blocked++;
        p_xfer->next_tx_ms = now(p_ctx) + 2u;
        return false;
    }
    p_xfer->rep.bytes_on_air += len;
    /* wait for tx_busy() to clear, then the profile gap */
    p_xfer->b_was_busy = true;
    return true;
}

static void tx_step (alfp_ctx_t* p_ctx)
{
    alfp_tx_t* p_xfer = &p_ctx->tx_session;
    uint32_t   tnow   = now(p_ctx);

    switch (p_xfer->state)
    {
        case TX_IDLE:
            return;

        case TX_HASHING:
        {
            uint32_t left = p_xfer->file_size - p_xfer->hash_off;
            uint16_t chunk_len =
                (uint16_t)(left < ALFP_HASH_CHUNK ? left : ALFP_HASH_CHUNK);
            int got = st(p_ctx)->p_read_at(
                st(p_ctx)->p_ctx, p_xfer->file_handle, p_xfer->hash_off,
                p_ctx->io_buf, chunk_len);
            if (got != (int)chunk_len)
            {
                tx_finish(p_ctx, ALFP_TX_IO_ERROR, 0);
                return;
            }
            p_xfer->crc32 = alfp_crc32(p_xfer->crc32, p_ctx->io_buf, chunk_len);
            p_xfer->hash_off += chunk_len;
            if (p_xfer->hash_off >= p_xfer->file_size)
            {
                bfill(p_xfer->todo, p_xfer->frag_count);
                p_xfer->todo_next       = 0;
                p_xfer->b_send_manifest = true;
                p_xfer->polls_left      = p_xfer->p_prof->poll_copies
                                              ? p_xfer->p_prof->poll_copies
                                              : 1u;
                p_xfer->copies          = 1;
                p_xfer->round           = 1;
                p_xfer->state           = TX_SENDING;
                p_xfer->next_tx_ms      = tnow;
                if (p_ctx->cfg.p_on_tx_start)
                {
                    p_ctx->cfg.p_on_tx_start(p_ctx, p_xfer);
                }
            }
            return;
        }

        case TX_WAITING:
            if (reached(tnow, p_xfer->wait_until_ms))
            {
                p_xfer->polls_unanswered++;
                if (p_xfer->polls_unanswered >= p_xfer->p_prof->max_polls)
                {
                    p_xfer->rep.why = 1;
                    tx_finish(p_ctx, ALFP_TX_GAVE_UP, 0);
                    return;
                }
                /* The POLL or its answer was lost: ask again (same round). */
                p_xfer->polls_left = p_xfer->p_prof->poll_copies
                                         ? p_xfer->p_prof->poll_copies
                                         : 1u;
                p_xfer->state      = TX_SENDING;
            }
            return;

        case TX_SENDING:
            break;

        default:
            /* t->state only ever holds the TX_* states above; carry on
             * sending. */
            break;
    }

    /* Pacing: wait until the radio has finished, then the profile's gap. */
    if (p_xfer->b_was_busy)
    {
        if (p_xfer->p_radio->p_tx_busy())
        {
            return;
        }
        p_xfer->b_was_busy = false;
        p_xfer->next_tx_ms = tnow + p_xfer->gap_ms;
    }
    if ((!reached(tnow, p_xfer->next_tx_ms)) || (p_xfer->p_radio->p_tx_busy()))
    {
        return;
    }

    if (p_xfer->b_send_manifest)
    {
        alfp_manifest_t manifest = {
            .file_size  = p_xfer->file_size,
            .frag_count = p_xfer->frag_count,
            .frag_size  = p_xfer->frag_size,
            .file_crc32 = p_xfer->crc32,
            .radio_id   = p_xfer->p_prof->radio_id,
            .meta_len   = p_xfer->meta_len,
            .p_meta     = p_xfer->meta,
        };
        uint16_t len = alfp_enc_manifest(p_ctx->pkt, sizeof(p_ctx->pkt),
                                         p_xfer->sid, &manifest);
        if (tx_put(p_ctx, p_ctx->pkt, len))
        {
            p_xfer->b_send_manifest = false;
            p_xfer->rep.manifests_sent++;
        }
        return;
    }

    /* Next fragment still to send this round. */
    while ((p_xfer->todo_next < p_xfer->frag_count) &&
           (!bget(p_xfer->todo, p_xfer->todo_next)))
    {
        p_xfer->todo_next++;
    }
    if (p_xfer->todo_next < p_xfer->frag_count)
    {
        uint16_t seq = p_xfer->todo_next;
        uint16_t chunk_len =
            frag_len(p_xfer->file_size, p_xfer->frag_size, seq);
        uint16_t hdr_len = alfp_enc_data_header(p_ctx->pkt, p_xfer->sid, seq,
                                                p_xfer->frag_count);
        int got = st(p_ctx)->p_read_at(st(p_ctx)->p_ctx, p_xfer->file_handle,
                                       (uint32_t)seq * p_xfer->frag_size,
                                       p_ctx->pkt + hdr_len, chunk_len);
        if (got != (int)chunk_len)
        {
            tx_finish(p_ctx, ALFP_TX_IO_ERROR, 0);
            return;
        }
        uint16_t len = alfp_enc_data_finish(p_ctx->pkt, chunk_len);
        if (tx_put(p_ctx, p_ctx->pkt, len))
        {
            p_xfer->rep.data_sent++;
            p_xfer->sent_round++;
            if (p_xfer->round > 1)
            {
                p_xfer->rep.data_resent++;
            }
            if (p_xfer->copy_left)
            {
                p_xfer->copy_left--; /* this was the extra copy */
            }
            else if (p_xfer->copies > 1)
            {
                p_xfer->copy_left = (uint8_t)(p_xfer->copies - 1u);
            }
            else
            {
                /* single copy per fragment: copy_left stays 0 */
            }
            if (!p_xfer->copy_left)
            {
                bclr(p_xfer->todo, seq);
                p_xfer->todo_next++;
            }
        }
        return;
    }

    if (p_xfer->polls_left)
    {
        uint16_t len = alfp_enc_poll(p_ctx->pkt, sizeof(p_ctx->pkt),
                                     p_xfer->sid, p_xfer->round);
        if (tx_put(p_ctx, p_ctx->pkt, len))
        {
            p_xfer->polls_left--;
            p_xfer->rep.polls_sent++;
            if (!p_xfer->polls_left)
            {
                p_xfer->state         = TX_WAITING;
                p_xfer->wait_until_ms = tnow + p_xfer->p_prof->poll_timeout_ms;
            }
        }
        return;
    }

    /* Nothing left to send and no poll pending: wait for an answer. */
    p_xfer->state         = TX_WAITING;
    p_xfer->wait_until_ms = tnow + p_xfer->p_prof->poll_timeout_ms;
}

static void mark_todo (uint16_t seq, void* p_user)
{
    alfp_tx_t* p_xfer = (alfp_tx_t*)p_user;
    bset(p_xfer->todo, seq);
}

static void count_seq (uint16_t seq, void* p_user)
{
    (void)seq;
    (*(uint16_t*)p_user)++;
}

static bool next_round (alfp_ctx_t* p_ctx)
{
    alfp_tx_t* p_xfer = &p_ctx->tx_session;
    if (p_xfer->round >= p_xfer->p_prof->max_rounds)
    {
        p_xfer->rep.why = 3;
        tx_finish(p_ctx, ALFP_TX_GAVE_UP, 0);
        return false;
    }
    p_xfer->round++;
    return true;
}

static void tx_on_nack (alfp_ctx_t* p_ctx, const alfp_pkt_t* p_pkt)
{
    alfp_tx_t*         p_xfer = &p_ctx->tx_session;
    const alfp_nack_t* p_nack = &p_pkt->body.nack;
    if ((TX_WAITING != p_xfer->state) || (p_nack->round != p_xfer->round))
    {
        /* stale or duplicate answer (e.g. two copies through the flood) */
        return;
    }
    p_xfer->polls_unanswered = 0;
    p_xfer->rep.nacks_received++;

    if ((ALFP_NACK_FMT_NONE == p_nack->fmt) &&
        (!(p_nack->flags & ALFP_NACK_NEED_MANIFEST)))
    {
        /* Bridge has every fragment and is verifying: wait for its ACK. */
        p_xfer->wait_until_ms = now(p_ctx) + p_xfer->p_prof->poll_timeout_ms;
        return;
    }
    /* Progress check: a round that recovered nothing counts towards giving
     * up. */
    uint16_t missing = 0;
    alfp_nack_for_each(p_nack, p_xfer->frag_count, count_seq, &missing);
    if (p_ctx->cfg.p_on_tx_nack)
    {
        p_ctx->cfg.p_on_tx_nack(p_ctx, p_xfer->sid, p_xfer->round, missing);
    }
    if ((p_xfer->round > 1) && (missing >= p_xfer->last_missing))
    {
        if (++p_xfer->stalled >= p_xfer->p_prof->max_stalled)
        {
            p_xfer->rep.why = 2;
            tx_finish(p_ctx, ALFP_TX_GAVE_UP, 0);
            return;
        }
    }
    else
    {
        p_xfer->stalled = 0;
    }
    p_xfer->last_missing = missing;
    pace_update(p_ctx, missing);
    p_xfer->sent_round = 0;
    if (!next_round(p_ctx))
    {
        return;
    }
    /* Tail: with only a few fragments left, a second copy of each saves whole
     * rounds. */
    p_xfer->copies =
        ((p_xfer->p_prof->tail_dup) && (missing <= p_xfer->p_prof->tail_dup))
            ? 2u
            : 1u;
    p_xfer->copy_left = 0;
    memset(p_xfer->todo, 0, sizeof(p_xfer->todo));
    if (p_ctx->cfg.b_baseline_resend_all)
    {
        bfill(p_xfer->todo, p_xfer->frag_count);
    }
    else
    {
        alfp_nack_for_each(p_nack, p_xfer->frag_count, mark_todo, p_xfer);
    }
    p_xfer->todo_next       = 0;
    p_xfer->b_send_manifest = (p_nack->flags & ALFP_NACK_NEED_MANIFEST) ||
                              (ALFP_NACK_FMT_ALL == p_nack->fmt);
    p_xfer->polls_left =
        p_xfer->p_prof->poll_copies ? p_xfer->p_prof->poll_copies : 1u;
    p_xfer->state = TX_SENDING;
}

static void tx_on_ack (alfp_ctx_t* p_ctx, const alfp_pkt_t* p_pkt)
{
    alfp_tx_t* p_xfer = &p_ctx->tx_session;
    if ((TX_IDLE == p_xfer->state) || (TX_HASHING == p_xfer->state))
    {
        return;
    }
    switch (p_pkt->body.status)
    {
        case ALFP_ACK_OK:
            tx_finish(p_ctx, ALFP_TX_DONE, ALFP_ACK_OK);
            return;
        case ALFP_ACK_CRC_FAIL:
            /* Every fragment passed its CRC-16 but the file CRC-32 did not
             * match: send it all again. */
            if (!next_round(p_ctx))
            {
                return;
            }
            bfill(p_xfer->todo, p_xfer->frag_count);
            p_xfer->todo_next = 0;
            p_xfer->polls_left =
                p_xfer->p_prof->poll_copies ? p_xfer->p_prof->poll_copies : 1u;
            p_xfer->state = TX_SENDING;
            return;
        default:
            tx_finish(p_ctx, ALFP_TX_REFUSED, p_pkt->body.status);
            return;
    }
}

/* =============================================================================
 * RECEIVER (bridge)
 * =============================================================================
 */

static alfp_rx_t* rx_find (alfp_ctx_t* p_ctx, uint8_t src, uint16_t sid)
{
    for (unsigned idx = 0; idx < ALFP_RX_SESSIONS; idx++)
    {
        alfp_rx_t* p_session = &p_ctx->rx_sessions[idx];
        if ((RX_FREE != p_session->state) && (p_session->src == src) &&
            (p_session->sid == sid))
        {
            return p_session;
        }
    }
    return NULL;
}

static const alfp_done_t* done_find (alfp_ctx_t* p_ctx, uint8_t src,
                                     uint16_t sid)
{
    for (unsigned idx = 0; idx < ALFP_DONE_MEMORY; idx++)
    {
        if ((p_ctx->done[idx].b_used) && (p_ctx->done[idx].src == src) &&
            (p_ctx->done[idx].sid == sid))
        {
            return &p_ctx->done[idx];
        }
    }
    return NULL;
}

static void done_add (alfp_ctx_t* p_ctx, uint8_t src, uint16_t sid,
                      uint8_t status)
{
    alfp_done_t* p_done = &p_ctx->done[p_ctx->done_next];
    p_ctx->done_next    = (uint8_t)((p_ctx->done_next + 1u) % ALFP_DONE_MEMORY);
    p_done->b_used      = true;
    p_done->src         = src;
    p_done->sid         = sid;
    p_done->status      = status;
}

/* Every refusal is logged with its reason, so a refusing bridge says why
 * instead of only the sender seeing "refused". */
static void log_refusal (alfp_ctx_t* p_ctx, uint8_t src, uint16_t sid,
                         uint8_t status, const char* p_why)
{
    const alfp_platform_t* p_plat = p_ctx->cfg.p_platform;
    if ((NULL != p_plat) && (NULL != p_plat->p_log))
    {
        char line[128];
        snprintf(line, sizeof(line),
                 "alfp: refused node %u sid %05u, status %u: %s", (unsigned)src,
                 (unsigned)sid, (unsigned)status, p_why);
        p_plat->p_log(p_plat->p_ctx, line);
    }
}

/* Best-effort reply outside any session (unknown session, remembered
 * result). */
static void reply_ack (alfp_ctx_t* p_ctx, const mesh_radio_t* p_radio,
                       uint8_t dst, uint16_t sid, uint8_t status)
{
    uint8_t  pkt[ALFP_ACK_LEN];
    uint16_t len = alfp_enc_ack(pkt, sizeof(pkt), sid, status);
    if (!p_radio->p_send(dst, pkt, len))
    {
        p_ctx->stats.send_blocked++;
    }
}

static void rx_free (alfp_ctx_t* p_ctx, alfp_rx_t* p_session, bool b_delete_tmp)
{
    if (p_session->file_handle >= 0)
    {
        st(p_ctx)->p_close(st(p_ctx)->p_ctx, p_session->file_handle);
        p_session->file_handle = -1;
    }
    if (b_delete_tmp && (p_session->tmp_path[0]))
    {
        st(p_ctx)->p_remove(st(p_ctx)->p_ctx, p_session->tmp_path);
    }
    p_session->state = RX_FREE;
}

static alfp_rx_t* rx_open (alfp_ctx_t* p_ctx, const mesh_radio_t* p_radio,
                           uint8_t src, uint16_t sid, uint8_t* p_refuse)
{
    alfp_rx_t* p_slot = NULL;
    for (unsigned idx = 0; (idx < ALFP_RX_SESSIONS) && (!p_slot); idx++)
    {
        if (RX_FREE == p_ctx->rx_sessions[idx].state)
        {
            p_slot = &p_ctx->rx_sessions[idx];
        }
    }
    if (!p_slot)
    {
        /* Evict the most idle ACTIVE session if it has been quiet for over half
         * its expiry. */
        uint32_t tnow  = now(p_ctx);
        uint32_t worst = 0;
        for (unsigned idx = 0; idx < ALFP_RX_SESSIONS; idx++)
        {
            alfp_rx_t* p_session = &p_ctx->rx_sessions[idx];
            uint32_t   idle      = tnow - p_session->last_ms;
            if ((RX_ACTIVE == p_session->state) &&
                (idle > p_session->p_prof->rx_expiry_ms / 2u) && (idle > worst))
            {
                worst  = idle;
                p_slot = p_session;
            }
        }
        if (!p_slot)
        {
            *p_refuse = ALFP_ACK_BUSY;
            p_ctx->stats.rx_refused_busy++;
            log_refusal(p_ctx, src, sid, ALFP_ACK_BUSY,
                        "all receive sessions busy");
            ev_rx_fail(p_ctx, src, sid, "busy");
            return NULL;
        }
        rx_free(p_ctx, p_slot, true);
        p_ctx->stats.rx_expired++;
        ev_rx_fail(p_ctx, p_slot->src, p_slot->sid, "evicted");
    }

    memset(p_slot, 0, sizeof(*p_slot));
    p_slot->file_handle = -1;
    p_slot->p_radio     = p_radio;
    p_slot->p_prof      = alfp_profile_for(p_ctx, p_radio);
    p_slot->src         = src;
    p_slot->sid         = sid;
    snprintf(p_slot->tmp_path, sizeof(p_slot->tmp_path), "%s/n%u_%05u.part",
             p_ctx->cfg.p_tmp_dir, (unsigned)src, (unsigned)sid);
    p_slot->file_handle =
        st(p_ctx)->p_open_write(st(p_ctx)->p_ctx, p_slot->tmp_path);
    if (p_slot->file_handle < 0)
    {
        char why[ALFP_PATH_MAX + 24];
        snprintf(why, sizeof(why), "cannot create %s", p_slot->tmp_path);
        log_refusal(p_ctx, src, sid, ALFP_ACK_STORAGE, why);
        p_slot->tmp_path[0] = 0;
        *p_refuse           = ALFP_ACK_STORAGE;
        ev_rx_fail(p_ctx, src, sid, "storage");
        return NULL;
    }
    p_slot->state        = RX_ACTIVE;
    p_slot->last_ms      = now(p_ctx);
    p_slot->rep.src      = src;
    p_slot->rep.sid      = sid;
    p_slot->rep.p_radio  = p_radio->p_name;
    p_slot->rep.first_ms = p_slot->last_ms;
    if (p_ctx->cfg.p_on_rx_start)
    {
        p_ctx->cfg.p_on_rx_start(p_ctx, src, sid, p_radio->p_name);
    }
    return p_slot;
}

static void rx_reset_data (alfp_rx_t* p_session)
{
    memset(p_session->have, 0, sizeof(p_session->have));
    p_session->n_have = 0;
}

static void rx_maybe_complete (alfp_ctx_t* p_ctx, alfp_rx_t* p_session)
{
    (void)p_ctx;
    if ((RX_ACTIVE == p_session->state) && (p_session->b_have_manifest) &&
        (p_session->n_have == p_session->frag_count))
    {
        p_session->state          = RX_VERIFYING;
        p_session->verify_off     = 0;
        p_session->verify_crc     = ALFP_CRC32_INIT;
        p_session->b_pending_nack = false;
    }
}

static void rx_adopt_manifest (alfp_ctx_t* p_ctx, alfp_rx_t* p_session,
                               const alfp_manifest_t* p_manifest)
{
    if ((p_session->n_have) &&
        ((p_manifest->frag_size != p_session->frag_size) ||
         (p_manifest->frag_count != p_session->frag_count)))
    {
        /* provisional layout from an early DATA was wrong: start clean */
        rx_reset_data(p_session);
    }
    p_session->manifest = *p_manifest;
    memcpy(p_session->meta, p_manifest->p_meta, p_manifest->meta_len);
    p_session->manifest.p_meta = p_session->meta;
    p_session->frag_size       = p_manifest->frag_size;
    p_session->frag_count      = p_manifest->frag_count;
    p_session->b_have_manifest = true;
    if (p_ctx->cfg.p_on_rx_manifest)
    {
        p_ctx->cfg.p_on_rx_manifest(
            p_ctx, p_session->src, p_session->sid, p_session->frag_count,
            p_session->frag_size, p_session->manifest.file_size,
            p_session->manifest.file_crc32, p_session->meta,
            p_session->manifest.meta_len);
    }
}

static void rx_on_manifest (alfp_ctx_t* p_ctx, const mesh_radio_t* p_radio,
                            uint8_t src, const alfp_pkt_t* p_pkt)
{
    const alfp_manifest_t* p_manifest = &p_pkt->body.manifest;
    const alfp_done_t*     p_done     = done_find(p_ctx, src, p_pkt->sid);
    if (p_done)
    {
        if (ALFP_ACK_OK != p_done->status)
        {
            log_refusal(p_ctx, src, p_pkt->sid, p_done->status,
                        "same session refused earlier (remembered result)");
        }
        reply_ack(p_ctx, p_radio, src, p_pkt->sid, p_done->status);
        return;
    }
    alfp_rx_t* p_session = rx_find(p_ctx, src, p_pkt->sid);
    if (!p_session)
    {
        if (p_manifest->frag_count > ALFP_MAX_FRAGS)
        {
            log_refusal(p_ctx, src, p_pkt->sid, ALFP_ACK_TOO_BIG,
                        "too many fragments");
            ev_rx_fail(p_ctx, src, p_pkt->sid, "too_big");
            reply_ack(p_ctx, p_radio, src, p_pkt->sid, ALFP_ACK_TOO_BIG);
            return;
        }
        uint8_t refuse = 0;
        p_session      = rx_open(p_ctx, p_radio, src, p_pkt->sid, &refuse);
        if (!p_session)
        {
            reply_ack(p_ctx, p_radio, src, p_pkt->sid, refuse);
            return;
        }
    }
    p_session->last_ms = now(p_ctx);
    if (!p_session->b_have_manifest)
    {
        rx_adopt_manifest(p_ctx, p_session, p_manifest);
        rx_maybe_complete(p_ctx, p_session);
    }
}

static void rx_on_data (alfp_ctx_t* p_ctx, const mesh_radio_t* p_radio,
                        uint8_t src, const alfp_pkt_t* p_pkt)
{
    const alfp_data_t* p_data_pkt = &p_pkt->body.data;
    if (done_find(p_ctx, src, p_pkt->sid))
    {
        /* stray fragment of a finished transfer; the next POLL gets the ACK */
        return;
    }
    if (p_data_pkt->frag_count > ALFP_MAX_FRAGS)
    {
        p_ctx->stats.pkts_bad_other++;
        return;
    }
    alfp_rx_t* p_session = rx_find(p_ctx, src, p_pkt->sid);
    if (!p_session)
    {
        uint8_t refuse = 0;
        p_session      = rx_open(p_ctx, p_radio, src, p_pkt->sid, &refuse);
        if (!p_session)
        {
            return; /* the sender's POLL will get the refusal */
        }
        p_ctx->stats.data_orphan++;
    }
    if (RX_ACTIVE != p_session->state)
    {
        return;
    }
    if ((!p_session->b_have_manifest) && (0 == p_session->n_have))
    {
        /* DATA before MANIFEST: infer the layout. Every fragment but the last
         * is full size. */
        p_session->frag_count = p_data_pkt->frag_count;
        if (p_data_pkt->seq + 1u < p_data_pkt->frag_count)
        {
            p_session->frag_size = p_data_pkt->len;
        }
        else
        {
            uint16_t meta_path = p_radio->p_max_payload();
            p_session->frag_size =
                (uint16_t)((meta_path > ALFP_MAX_PKT ? ALFP_MAX_PKT
                                                     : meta_path) -
                           ALFP_DATA_OVERHEAD);
        }
    }
    if (p_data_pkt->frag_count != p_session->frag_count)
    {
        p_ctx->stats.pkts_bad_other++;
        return;
    }
    /* Length check: all but the last are full; the last is whatever remains. */
    if (p_session->b_have_manifest)
    {
        if (p_data_pkt->len != frag_len(p_session->manifest.file_size,
                                        p_session->frag_size, p_data_pkt->seq))
        {
            p_ctx->stats.pkts_bad_other++;
            return;
        }
    }
    else if (p_data_pkt->seq + 1u < p_data_pkt->frag_count
                 ? p_data_pkt->len != p_session->frag_size
                 : p_data_pkt->len > p_session->frag_size)
    {
        p_ctx->stats.pkts_bad_other++;
        return;
    }
    else
    {
        /* fragment length is valid: store it below */
    }
    p_session->last_ms = now(p_ctx);
    if (bget(p_session->have, p_data_pkt->seq))
    {
        p_ctx->stats.data_dup++;
        p_session->rep.duplicates++;
        if (p_ctx->cfg.p_on_rx_frag)
        {
            p_ctx->cfg.p_on_rx_frag(p_ctx, src, p_pkt->sid, p_data_pkt->seq,
                                    (uint8_t)p_session->rep.nacks_sent, true);
        }
        return;
    }
    int written =
        st(p_ctx)->p_write_at(st(p_ctx)->p_ctx, p_session->file_handle,
                              (uint32_t)p_data_pkt->seq * p_session->frag_size,
                              p_data_pkt->p_payload, p_data_pkt->len);
    if (written != (int)p_data_pkt->len)
    {
        logf_(p_ctx, "alfp: SD write failed");
        return; /* stays missing; will be re-requested */
    }
    bset(p_session->have, p_data_pkt->seq);
    p_session->n_have++;
    if (p_ctx->cfg.p_on_rx_frag)
    {
        p_ctx->cfg.p_on_rx_frag(p_ctx, src, p_pkt->sid, p_data_pkt->seq,
                                (uint8_t)p_session->rep.nacks_sent, false);
    }
    rx_maybe_complete(p_ctx, p_session);
}

static void rx_try_send_nack (alfp_ctx_t* p_ctx, alfp_rx_t* p_session)
{
    uint8_t flags =
        (uint8_t)(p_session->b_have_manifest ? 0u : ALFP_NACK_NEED_MANIFEST);
    uint16_t cap = p_session->p_radio->p_max_payload();
    if (cap > sizeof(p_ctx->pkt))
    {
        cap = sizeof(p_ctx->pkt);
    }
    uint16_t len = alfp_enc_nack(
        p_ctx->pkt, cap, p_session->sid, flags, p_session->pending_round,
        p_session->have, p_session->frag_count, 0 == p_session->frag_count);
    if (p_session->p_radio->p_send(p_session->src, p_ctx->pkt, len))
    {
        p_session->b_pending_nack = false;
        p_session->rep.nacks_sent++;
        if (p_ctx->cfg.p_on_rx_nack)
        {
            p_ctx->cfg.p_on_rx_nack(
                p_ctx, p_session->src, p_session->sid,
                (uint8_t)p_session->rep.nacks_sent,
                (uint16_t)(p_session->frag_count - p_session->n_have));
        }
    }
    else
    {
        p_ctx->stats.send_blocked++;
    }
}

static void rx_on_poll (alfp_ctx_t* p_ctx, const mesh_radio_t* p_radio,
                        uint8_t src, const alfp_pkt_t* p_pkt)
{
    const alfp_done_t* p_done = done_find(p_ctx, src, p_pkt->sid);
    if (p_done)
    {
        /* our ACK was lost: say it again */
        reply_ack(p_ctx, p_radio, src, p_pkt->sid, p_done->status);
        return;
    }
    alfp_rx_t* p_session = rx_find(p_ctx, src, p_pkt->sid);
    if (!p_session)
    {
        /* Everything so far was lost (or we rebooted): ask for manifest + all
         * data. */
        uint8_t  pkt[ALFP_NACK_MIN];
        uint16_t len =
            alfp_enc_nack(pkt, sizeof(pkt), p_pkt->sid, ALFP_NACK_NEED_MANIFEST,
                          p_pkt->body.round, NULL, 0, true);
        if (!p_radio->p_send(src, pkt, len))
        {
            p_ctx->stats.send_blocked++;
        }
        return;
    }
    p_session->last_ms = now(p_ctx);
    if (RX_VERIFYING == p_session->state)
    {
        return; /* ACK follows shortly */
    }
    if (p_session->b_pending_ack)
    {
        return;
    }
    p_session->b_pending_nack = true;
    p_session->pending_round  = p_pkt->body.round;
    rx_try_send_nack(p_ctx, p_session);
}

/* Where a finished file goes. Two problems with the old flat
 * "inbox/n<src>_<sid>.bin":
 *  - Follow-up F3: the session id is 16 bits and starts at a random value every
 *  boot, so on a card
 *    used long enough a new name can meet an older file, and the storage rename
 *    replaces its
 *    target. Now the first free name of n<src>_<sid>.bin, n<src>_<sid>_1.bin
 *    ... _99 is used (the
 *    .met sidecar takes the same stem).
 *  - Bridge freeze: one flat folder holding every file ever received. FatFs
 *  looks names up by
 *    reading the whole folder, and a long name (the old ".meta") needs up to ~6
 *    more passes to pick
 *    a unique 8.3 alias, so the create and rename here took up to 2.8 s each on
 *    node 1's card
 *    (SPI at 1 MHz, ~6 ms per 512-byte sector), all inside alfp_service() with
 *    both radios
 *    unpolled. Files are now spread over 256 subfolders by the session id's
 *    high byte
 *    ("rcv/7f/"), so a folder holds at most 256 ids per sender, and both names
 *    are plain 8.3
 *    ("n2_32561.bin" + "n2_32561.met"), so each create is one pass over a small
 *    folder. */
#define INBOX_SUFFIX_MAX 99u

static bool path_exists (alfp_ctx_t* p_ctx, const char* p_path)
{
    int handle = st(p_ctx)->p_open_read(st(p_ctx)->p_ctx, p_path);
    if (handle < 0)
    {
        return false;
    }
    st(p_ctx)->p_close(st(p_ctx)->p_ctx, handle);
    return true;
}

static void inbox_stem (alfp_ctx_t* p_ctx, const alfp_rx_t* p_session,
                        char* p_stem, size_t cap)
{
    unsigned bucket = (unsigned)(p_session->sid >> 8);
    char     dir[ALFP_PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s/%02x", p_ctx->cfg.p_inbox_dir, bucket) <
        0)
    {
        dir[0] = 0;
    }
    if (!(p_ctx->inbox_made[bucket / 8u] & (1u << (bucket % 8u))))
    {
        if (st(p_ctx)->p_mkdir)
        {
            st(p_ctx)->p_mkdir(st(p_ctx)->p_ctx, dir);
        }
        p_ctx->inbox_made[bucket / 8u] =
            (uint8_t)(p_ctx->inbox_made[bucket / 8u] | (1u << (bucket % 8u)));
    }
    /* (return values checked: a too-long inbox_dir just gives a truncated name,
     * never an overflow) */
    int  len = snprintf(p_stem, cap, "%s/n%u_%05u", dir,
                        (unsigned)p_session->src, (unsigned)p_session->sid);
    char probe[ALFP_PATH_MAX + 8];
    len = len < 0 ? len : snprintf(probe, sizeof(probe), "%s.bin", p_stem);
    for (unsigned kdx = 1;
         (len >= 0) && (kdx <= INBOX_SUFFIX_MAX) && (path_exists(p_ctx, probe));
         kdx++)
    {
        len = snprintf(p_stem, cap, "%s/n%u_%05u_%u", dir,
                       (unsigned)p_session->src, (unsigned)p_session->sid, kdx);
        len = len < 0 ? len : snprintf(probe, sizeof(probe), "%s.bin", p_stem);
    }
}

static void rx_finish_ok (alfp_ctx_t* p_ctx, alfp_rx_t* p_session)
{
    char final_path[ALFP_PATH_MAX + 8];
    char stem[ALFP_PATH_MAX];
    st(p_ctx)->p_close(st(p_ctx)->p_ctx, p_session->file_handle);
    p_session->file_handle = -1;
    inbox_stem(p_ctx, p_session, stem, sizeof(stem));
    snprintf(final_path, sizeof(final_path), "%s.bin", stem);
    bool b_is_ok = st(p_ctx)->p_rename(st(p_ctx)->p_ctx, p_session->tmp_path,
                                       final_path) >= 0;
    if (b_is_ok && (p_session->manifest.meta_len))
    {
        char meta_path[ALFP_PATH_MAX + 8];
        snprintf(meta_path, sizeof(meta_path), "%s.met", stem);
        int meta_handle = st(p_ctx)->p_open_write(st(p_ctx)->p_ctx, meta_path);
        if (meta_handle >= 0)
        {
            st(p_ctx)->p_write_at(st(p_ctx)->p_ctx, meta_handle, 0,
                                  p_session->meta,
                                  p_session->manifest.meta_len);
            st(p_ctx)->p_close(st(p_ctx)->p_ctx, meta_handle);
        }
    }
    uint8_t status = b_is_ok ? ALFP_ACK_OK : ALFP_ACK_STORAGE;
    if (!b_is_ok)
    {
        /* "cannot rename to " + final_path (ALFP_PATH_MAX + 8) */
        char why[ALFP_PATH_MAX + 32];
        snprintf(why, sizeof(why), "cannot rename to %s", final_path);
        log_refusal(p_ctx, p_session->src, p_session->sid, status, why);
        ev_rx_fail(p_ctx, p_session->src, p_session->sid, "storage");
    }
    done_add(p_ctx, p_session->src, p_session->sid, status);
    p_session->tmp_path[0] = 0;

    if (b_is_ok)
    {
        p_ctx->stats.rx_ok++;
        p_session->rep.p_path             = final_path;
        p_session->rep.file_size          = p_session->manifest.file_size;
        p_session->rep.frag_count         = p_session->frag_count;
        p_session->rep.file_crc32         = p_session->manifest.file_crc32;
        p_session->rep.p_meta             = p_session->meta;
        p_session->rep.meta_len           = p_session->manifest.meta_len;
        p_session->rep.fragments_received = p_session->n_have;
        p_session->rep.done_ms            = now(p_ctx);
        if (p_ctx->cfg.p_on_rx_done)
        {
            p_ctx->cfg.p_on_rx_done(p_ctx, &p_session->rep);
        }
        p_session->rep.p_path = NULL; /* final_path is on this stack frame */
    }
    /* Tell the sender straight away instead of waiting for its POLL. */
    p_session->b_pending_ack  = true;
    p_session->pending_status = status;
    p_session->state = RX_ACTIVE; /* kept only until the ACK is queued */
    p_session->b_have_manifest = false;
    uint8_t  pkt[ALFP_ACK_LEN];
    uint16_t len = alfp_enc_ack(pkt, sizeof(pkt), p_session->sid, status);
    if (p_session->p_radio->p_send(p_session->src, pkt, len))
    {
        p_session->state = RX_FREE;
    }
}

static void rx_step (alfp_ctx_t* p_ctx)
{
    uint32_t tnow = now(p_ctx);
    for (unsigned idx = 0; idx < ALFP_RX_SESSIONS; idx++)
    {
        alfp_rx_t* p_session = &p_ctx->rx_sessions[idx];
        switch (p_session->state)
        {
            case RX_FREE:
                break;

            case RX_ACTIVE:
                if (p_session->b_pending_ack)
                {
                    uint8_t  pkt[ALFP_ACK_LEN];
                    uint16_t len =
                        alfp_enc_ack(pkt, sizeof(pkt), p_session->sid,
                                     p_session->pending_status);
                    if (p_session->p_radio->p_send(p_session->src, pkt, len))
                    {
                        p_session->state = RX_FREE;
                    }
                    break;
                }
                if (p_session->b_pending_nack)
                {
                    rx_try_send_nack(p_ctx, p_session);
                }
                if ((uint32_t)(tnow - p_session->last_ms) >
                    p_session->p_prof->rx_expiry_ms)
                {
                    p_ctx->stats.rx_expired++;
                    rx_free(p_ctx, p_session, true);
                    ev_rx_fail(p_ctx, p_session->src, p_session->sid,
                               "expired");
                }
                break;

            case RX_VERIFYING:
            {
                uint32_t left =
                    p_session->manifest.file_size - p_session->verify_off;
                uint16_t chunk_len =
                    (uint16_t)(left < ALFP_HASH_CHUNK ? left : ALFP_HASH_CHUNK);
                int got = st(p_ctx)->p_read_at(
                    st(p_ctx)->p_ctx, p_session->file_handle,
                    p_session->verify_off, p_ctx->io_buf, chunk_len);
                if (got != (int)chunk_len)
                {
                    /* Treat an unreadable partial file as corrupt: start the
                     * data again. */
                    p_session->verify_off = p_session->manifest.file_size;
                    p_session->verify_crc = ~p_session->manifest.file_crc32;
                }
                else
                {
                    p_session->verify_crc = alfp_crc32(
                        p_session->verify_crc, p_ctx->io_buf, chunk_len);
                    p_session->verify_off += chunk_len;
                }
                if (p_session->verify_off >= p_session->manifest.file_size)
                {
                    if (p_session->verify_crc == p_session->manifest.file_crc32)
                    {
                        rx_finish_ok(p_ctx, p_session);
                    }
                    else
                    {
                        p_ctx->stats.rx_crc_fail++;
                        logf_(p_ctx, "alfp: file CRC-32 mismatch, requesting "
                                     "full resend");
                        ev_rx_fail(p_ctx, p_session->src, p_session->sid,
                                   "crc");
                        rx_reset_data(p_session);
                        p_session->state = RX_ACTIVE;
                        uint8_t  pkt[ALFP_ACK_LEN];
                        uint16_t len =
                            alfp_enc_ack(pkt, sizeof(pkt), p_session->sid,
                                         ALFP_ACK_CRC_FAIL);
                        if (!p_session->p_radio->p_send(p_session->src, pkt,
                                                        len))
                        {
                            p_ctx->stats.send_blocked++;
                        }
                    }
                }
                break;
            }

            default:
                /* r->state only ever holds the RX_* states above; nothing to
                 * do. */
                break;
        }
    }
}

/* =============================================================================
 * DISPATCH
 * =============================================================================
 */

void alfp_handle_packet (alfp_ctx_t* p_ctx, const mesh_radio_t* p_radio,
                         uint8_t src, const uint8_t* p_buf, uint16_t len)
{
    alfp_pkt_t pkt;
    p_ctx->stats.pkts_in++;
    alfp_dec_t dec = alfp_decode(p_buf, len, &pkt);
    if (ALFP_DEC_OK != dec)
    {
        if (ALFP_DEC_CRC == dec)
        {
            p_ctx->stats.pkts_bad_crc++;
        }
        else
        {
            p_ctx->stats.pkts_bad_other++;
        }
        return;
    }
    bool b_is_bridge = p_ctx->cfg.node_id == p_ctx->cfg.bridge_id;
    switch (pkt.type)
    {
        case ALFP_T_MANIFEST:
            if (b_is_bridge)
            {
                rx_on_manifest(p_ctx, p_radio, src, &pkt);
            }
            break;
        case ALFP_T_DATA:
            if (b_is_bridge)
            {
                rx_on_data(p_ctx, p_radio, src, &pkt);
            }
            break;
        case ALFP_T_POLL:
            if (b_is_bridge)
            {
                rx_on_poll(p_ctx, p_radio, src, &pkt);
            }
            break;
        case ALFP_T_NACK:
            if ((TX_IDLE != p_ctx->tx_session.state) &&
                (pkt.sid == p_ctx->tx_session.sid) &&
                (src == p_ctx->tx_session.dst) &&
                (p_radio == p_ctx->tx_session.p_radio))
            {
                tx_on_nack(p_ctx, &pkt);
            }
            break;
        case ALFP_T_ACK:
            if ((TX_IDLE != p_ctx->tx_session.state) &&
                (pkt.sid == p_ctx->tx_session.sid) &&
                (src == p_ctx->tx_session.dst) &&
                (p_radio == p_ctx->tx_session.p_radio))
            {
                tx_on_ack(p_ctx, &pkt);
            }
            break;
        default:
            break;
    }
}

void alfp_step (alfp_ctx_t* p_ctx)
{
    tx_step(p_ctx);
    rx_step(p_ctx);
}

void alfp_service (alfp_ctx_t* p_ctx, const mesh_radio_t* const* pp_radios,
                   uint8_t n_radios)
{
    uint8_t src;
    for (uint8_t idx = 0; idx < n_radios; idx++)
    {
        const mesh_radio_t* p_radio = pp_radios[idx];
        if (!p_radio)
        {
            continue;
        }
        /* Bounded: at most 16 packets per radio per call, so one busy radio
         * cannot starve the rest. */
        for (int kdx = 0; kdx < 16; kdx++)
        {
            int len = p_radio->p_recv(&src, p_ctx->pkt, sizeof(p_ctx->pkt));
            if (MESH_RECV_NONE == len)
            {
                break;
            }
            if (len < 0)
            {
                p_ctx->stats.pkts_bad_other++;
                continue;
            }
            /* Copy out: handlers reuse ctx->pkt for replies. */
            uint8_t input[ALFP_MAX_PKT];
            memcpy(input, p_ctx->pkt, (size_t)len);
            alfp_handle_packet(p_ctx, p_radio, src, input, (uint16_t)len);
        }
    }
    alfp_step(p_ctx);
}

/*** end of file ***/

