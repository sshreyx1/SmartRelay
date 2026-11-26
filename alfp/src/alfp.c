/*
 * alfp.c - the sender and receiver state machines.
 *
 * Sender (field node), one transfer at a time:
 *   IDLE -> HASHING (CRC-32 of the file, 512 bytes per step)
 *        -> SENDING  (manifest, then every fragment in 'todo', then a POLL; paced by the profile)
 *        -> WAITING  (for NACK/ACK; timeout re-sends just the POLL)
 *        -> NACK: todo = what the bridge is missing, round++, back to SENDING
 *        -> ACK(OK): done.   Too many rounds or unanswered polls: give up (file stays on SD).
 *
 * Receiver (bridge), up to ALFP_RX_SESSIONS at once, keyed by (source node, session id):
 *   first MANIFEST or DATA opens a session and a .part file on SD
 *   each DATA is CRC-checked, then written straight to SD at seq * frag_size (no RAM copy of the image)
 *   a bitmap records which fragments have arrived; duplicates are counted and ignored
 *   POLL is answered with a NACK built from the bitmap (list or bitmap, whichever is shorter)
 *   when every fragment is in: verify CRC-32 from SD, rename to inbox/<xx>/, ACK, remember the result
 */
#include "alfp.h"

#include <stdio.h>
#include <string.h>

#include "alfp_crc.h"

/* ---- small helpers --------------------------------------------------------------------- */

static uint32_t now(alfp_ctx_t* c)
{
    return c->cfg.platform->now_ms(c->cfg.platform->ctx);
}

static bool reached(uint32_t t_now, uint32_t deadline)
{
    return (int32_t)(t_now - deadline) >= 0; /* wrap-safe */
}

static void logf_(alfp_ctx_t* c, const char* msg)
{
    if (c->cfg.platform->log)
    {
        c->cfg.platform->log(c->cfg.platform->ctx, msg);
    }
}

static const alfp_storage_t* st(alfp_ctx_t* c)
{
    return c->cfg.storage;
}

/* Optional progress callbacks (alfp_cfg_t.on_rx_fail ...): report only, never change state. */
static void ev_rx_fail(alfp_ctx_t* c, uint8_t src, uint16_t sid, const char* why)
{
    if (c->cfg.on_rx_fail)
    {
        c->cfg.on_rx_fail(c, src, sid, why);
    }
}

static void bset(uint8_t* bm, uint16_t i)
{
    bm[i >> 3] = (uint8_t)(bm[i >> 3] | (1u << (i & 7u)));
}

static void bclr(uint8_t* bm, uint16_t i)
{
    bm[i >> 3] = (uint8_t)(bm[i >> 3] & ~(1u << (i & 7u)));
}

static bool bget(const uint8_t* bm, uint16_t i)
{
    return (bm[i >> 3] >> (i & 7u)) & 1u;
}

static void bfill(uint8_t* bm, uint16_t n)
{
    memset(bm, 0, ALFP_BITMAP_BYTES);
    for (uint16_t i = 0; i < n; i++)
    {
        bset(bm, i);
    }
}

static uint16_t frag_len(uint32_t file_size, uint16_t frag_size, uint16_t seq)
{
    uint32_t off = (uint32_t)seq * frag_size;
    uint32_t left = file_size - off;
    return (uint16_t)(left < frag_size ? left : frag_size);
}

static alfp_pace_t* pace_for(alfp_ctx_t* c, const mesh_radio_t* radio);
static void pace_update(alfp_ctx_t* c, uint16_t missing);

/* ---- init --------------------------------------------------------------------------------- */

/* Follow-up F9: a transfer cut off by a reboot or reflash leaves its "<tmp_dir>/nS_SSSSS.part"
 * behind, and nothing else ever deletes it (the session it belonged to is gone). At start-up no
 * session is open, so every .part file in tmp_dir is stale. Names are collected first and removed
 * afterwards, a batch at a time, so the directory is never changed while it is being listed. */
#define STALE_BATCH 8u
typedef struct
{
    char names[STALE_BATCH][24];
    unsigned n;
    bool more;
} stale_list_t;

static void stale_part_cb(const char* name, void* user)
{
    stale_list_t* l = (stale_list_t*)user;
    size_t len = strlen(name);
    if (len < 6u || strcmp(name + len - 5u, ".part") != 0 || len >= sizeof(l->names[0]))
    {
        return;
    }
    if (l->n < STALE_BATCH)
    {
        memcpy(l->names[l->n++], name, len + 1u);
    }
    else
    {
        l->more = true;
    }
}

static void remove_stale_parts(alfp_ctx_t* ctx)
{
    if (!st(ctx)->list || !st(ctx)->remove)
    {
        return;
    }
    unsigned removed = 0;
    for (unsigned pass = 0; pass < 64u; pass++)
    {
        stale_list_t l;
        memset(&l, 0, sizeof(l));
        st(ctx)->list(st(ctx)->ctx, ctx->cfg.tmp_dir, stale_part_cb, &l);
        unsigned gone = 0;
        for (unsigned i = 0; i < l.n; i++)
        {
            char p[ALFP_PATH_MAX + 24];
            snprintf(p, sizeof(p), "%s/%s", ctx->cfg.tmp_dir, l.names[i]);
            if (st(ctx)->remove(st(ctx)->ctx, p) >= 0)
            {
                gone++;
            }
        }
        removed += gone;
        if (!l.more || gone == 0u)
        {
            break;
        }
    }
    if (removed && ctx->cfg.platform->log)
    {
        char line[64];
        snprintf(line, sizeof(line), "alfp: removed %u stale .part file(s) from %s/", removed, ctx->cfg.tmp_dir);
        ctx->cfg.platform->log(ctx->cfg.platform->ctx, line);
    }
}

void alfp_init(alfp_ctx_t* ctx, const alfp_cfg_t* cfg)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = *cfg;
    if (!ctx->cfg.inbox_dir)
    {
        ctx->cfg.inbox_dir = "inbox";
    }
    if (!ctx->cfg.tmp_dir)
    {
        ctx->cfg.tmp_dir = "rx";
    }
    ctx->tx.fh = -1;
    for (unsigned i = 0; i < ALFP_RX_SESSIONS; i++)
    {
        ctx->rx[i].fh = -1;
    }
    /* Random start so a rebooted node does not reuse a session id the bridge still remembers. */
    ctx->next_sid = (uint16_t)cfg->platform->rand32(cfg->platform->ctx);
    if (ctx->cfg.node_id == ctx->cfg.bridge_id && st(ctx)->mkdir)
    {
        st(ctx)->mkdir(st(ctx)->ctx, ctx->cfg.inbox_dir);
        st(ctx)->mkdir(st(ctx)->ctx, ctx->cfg.tmp_dir);
    }
    if (ctx->cfg.node_id == ctx->cfg.bridge_id)
    {
        remove_stale_parts(ctx);
    }
}

/* =========================================================================================
 * SENDER
 * ========================================================================================= */

bool alfp_tx_active(const alfp_ctx_t* ctx)
{
    return ctx->tx.state != TX_IDLE;
}

static void tx_finish(alfp_ctx_t* c, alfp_tx_result_t res, uint8_t ack_status)
{
    alfp_tx_t* t = &c->tx;
    if (t->fh >= 0)
    {
        st(c)->close(st(c)->ctx, t->fh);
        t->fh = -1;
    }
    t->rep.result = res;
    t->rep.ack_status = ack_status;
    t->rep.rounds = t->round;
    t->rep.gap_ms_end = t->gap_ms;
    t->rep.file_crc32 = t->state == TX_HASHING ? 0 : t->crc32;
    if (res == ALFP_TX_DONE && t->round == 1 && t->radio)
    {
        pace_update(c, 0); /* a clean first round: allow the gap to shrink */
    }
    t->rep.finished_ms = now(c);
    t->state = TX_IDLE;
    if (res == ALFP_TX_DONE)
    {
        c->stats.tx_ok++;
    }
    else
    {
        c->stats.tx_failed++;
    }
    if (c->cfg.on_tx_done)
    {
        alfp_tx_report_t r = t->rep; /* callback may start the next transfer */
        c->cfg.on_tx_done(c, &r);
    }
}

alfp_err_t alfp_send_file(alfp_ctx_t* ctx, const mesh_radio_t* radio, uint8_t dst, const char* path,
                          const uint8_t* meta, uint8_t meta_len)
{
    alfp_tx_t* t = &ctx->tx;
    if (t->state != TX_IDLE)
    {
        return ALFP_E_BUSY;
    }
    if (!radio || !path || meta_len > ALFP_MAX_META || (meta_len && !meta) || strlen(path) >= ALFP_PATH_MAX)
    {
        return ALFP_E_ARG;
    }
    uint16_t mp = radio->max_payload();
    if (mp > ALFP_MAX_PKT)
    {
        mp = ALFP_MAX_PKT;
    }
    if (mp < ALFP_MANIFEST_OVERHEAD + meta_len || mp <= ALFP_DATA_OVERHEAD)
    {
        return ALFP_E_ARG;
    }

    int fh = st(ctx)->open_read(st(ctx)->ctx, path);
    if (fh < 0)
    {
        return ALFP_E_OPEN;
    }
    int32_t size = st(ctx)->size(st(ctx)->ctx, fh);
    if (size <= 0)
    {
        st(ctx)->close(st(ctx)->ctx, fh);
        return size < 0 ? ALFP_E_OPEN : ALFP_E_EMPTY;
    }
    uint16_t fsize = (uint16_t)(mp - ALFP_DATA_OVERHEAD);
    uint32_t count = ((uint32_t)size + fsize - 1u) / fsize;
    if (count > ALFP_MAX_FRAGS)
    {
        st(ctx)->close(st(ctx)->ctx, fh);
        return ALFP_E_TOO_BIG;
    }

    memset(t, 0, sizeof(*t));
    t->radio = radio;
    t->prof = alfp_profile_for(ctx, radio);
    t->dst = dst;
    t->fh = fh;
    strcpy(t->path, path);
    if (meta_len)
    {
        memcpy(t->meta, meta, meta_len);
    }
    t->meta_len = meta_len;
    t->sid = ctx->next_sid++;
    t->file_size = (uint32_t)size;
    t->frag_size = fsize;
    t->frag_count = (uint16_t)count;
    t->crc32 = ALFP_CRC32_INIT;
    t->hash_off = 0;
    t->state = TX_HASHING;
    {
        alfp_pace_t* pc = pace_for(ctx, radio);
        bool adaptive = t->prof->gap_max_ms > t->prof->gap_ms;
        t->gap_ms = (adaptive && pc->gap_ms) ? pc->gap_ms : t->prof->gap_ms;
    }

    t->rep.sid = t->sid;
    t->rep.radio = radio->name;
    t->rep.path = t->path;
    t->rep.file_size = t->file_size;
    t->rep.frag_count = t->frag_count;
    t->rep.frag_size = t->frag_size;
    t->rep.started_ms = now(ctx);
    return ALFP_OK;
}

/* Try to hand one packet to the radio. Returns true if accepted. */
static alfp_pace_t* pace_for(alfp_ctx_t* c, const mesh_radio_t* radio)
{
    for (unsigned i = 0; i < 2; i++)
    {
        if (c->pace[i].radio == radio)
        {
            return &c->pace[i];
        }
    }
    for (unsigned i = 0; i < 2; i++)
    {
        if (!c->pace[i].radio)
        {
            c->pace[i].radio = radio;
            c->pace[i].gap_ms = 0;
            return &c->pace[i];
        }
    }
    return &c->pace[0];
}

/* Adaptive pacing, applied once per round from the NACK:
 *   more than 25% of this round's fragments missing -> double the gap (flood colliding with itself)
 *   under 5% missing                                -> shrink it by a quarter
 * bounded by the profile's [gap_ms, gap_max_ms]. Random link loss also raises it, which costs time
 * but never correctness. */
static void pace_update(alfp_ctx_t* c, uint16_t missing)
{
    alfp_tx_t* t = &c->tx;
    if (!t->sent_round)
    {
        return;
    }
    uint32_t lo = t->prof->gap_ms, hi = t->prof->gap_max_ms > lo ? t->prof->gap_max_ms : lo;
    uint32_t loss_pct = (uint32_t)missing * 100u / t->sent_round;
    if (loss_pct > 25u)
    {
        t->gap_ms = t->gap_ms * 2u > hi ? hi : t->gap_ms * 2u;
    }
    else if (loss_pct < 5u)
    {
        t->gap_ms = t->gap_ms - t->gap_ms / 4u < lo ? lo : t->gap_ms - t->gap_ms / 4u;
    }
    pace_for(c, t->radio)->gap_ms = t->gap_ms;
}

static bool tx_put(alfp_ctx_t* c, const uint8_t* pkt, uint16_t len)
{
    alfp_tx_t* t = &c->tx;
    if (!t->radio->send(t->dst, pkt, len))
    {
        c->stats.send_blocked++;
        t->next_tx_ms = now(c) + 2u;
        return false;
    }
    t->rep.bytes_on_air += len;
    t->was_busy = true; /* wait for tx_busy() to clear, then the profile gap */
    return true;
}

static void tx_step(alfp_ctx_t* c)
{
    alfp_tx_t* t = &c->tx;
    uint32_t tnow = now(c);

    switch (t->state)
    {
        case TX_IDLE:
            return;

        case TX_HASHING:
        {
            uint32_t left = t->file_size - t->hash_off;
            uint16_t n = (uint16_t)(left < ALFP_HASH_CHUNK ? left : ALFP_HASH_CHUNK);
            int got = st(c)->read_at(st(c)->ctx, t->fh, t->hash_off, c->io, n);
            if (got != (int)n)
            {
                tx_finish(c, ALFP_TX_IO_ERROR, 0);
                return;
            }
            t->crc32 = alfp_crc32(t->crc32, c->io, n);
            t->hash_off += n;
            if (t->hash_off >= t->file_size)
            {
                bfill(t->todo, t->frag_count);
                t->todo_next = 0;
                t->send_manifest = true;
                t->polls_left = t->prof->poll_copies ? t->prof->poll_copies : 1u;
                t->copies = 1;
                t->round = 1;
                t->state = TX_SENDING;
                t->next_tx_ms = tnow;
                if (c->cfg.on_tx_start)
                {
                    c->cfg.on_tx_start(c, t);
                }
            }
            return;
        }

        case TX_WAITING:
            if (reached(tnow, t->wait_until_ms))
            {
                t->polls_unanswered++;
                if (t->polls_unanswered >= t->prof->max_polls)
                {
                    t->rep.why = 1;
                    tx_finish(c, ALFP_TX_GAVE_UP, 0);
                    return;
                }
                /* The POLL or its answer was lost: ask again (same round). */
                t->polls_left = t->prof->poll_copies ? t->prof->poll_copies : 1u;
                t->state = TX_SENDING;
            }
            return;

        case TX_SENDING:
            break;
    }

    /* Pacing: wait until the radio has finished, then the profile's gap. */
    if (t->was_busy)
    {
        if (t->radio->tx_busy())
        {
            return;
        }
        t->was_busy = false;
        t->next_tx_ms = tnow + t->gap_ms;
    }
    if (!reached(tnow, t->next_tx_ms) || t->radio->tx_busy())
    {
        return;
    }

    if (t->send_manifest)
    {
        alfp_manifest_t m = {
            .file_size = t->file_size,
            .frag_count = t->frag_count,
            .frag_size = t->frag_size,
            .file_crc32 = t->crc32,
            .radio_id = t->prof->radio_id,
            .meta_len = t->meta_len,
            .meta = t->meta,
        };
        uint16_t len = alfp_enc_manifest(c->pkt, sizeof(c->pkt), t->sid, &m);
        if (tx_put(c, c->pkt, len))
        {
            t->send_manifest = false;
            t->rep.manifests_sent++;
        }
        return;
    }

    /* Next fragment still to send this round. */
    while (t->todo_next < t->frag_count && !bget(t->todo, t->todo_next))
    {
        t->todo_next++;
    }
    if (t->todo_next < t->frag_count)
    {
        uint16_t seq = t->todo_next;
        uint16_t n = frag_len(t->file_size, t->frag_size, seq);
        uint16_t h = alfp_enc_data_header(c->pkt, t->sid, seq, t->frag_count);
        int got = st(c)->read_at(st(c)->ctx, t->fh, (uint32_t)seq * t->frag_size, c->pkt + h, n);
        if (got != (int)n)
        {
            tx_finish(c, ALFP_TX_IO_ERROR, 0);
            return;
        }
        uint16_t len = alfp_enc_data_finish(c->pkt, n);
        if (tx_put(c, c->pkt, len))
        {
            t->rep.data_sent++;
            t->sent_round++;
            if (t->round > 1)
            {
                t->rep.data_resent++;
            }
            if (t->copy_left)
            {
                t->copy_left--; /* this was the extra copy */
            }
            else if (t->copies > 1)
            {
                t->copy_left = (uint8_t)(t->copies - 1u);
            }
            if (!t->copy_left)
            {
                bclr(t->todo, seq);
                t->todo_next++;
            }
        }
        return;
    }

    if (t->polls_left)
    {
        uint16_t len = alfp_enc_poll(c->pkt, sizeof(c->pkt), t->sid, t->round);
        if (tx_put(c, c->pkt, len))
        {
            t->polls_left--;
            t->rep.polls_sent++;
            if (!t->polls_left)
            {
                t->state = TX_WAITING;
                t->wait_until_ms = tnow + t->prof->poll_timeout_ms;
            }
        }
        return;
    }

    /* Nothing left to send and no poll pending: wait for an answer. */
    t->state = TX_WAITING;
    t->wait_until_ms = tnow + t->prof->poll_timeout_ms;
}

static void mark_todo(uint16_t seq, void* user)
{
    alfp_tx_t* t = (alfp_tx_t*)user;
    bset(t->todo, seq);
}

static void count_seq(uint16_t seq, void* user)
{
    (void)seq;
    (*(uint16_t*)user)++;
}

static bool next_round(alfp_ctx_t* c)
{
    alfp_tx_t* t = &c->tx;
    if (t->round >= t->prof->max_rounds)
    {
        t->rep.why = 3;
        tx_finish(c, ALFP_TX_GAVE_UP, 0);
        return false;
    }
    t->round++;
    return true;
}

static void tx_on_nack(alfp_ctx_t* c, const alfp_pkt_t* p)
{
    alfp_tx_t* t = &c->tx;
    const alfp_nack_t* n = &p->u.nack;
    if (t->state != TX_WAITING || n->round != t->round)
    {
        return; /* stale or duplicate answer (e.g. two copies through the flood) */
    }
    t->polls_unanswered = 0;
    t->rep.nacks_received++;

    if (n->fmt == ALFP_NACK_FMT_NONE && !(n->flags & ALFP_NACK_NEED_MANIFEST))
    {
        /* Bridge has every fragment and is verifying: wait for its ACK. */
        t->wait_until_ms = now(c) + t->prof->poll_timeout_ms;
        return;
    }
    /* Progress check: a round that recovered nothing counts towards giving up. */
    uint16_t missing = 0;
    alfp_nack_for_each(n, t->frag_count, count_seq, &missing);
    if (c->cfg.on_tx_nack)
    {
        c->cfg.on_tx_nack(c, t->sid, t->round, missing);
    }
    if (t->round > 1 && missing >= t->last_missing)
    {
        if (++t->stalled >= t->prof->max_stalled)
        {
            t->rep.why = 2;
            tx_finish(c, ALFP_TX_GAVE_UP, 0);
            return;
        }
    }
    else
    {
        t->stalled = 0;
    }
    t->last_missing = missing;
    pace_update(c, missing);
    t->sent_round = 0;
    if (!next_round(c))
    {
        return;
    }
    /* Tail: with only a few fragments left, a second copy of each saves whole rounds. */
    t->copies = (t->prof->tail_dup && missing <= t->prof->tail_dup) ? 2u : 1u;
    t->copy_left = 0;
    memset(t->todo, 0, sizeof(t->todo));
    if (c->cfg.baseline_resend_all)
    {
        bfill(t->todo, t->frag_count);
    }
    else
    {
        alfp_nack_for_each(n, t->frag_count, mark_todo, t);
    }
    t->todo_next = 0;
    t->send_manifest = (n->flags & ALFP_NACK_NEED_MANIFEST) || n->fmt == ALFP_NACK_FMT_ALL;
    t->polls_left = t->prof->poll_copies ? t->prof->poll_copies : 1u;
    t->state = TX_SENDING;
}

static void tx_on_ack(alfp_ctx_t* c, const alfp_pkt_t* p)
{
    alfp_tx_t* t = &c->tx;
    if (t->state == TX_IDLE || t->state == TX_HASHING)
    {
        return;
    }
    switch (p->u.status)
    {
        case ALFP_ACK_OK:
            tx_finish(c, ALFP_TX_DONE, ALFP_ACK_OK);
            return;
        case ALFP_ACK_CRC_FAIL:
            /* Every fragment passed its CRC-16 but the file CRC-32 did not match: send it all again. */
            if (!next_round(c))
            {
                return;
            }
            bfill(t->todo, t->frag_count);
            t->todo_next = 0;
            t->polls_left = t->prof->poll_copies ? t->prof->poll_copies : 1u;
            t->state = TX_SENDING;
            return;
        default:
            tx_finish(c, ALFP_TX_REFUSED, p->u.status);
            return;
    }
}

/* =========================================================================================
 * RECEIVER (bridge)
 * ========================================================================================= */

static alfp_rx_t* rx_find(alfp_ctx_t* c, uint8_t src, uint16_t sid)
{
    for (unsigned i = 0; i < ALFP_RX_SESSIONS; i++)
    {
        alfp_rx_t* r = &c->rx[i];
        if (r->state != RX_FREE && r->src == src && r->sid == sid)
        {
            return r;
        }
    }
    return NULL;
}

static const alfp_done_t* done_find(alfp_ctx_t* c, uint8_t src, uint16_t sid)
{
    for (unsigned i = 0; i < ALFP_DONE_MEMORY; i++)
    {
        if (c->done[i].used && c->done[i].src == src && c->done[i].sid == sid)
        {
            return &c->done[i];
        }
    }
    return NULL;
}

static void done_add(alfp_ctx_t* c, uint8_t src, uint16_t sid, uint8_t status)
{
    alfp_done_t* d = &c->done[c->done_next];
    c->done_next = (uint8_t)((c->done_next + 1u) % ALFP_DONE_MEMORY);
    d->used = true;
    d->src = src;
    d->sid = sid;
    d->status = status;
}

/* Every refusal is logged with its reason, so a refusing bridge says why instead of only the sender
 * seeing "refused". */
static void log_refusal(alfp_ctx_t* c, uint8_t src, uint16_t sid, uint8_t status, const char* why)
{
    const alfp_platform_t* p = c->cfg.platform;
    if (p != NULL && p->log != NULL)
    {
        char line[128];
        snprintf(line, sizeof(line), "alfp: refused node %u sid %05u, status %u: %s", (unsigned)src, (unsigned)sid,
                 (unsigned)status, why);
        p->log(p->ctx, line);
    }
}

/* Best-effort reply outside any session (unknown session, remembered result). */
static void reply_ack(alfp_ctx_t* c, const mesh_radio_t* radio, uint8_t dst, uint16_t sid, uint8_t status)
{
    uint8_t pkt[ALFP_ACK_LEN];
    uint16_t len = alfp_enc_ack(pkt, sizeof(pkt), sid, status);
    if (!radio->send(dst, pkt, len))
    {
        c->stats.send_blocked++;
    }
}

static void rx_free(alfp_ctx_t* c, alfp_rx_t* r, bool delete_tmp)
{
    if (r->fh >= 0)
    {
        st(c)->close(st(c)->ctx, r->fh);
        r->fh = -1;
    }
    if (delete_tmp && r->tmp_path[0])
    {
        st(c)->remove(st(c)->ctx, r->tmp_path);
    }
    r->state = RX_FREE;
}

static alfp_rx_t* rx_open(alfp_ctx_t* c, const mesh_radio_t* radio, uint8_t src, uint16_t sid, uint8_t* refuse)
{
    alfp_rx_t* slot = NULL;
    for (unsigned i = 0; i < ALFP_RX_SESSIONS && !slot; i++)
    {
        if (c->rx[i].state == RX_FREE)
        {
            slot = &c->rx[i];
        }
    }
    if (!slot)
    {
        /* Evict the most idle ACTIVE session if it has been quiet for over half its expiry. */
        uint32_t tnow = now(c), worst = 0;
        for (unsigned i = 0; i < ALFP_RX_SESSIONS; i++)
        {
            alfp_rx_t* r = &c->rx[i];
            uint32_t idle = tnow - r->last_ms;
            if (r->state == RX_ACTIVE && idle > r->prof->rx_expiry_ms / 2u && idle > worst)
            {
                worst = idle;
                slot = r;
            }
        }
        if (!slot)
        {
            *refuse = ALFP_ACK_BUSY;
            c->stats.rx_refused_busy++;
            log_refusal(c, src, sid, ALFP_ACK_BUSY, "all receive sessions busy");
            ev_rx_fail(c, src, sid, "busy");
            return NULL;
        }
        rx_free(c, slot, true);
        c->stats.rx_expired++;
        ev_rx_fail(c, slot->src, slot->sid, "evicted");
    }

    memset(slot, 0, sizeof(*slot));
    slot->fh = -1;
    slot->radio = radio;
    slot->prof = alfp_profile_for(c, radio);
    slot->src = src;
    slot->sid = sid;
    snprintf(slot->tmp_path, sizeof(slot->tmp_path), "%s/n%u_%05u.part", c->cfg.tmp_dir, (unsigned)src, (unsigned)sid);
    slot->fh = st(c)->open_write(st(c)->ctx, slot->tmp_path);
    if (slot->fh < 0)
    {
        char why[ALFP_PATH_MAX + 24];
        snprintf(why, sizeof(why), "cannot create %s", slot->tmp_path);
        log_refusal(c, src, sid, ALFP_ACK_STORAGE, why);
        slot->tmp_path[0] = 0;
        *refuse = ALFP_ACK_STORAGE;
        ev_rx_fail(c, src, sid, "storage");
        return NULL;
    }
    slot->state = RX_ACTIVE;
    slot->last_ms = now(c);
    slot->rep.src = src;
    slot->rep.sid = sid;
    slot->rep.radio = radio->name;
    slot->rep.first_ms = slot->last_ms;
    if (c->cfg.on_rx_start)
    {
        c->cfg.on_rx_start(c, src, sid, radio->name);
    }
    return slot;
}

static void rx_reset_data(alfp_rx_t* r)
{
    memset(r->have, 0, sizeof(r->have));
    r->n_have = 0;
}

static void rx_maybe_complete(alfp_ctx_t* c, alfp_rx_t* r)
{
    (void)c;
    if (r->state == RX_ACTIVE && r->have_manifest && r->n_have == r->frag_count)
    {
        r->state = RX_VERIFYING;
        r->verify_off = 0;
        r->verify_crc = ALFP_CRC32_INIT;
        r->pending_nack = false;
    }
}

static void rx_adopt_manifest(alfp_ctx_t* c, alfp_rx_t* r, const alfp_manifest_t* m)
{
    if (r->n_have && (m->frag_size != r->frag_size || m->frag_count != r->frag_count))
    {
        rx_reset_data(r); /* provisional layout from an early DATA was wrong: start clean */
    }
    r->m = *m;
    memcpy(r->meta, m->meta, m->meta_len);
    r->m.meta = r->meta;
    r->frag_size = m->frag_size;
    r->frag_count = m->frag_count;
    r->have_manifest = true;
    if (c->cfg.on_rx_manifest)
    {
        c->cfg.on_rx_manifest(c, r->src, r->sid, r->frag_count, r->frag_size, r->m.file_size, r->m.file_crc32,
                              r->meta, r->m.meta_len);
    }
}

static void rx_on_manifest(alfp_ctx_t* c, const mesh_radio_t* radio, uint8_t src, const alfp_pkt_t* p)
{
    const alfp_manifest_t* m = &p->u.manifest;
    const alfp_done_t* d = done_find(c, src, p->sid);
    if (d)
    {
        if (d->status != ALFP_ACK_OK)
        {
            log_refusal(c, src, p->sid, d->status, "same session refused earlier (remembered result)");
        }
        reply_ack(c, radio, src, p->sid, d->status);
        return;
    }
    alfp_rx_t* r = rx_find(c, src, p->sid);
    if (!r)
    {
        if (m->frag_count > ALFP_MAX_FRAGS)
        {
            log_refusal(c, src, p->sid, ALFP_ACK_TOO_BIG, "too many fragments");
            ev_rx_fail(c, src, p->sid, "too_big");
            reply_ack(c, radio, src, p->sid, ALFP_ACK_TOO_BIG);
            return;
        }
        uint8_t refuse = 0;
        r = rx_open(c, radio, src, p->sid, &refuse);
        if (!r)
        {
            reply_ack(c, radio, src, p->sid, refuse);
            return;
        }
    }
    r->last_ms = now(c);
    if (!r->have_manifest)
    {
        rx_adopt_manifest(c, r, m);
        rx_maybe_complete(c, r);
    }
}

static void rx_on_data(alfp_ctx_t* c, const mesh_radio_t* radio, uint8_t src, const alfp_pkt_t* p)
{
    const alfp_data_t* dd = &p->u.data;
    if (done_find(c, src, p->sid))
    {
        return; /* stray fragment of a finished transfer; the next POLL gets the ACK */
    }
    if (dd->frag_count > ALFP_MAX_FRAGS)
    {
        c->stats.pkts_bad_other++;
        return;
    }
    alfp_rx_t* r = rx_find(c, src, p->sid);
    if (!r)
    {
        uint8_t refuse = 0;
        r = rx_open(c, radio, src, p->sid, &refuse);
        if (!r)
        {
            return; /* the sender's POLL will get the refusal */
        }
        c->stats.data_orphan++;
    }
    if (r->state != RX_ACTIVE)
    {
        return;
    }
    if (!r->have_manifest && r->n_have == 0)
    {
        /* DATA before MANIFEST: infer the layout. Every fragment but the last is full size. */
        r->frag_count = dd->frag_count;
        if (dd->seq + 1u < dd->frag_count)
        {
            r->frag_size = dd->len;
        }
        else
        {
            uint16_t mp = radio->max_payload();
            r->frag_size = (uint16_t)((mp > ALFP_MAX_PKT ? ALFP_MAX_PKT : mp) - ALFP_DATA_OVERHEAD);
        }
    }
    if (dd->frag_count != r->frag_count)
    {
        c->stats.pkts_bad_other++;
        return;
    }
    /* Length check: all but the last are full; the last is whatever remains. */
    if (r->have_manifest)
    {
        if (dd->len != frag_len(r->m.file_size, r->frag_size, dd->seq))
        {
            c->stats.pkts_bad_other++;
            return;
        }
    }
    else if (dd->seq + 1u < dd->frag_count ? dd->len != r->frag_size : dd->len > r->frag_size)
    {
        c->stats.pkts_bad_other++;
        return;
    }
    r->last_ms = now(c);
    if (bget(r->have, dd->seq))
    {
        c->stats.data_dup++;
        r->rep.duplicates++;
        if (c->cfg.on_rx_frag)
        {
            c->cfg.on_rx_frag(c, src, p->sid, dd->seq, (uint8_t)r->rep.nacks_sent, true);
        }
        return;
    }
    int w = st(c)->write_at(st(c)->ctx, r->fh, (uint32_t)dd->seq * r->frag_size, dd->payload, dd->len);
    if (w != (int)dd->len)
    {
        logf_(c, "alfp: SD write failed");
        return; /* stays missing; will be re-requested */
    }
    bset(r->have, dd->seq);
    r->n_have++;
    if (c->cfg.on_rx_frag)
    {
        c->cfg.on_rx_frag(c, src, p->sid, dd->seq, (uint8_t)r->rep.nacks_sent, false);
    }
    rx_maybe_complete(c, r);
}

static void rx_try_send_nack(alfp_ctx_t* c, alfp_rx_t* r)
{
    uint8_t flags = (uint8_t)(r->have_manifest ? 0u : ALFP_NACK_NEED_MANIFEST);
    uint16_t cap = r->radio->max_payload();
    if (cap > sizeof(c->pkt))
    {
        cap = sizeof(c->pkt);
    }
    uint16_t len
        = alfp_enc_nack(c->pkt, cap, r->sid, flags, r->pending_round, r->have, r->frag_count, r->frag_count == 0);
    if (r->radio->send(r->src, c->pkt, len))
    {
        r->pending_nack = false;
        r->rep.nacks_sent++;
        if (c->cfg.on_rx_nack)
        {
            c->cfg.on_rx_nack(c, r->src, r->sid, (uint8_t)r->rep.nacks_sent, (uint16_t)(r->frag_count - r->n_have));
        }
    }
    else
    {
        c->stats.send_blocked++;
    }
}

static void rx_on_poll(alfp_ctx_t* c, const mesh_radio_t* radio, uint8_t src, const alfp_pkt_t* p)
{
    const alfp_done_t* d = done_find(c, src, p->sid);
    if (d)
    {
        reply_ack(c, radio, src, p->sid, d->status); /* our ACK was lost: say it again */
        return;
    }
    alfp_rx_t* r = rx_find(c, src, p->sid);
    if (!r)
    {
        /* Everything so far was lost (or we rebooted): ask for manifest + all data. */
        uint8_t pkt[ALFP_NACK_MIN];
        uint16_t len = alfp_enc_nack(pkt, sizeof(pkt), p->sid, ALFP_NACK_NEED_MANIFEST, p->u.round, NULL, 0, true);
        if (!radio->send(src, pkt, len))
        {
            c->stats.send_blocked++;
        }
        return;
    }
    r->last_ms = now(c);
    if (r->state == RX_VERIFYING)
    {
        return; /* ACK follows shortly */
    }
    if (r->pending_ack)
    {
        return;
    }
    r->pending_nack = true;
    r->pending_round = p->u.round;
    rx_try_send_nack(c, r);
}

/* Where a finished file goes. Two problems with the old flat "inbox/n<src>_<sid>.bin":
 *  - Follow-up F3: the session id is 16 bits and starts at a random value every boot, so on a card
 *    used long enough a new name can meet an older file, and the storage rename replaces its
 *    target. Now the first free name of n<src>_<sid>.bin, n<src>_<sid>_1.bin ... _99 is used (the
 *    .met sidecar takes the same stem).
 *  - Bridge freeze: one flat folder holding every file ever received. FatFs looks names up by
 *    reading the whole folder, and a long name (the old ".meta") needs up to ~6 more passes to pick
 *    a unique 8.3 alias, so the create and rename here took up to 2.8 s each on node 1's card
 *    (SPI at 1 MHz, ~6 ms per 512-byte sector), all inside alfp_service() with both radios
 *    unpolled. Files are now spread over 256 subfolders by the session id's high byte
 *    ("rcv/7f/"), so a folder holds at most 256 ids per sender, and both names are plain 8.3
 *    ("n2_32561.bin" + "n2_32561.met"), so each create is one pass over a small folder. */
#define INBOX_SUFFIX_MAX 99u

static bool path_exists(alfp_ctx_t* c, const char* path)
{
    int h = st(c)->open_read(st(c)->ctx, path);
    if (h < 0)
    {
        return false;
    }
    st(c)->close(st(c)->ctx, h);
    return true;
}

static void inbox_stem(alfp_ctx_t* c, const alfp_rx_t* r, char* stem, size_t cap)
{
    unsigned bucket = (unsigned)(r->sid >> 8);
    char dir[ALFP_PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s/%02x", c->cfg.inbox_dir, bucket) < 0)
    {
        dir[0] = 0;
    }
    if (!(c->inbox_made[bucket / 8u] & (1u << (bucket % 8u))))
    {
        if (st(c)->mkdir)
        {
            st(c)->mkdir(st(c)->ctx, dir);
        }
        c->inbox_made[bucket / 8u] = (uint8_t)(c->inbox_made[bucket / 8u] | (1u << (bucket % 8u)));
    }
    /* (return values checked: a too-long inbox_dir just gives a truncated name, never an overflow) */
    int n = snprintf(stem, cap, "%s/n%u_%05u", dir, (unsigned)r->src, (unsigned)r->sid);
    char probe[ALFP_PATH_MAX + 8];
    n = n < 0 ? n : snprintf(probe, sizeof(probe), "%s.bin", stem);
    for (unsigned k = 1; n >= 0 && k <= INBOX_SUFFIX_MAX && path_exists(c, probe); k++)
    {
        n = snprintf(stem, cap, "%s/n%u_%05u_%u", dir, (unsigned)r->src, (unsigned)r->sid, k);
        n = n < 0 ? n : snprintf(probe, sizeof(probe), "%s.bin", stem);
    }
}

static void rx_finish_ok(alfp_ctx_t* c, alfp_rx_t* r)
{
    char final_path[ALFP_PATH_MAX + 8];
    char stem[ALFP_PATH_MAX];
    st(c)->close(st(c)->ctx, r->fh);
    r->fh = -1;
    inbox_stem(c, r, stem, sizeof(stem));
    snprintf(final_path, sizeof(final_path), "%s.bin", stem);
    bool ok = st(c)->rename(st(c)->ctx, r->tmp_path, final_path) >= 0;
    if (ok && r->m.meta_len)
    {
        char meta_path[ALFP_PATH_MAX + 8];
        snprintf(meta_path, sizeof(meta_path), "%s.met", stem);
        int mh = st(c)->open_write(st(c)->ctx, meta_path);
        if (mh >= 0)
        {
            st(c)->write_at(st(c)->ctx, mh, 0, r->meta, r->m.meta_len);
            st(c)->close(st(c)->ctx, mh);
        }
    }
    uint8_t status = ok ? ALFP_ACK_OK : ALFP_ACK_STORAGE;
    if (!ok)
    {
        char why[ALFP_PATH_MAX + 32]; /* "cannot rename to " + final_path (ALFP_PATH_MAX + 8) */
        snprintf(why, sizeof(why), "cannot rename to %s", final_path);
        log_refusal(c, r->src, r->sid, status, why);
        ev_rx_fail(c, r->src, r->sid, "storage");
    }
    done_add(c, r->src, r->sid, status);
    r->tmp_path[0] = 0;

    if (ok)
    {
        c->stats.rx_ok++;
        r->rep.path = final_path;
        r->rep.file_size = r->m.file_size;
        r->rep.frag_count = r->frag_count;
        r->rep.file_crc32 = r->m.file_crc32;
        r->rep.meta = r->meta;
        r->rep.meta_len = r->m.meta_len;
        r->rep.fragments_received = r->n_have;
        r->rep.done_ms = now(c);
        if (c->cfg.on_rx_done)
        {
            c->cfg.on_rx_done(c, &r->rep);
        }
        r->rep.path = NULL; /* final_path is on this stack frame */
    }
    /* Tell the sender straight away instead of waiting for its POLL. */
    r->pending_ack = true;
    r->pending_status = status;
    r->state = RX_ACTIVE; /* kept only until the ACK is queued */
    r->have_manifest = false;
    uint8_t pkt[ALFP_ACK_LEN];
    uint16_t len = alfp_enc_ack(pkt, sizeof(pkt), r->sid, status);
    if (r->radio->send(r->src, pkt, len))
    {
        r->state = RX_FREE;
    }
}

static void rx_step(alfp_ctx_t* c)
{
    uint32_t tnow = now(c);
    for (unsigned i = 0; i < ALFP_RX_SESSIONS; i++)
    {
        alfp_rx_t* r = &c->rx[i];
        switch (r->state)
        {
            case RX_FREE:
                break;

            case RX_ACTIVE:
                if (r->pending_ack)
                {
                    uint8_t pkt[ALFP_ACK_LEN];
                    uint16_t len = alfp_enc_ack(pkt, sizeof(pkt), r->sid, r->pending_status);
                    if (r->radio->send(r->src, pkt, len))
                    {
                        r->state = RX_FREE;
                    }
                    break;
                }
                if (r->pending_nack)
                {
                    rx_try_send_nack(c, r);
                }
                if ((uint32_t)(tnow - r->last_ms) > r->prof->rx_expiry_ms)
                {
                    c->stats.rx_expired++;
                    rx_free(c, r, true);
                    ev_rx_fail(c, r->src, r->sid, "expired");
                }
                break;

            case RX_VERIFYING:
            {
                uint32_t left = r->m.file_size - r->verify_off;
                uint16_t n = (uint16_t)(left < ALFP_HASH_CHUNK ? left : ALFP_HASH_CHUNK);
                int got = st(c)->read_at(st(c)->ctx, r->fh, r->verify_off, c->io, n);
                if (got != (int)n)
                {
                    /* Treat an unreadable partial file as corrupt: start the data again. */
                    r->verify_off = r->m.file_size;
                    r->verify_crc = ~r->m.file_crc32;
                }
                else
                {
                    r->verify_crc = alfp_crc32(r->verify_crc, c->io, n);
                    r->verify_off += n;
                }
                if (r->verify_off >= r->m.file_size)
                {
                    if (r->verify_crc == r->m.file_crc32)
                    {
                        rx_finish_ok(c, r);
                    }
                    else
                    {
                        c->stats.rx_crc_fail++;
                        logf_(c, "alfp: file CRC-32 mismatch, requesting full resend");
                        ev_rx_fail(c, r->src, r->sid, "crc");
                        rx_reset_data(r);
                        r->state = RX_ACTIVE;
                        uint8_t pkt[ALFP_ACK_LEN];
                        uint16_t len = alfp_enc_ack(pkt, sizeof(pkt), r->sid, ALFP_ACK_CRC_FAIL);
                        if (!r->radio->send(r->src, pkt, len))
                        {
                            c->stats.send_blocked++;
                        }
                    }
                }
                break;
            }
        }
    }
}

/* =========================================================================================
 * DISPATCH
 * ========================================================================================= */

void alfp_handle_packet(alfp_ctx_t* ctx, const mesh_radio_t* radio, uint8_t src, const uint8_t* buf, uint16_t len)
{
    alfp_pkt_t p;
    ctx->stats.pkts_in++;
    alfp_dec_t d = alfp_decode(buf, len, &p);
    if (d != ALFP_DEC_OK)
    {
        if (d == ALFP_DEC_CRC)
        {
            ctx->stats.pkts_bad_crc++;
        }
        else
        {
            ctx->stats.pkts_bad_other++;
        }
        return;
    }
    bool is_bridge = ctx->cfg.node_id == ctx->cfg.bridge_id;
    switch (p.type)
    {
        case ALFP_T_MANIFEST:
            if (is_bridge)
            {
                rx_on_manifest(ctx, radio, src, &p);
            }
            break;
        case ALFP_T_DATA:
            if (is_bridge)
            {
                rx_on_data(ctx, radio, src, &p);
            }
            break;
        case ALFP_T_POLL:
            if (is_bridge)
            {
                rx_on_poll(ctx, radio, src, &p);
            }
            break;
        case ALFP_T_NACK:
            if (ctx->tx.state != TX_IDLE && p.sid == ctx->tx.sid && src == ctx->tx.dst && radio == ctx->tx.radio)
            {
                tx_on_nack(ctx, &p);
            }
            break;
        case ALFP_T_ACK:
            if (ctx->tx.state != TX_IDLE && p.sid == ctx->tx.sid && src == ctx->tx.dst && radio == ctx->tx.radio)
            {
                tx_on_ack(ctx, &p);
            }
            break;
        default:
            break;
    }
}

void alfp_step(alfp_ctx_t* ctx)
{
    tx_step(ctx);
    rx_step(ctx);
}

void alfp_service(alfp_ctx_t* ctx, const mesh_radio_t* const* radios, uint8_t n_radios)
{
    uint8_t src;
    for (uint8_t i = 0; i < n_radios; i++)
    {
        const mesh_radio_t* r = radios[i];
        if (!r)
        {
            continue;
        }
        /* Bounded: at most 16 packets per radio per call, so one busy radio cannot starve the rest. */
        for (int k = 0; k < 16; k++)
        {
            int n = r->recv(&src, ctx->pkt, sizeof(ctx->pkt));
            if (n == MESH_RECV_NONE)
            {
                break;
            }
            if (n < 0)
            {
                ctx->stats.pkts_bad_other++;
                continue;
            }
            /* Copy out: handlers reuse ctx->pkt for replies. */
            uint8_t in[ALFP_MAX_PKT];
            memcpy(in, ctx->pkt, (size_t)n);
            alfp_handle_packet(ctx, r, src, in, (uint16_t)n);
        }
    }
    alfp_step(ctx);
}
