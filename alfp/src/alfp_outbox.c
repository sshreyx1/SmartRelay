/*
 * alfp_outbox.c - see alfp_outbox.h.
 */
#include "alfp_outbox.h"

#include <stdio.h>
#include <string.h>

#define BACKOFF_MIN_MS 5000u
#define BACKOFF_MAX_MS 60000u
#define NO_RADIO_RECHECK_MS 1000u

static const alfp_storage_t* st(alfp_outbox_t* ob)
{
    return ob->alfp->cfg.storage;
}

static uint32_t now(alfp_outbox_t* ob)
{
    const alfp_platform_t* p = ob->alfp->cfg.platform;
    return p->now_ms(p->ctx);
}

/* Size of a job's file without reading its contents: open, ask the storage port, close.
 * Returns -1 if the file cannot be opened or sized (caller falls back to alfp_select_radio()). */
static int32_t job_size(alfp_outbox_t* ob, const char* path)
{
    int h = st(ob)->open_read(st(ob)->ctx, path);
    if (h < 0)
    {
        return -1;
    }
    int32_t sz = st(ob)->size(st(ob)->ctx, h);
    st(ob)->close(st(ob)->ctx, h);
    return sz;
}

static void log_size_choice(alfp_outbox_t* ob, const char* path, int32_t size_bytes, const mesh_radio_t* radio,
                            const char* reason)
{
    const alfp_platform_t* p = ob->alfp->cfg.platform;
    if (p == NULL || p->log == NULL)
    {
        return;
    }
    char line[160];
    snprintf(line, sizeof(line), "tx %s size=%ld thr=%lu radio=%s reason=%s", path, (long)size_bytes,
             (unsigned long)ob->size_threshold, (radio->name != NULL) ? radio->name : "?", reason);
    p->log(p->ctx, line);
}

static bool parse_seq(const char* name, uint32_t* seq)
{
    unsigned long v = 0;
    char tail[8] = { 0 };
    if (name[0] != 'q' || sscanf(name + 1, "%8lu%7s", &v, tail) != 2 || strcmp(tail, ".job") != 0)
    {
        return false;
    }
    *seq = (uint32_t)v;
    return true;
}

static void job_path(const alfp_outbox_t* ob, uint32_t seq, char* out, size_t cap)
{
    snprintf(out, cap, "%s/q%08lu.job", ob->dir, (unsigned long)seq);
}

typedef struct
{
    uint32_t min, max;
    uint16_t count;
} scan_t;

static void scan_cb(const char* name, void* user)
{
    uint32_t seq;
    if (parse_seq(name, &seq))
    {
        scan_t* s = user;
        if (s->count == 0 || seq < s->min)
        {
            s->min = seq;
        }
        if (seq > s->max)
        {
            s->max = seq;
        }
        s->count++;
    }
}

static scan_t scan(alfp_outbox_t* ob)
{
    scan_t s = { 0, 0, 0 };
    st(ob)->list(st(ob)->ctx, ob->dir, scan_cb, &s);
    return s;
}

void alfp_outbox_init(alfp_outbox_t* ob, alfp_ctx_t* alfp, const mesh_radio_t* const* radios, uint8_t n_radios,
                      uint8_t bridge_id, uint32_t fresh_ms, const char* dir)
{
    memset(ob, 0, sizeof(*ob));
    ob->alfp = alfp;
    ob->radios = radios;
    ob->n_radios = n_radios;
    ob->bridge_id = bridge_id;
    ob->fresh_ms = fresh_ms;
    ob->dir = dir ? dir : "outbox";
    if (st(ob)->mkdir)
    {
        st(ob)->mkdir(st(ob)->ctx, ob->dir);
    }
    scan_t s = scan(ob);
    ob->count = s.count;
    ob->next_seq = s.max + 1u;
    ob->retry_at_ms = now(ob);
}

static void drop_job(alfp_outbox_t* ob, uint32_t seq)
{
    char p[ALFP_PATH_MAX + 16];
    job_path(ob, seq, p, sizeof(p));
    if (st(ob)->remove(st(ob)->ctx, p) >= 0 && ob->count)
    {
        ob->count--;
    }
}

bool alfp_outbox_add(alfp_outbox_t* ob, const char* image_path, const uint8_t* meta, uint8_t meta_len)
{
    size_t pl = strlen(image_path);
    if (pl >= ALFP_PATH_MAX || meta_len > ALFP_MAX_META)
    {
        return false;
    }
    while (ob->count >= ALFP_OUTBOX_MAX)
    {
        scan_t s = scan(ob);
        if (!s.count)
        {
            ob->count = 0;
            break;
        }
        uint32_t victim = s.min;
        if (victim == ob->active_seq && s.count > 1)
        {
            break; /* never drop the job being sent; accept one over the cap briefly */
        }
        drop_job(ob, victim);
        ob->dropped_full++;
        /* Follow-up F2: a dropped capture must be visible, not only counted. */
        const alfp_platform_t* p = ob->alfp ? ob->alfp->cfg.platform : NULL;
        if (p != NULL && p->log != NULL)
        {
            char line[96];
            snprintf(line, sizeof(line), "outbox: FULL (%u jobs), dropped oldest job q%08lu (%lu dropped so far)",
                     (unsigned)ALFP_OUTBOX_MAX, (unsigned long)victim, (unsigned long)ob->dropped_full);
            p->log(p->ctx, line);
        }
    }
    uint8_t buf[2 + 1 + ALFP_PATH_MAX + 1 + ALFP_MAX_META];
    size_t n = 0;
    buf[n++] = 'Q';
    buf[n++] = '1';
    buf[n++] = (uint8_t)pl;
    memcpy(buf + n, image_path, pl);
    n += pl;
    buf[n++] = meta_len;
    if (meta_len)
    {
        memcpy(buf + n, meta, meta_len);
        n += meta_len;
    }
    char p[ALFP_PATH_MAX + 16];
    uint32_t seq = ob->next_seq++;
    job_path(ob, seq, p, sizeof(p));
    int h = st(ob)->open_write(st(ob)->ctx, p);
    if (h < 0)
    {
        return false;
    }
    bool ok = st(ob)->write_at(st(ob)->ctx, h, 0, buf, (uint16_t)n) == (int)n;
    st(ob)->close(st(ob)->ctx, h);
    if (!ok)
    {
        st(ob)->remove(st(ob)->ctx, p);
        return false;
    }
    ob->count++;
    return true;
}

static bool load_job(alfp_outbox_t* ob, uint32_t seq, char* path, uint8_t* meta, uint8_t* meta_len)
{
    char p[ALFP_PATH_MAX + 16];
    uint8_t buf[2 + 1 + ALFP_PATH_MAX + 1 + ALFP_MAX_META];
    job_path(ob, seq, p, sizeof(p));
    int h = st(ob)->open_read(st(ob)->ctx, p);
    if (h < 0)
    {
        return false;
    }
    int n = st(ob)->read_at(st(ob)->ctx, h, 0, buf, sizeof(buf));
    st(ob)->close(st(ob)->ctx, h);
    if (n < 4 || buf[0] != 'Q' || buf[1] != '1')
    {
        return false;
    }
    uint8_t pl = buf[2];
    if (pl >= ALFP_PATH_MAX || 3 + pl + 1 > n)
    {
        return false;
    }
    memcpy(path, buf + 3, pl);
    path[pl] = 0;
    uint8_t ml = buf[3 + pl];
    if (ml > ALFP_MAX_META || 4 + pl + ml > n)
    {
        return false;
    }
    memcpy(meta, buf + 4 + pl, ml);
    *meta_len = ml;
    return true;
}

void alfp_outbox_pump(alfp_outbox_t* ob)
{
    if (!ob->count || alfp_tx_active(ob->alfp) || ob->active_seq)
    {
        return;
    }
    uint32_t t = now(ob);
    if ((int32_t)(t - ob->retry_at_ms) < 0)
    {
        return;
    }
    scan_t s = scan(ob);
    ob->count = s.count;
    if (!s.count)
    {
        return;
    }
    const mesh_radio_t* radio;
    char path[ALFP_PATH_MAX];
    uint8_t meta[ALFP_MAX_META];
    uint8_t ml = 0;
    if (ob->size_threshold == 0)
    {
        /* Legacy behaviour, byte-identical to before size-based choice existed: pick a radio by
         * preference order first, and only touch the SD card if one is available. */
        radio = alfp_select_radio(ob->radios, ob->n_radios, ob->bridge_id, t, ob->fresh_ms);
        if (!radio)
        {
            ob->no_radio_waits++;
            ob->retry_at_ms = t + NO_RADIO_RECHECK_MS; /* keep it on SD; check again shortly */
            return;
        }
        if (!load_job(ob, s.min, path, meta, &ml))
        {
            drop_job(ob, s.min); /* unreadable job file */
            return;
        }
    }
    else
    {
        /* Size decides the preferred radio, so the job's path (and therefore its size) must be
         * known before selecting. */
        if (!load_job(ob, s.min, path, meta, &ml))
        {
            drop_job(ob, s.min); /* unreadable job file */
            return;
        }
        int32_t size_bytes = job_size(ob, path);
        const char* reason;
        if (size_bytes >= 0)
        {
            radio = alfp_select_radio_for_size(ob->radios, ob->n_radios, ob->bridge_id, t, ob->fresh_ms,
                                               (uint32_t)size_bytes, ob->size_threshold);
            reason = (radio != NULL
                      && alfp_radio_was_size_preferred(ob->radios, ob->n_radios, (uint32_t)size_bytes,
                                                       ob->size_threshold, radio))
                ? "size"
                : "fallback";
        }
        else
        {
            /* Size could not be read: fall back to the old, size-unaware selection. */
            radio = alfp_select_radio(ob->radios, ob->n_radios, ob->bridge_id, t, ob->fresh_ms);
            reason = "fallback";
        }
        if (!radio)
        {
            ob->no_radio_waits++;
            ob->retry_at_ms = t + NO_RADIO_RECHECK_MS;
            return;
        }
        log_size_choice(ob, path, size_bytes, radio, reason);
    }
    if (ob->prepare_meta != NULL && ml != 0u)
    {
        ob->prepare_meta(meta, ml, ob->prepare_user);
    }
    alfp_err_t e = alfp_send_file(ob->alfp, radio, ob->bridge_id, path, meta, ml);
    if (e == ALFP_OK)
    {
        ob->active_seq = s.min;
    }
    else if (e == ALFP_E_OPEN || e == ALFP_E_EMPTY || e == ALFP_E_TOO_BIG || e == ALFP_E_ARG)
    {
        drop_job(ob, s.min); /* can never succeed */
    }
}

void alfp_outbox_set_size_threshold(alfp_outbox_t* ob, uint32_t threshold)
{
    ob->size_threshold = threshold;
}

void alfp_outbox_on_tx_done(alfp_outbox_t* ob, const alfp_tx_report_t* r)
{
    if (!ob->active_seq)
    {
        return;
    }
    uint32_t t = now(ob);
    if (r->result == ALFP_TX_DONE)
    {
        drop_job(ob, ob->active_seq);
        ob->sent_ok++;
        ob->backoff_ms = 0;
        ob->retry_at_ms = t;
    }
    else if (r->result == ALFP_TX_REFUSED && r->ack_status == ALFP_ACK_TOO_BIG)
    {
        drop_job(ob, ob->active_seq);
        ob->retry_at_ms = t;
    }
    else
    {
        /* Bridge unreachable or busy: back off (5 s, 10 s, 20 s ... 60 s) and try again. */
        ob->backoff_ms = ob->backoff_ms ? ob->backoff_ms * 2u : BACKOFF_MIN_MS;
        if (ob->backoff_ms > BACKOFF_MAX_MS)
        {
            ob->backoff_ms = BACKOFF_MAX_MS;
        }
        ob->retry_at_ms = t + ob->backoff_ms;
    }
    ob->active_seq = 0;
}

uint16_t alfp_outbox_count(const alfp_outbox_t* ob)
{
    return ob->count;
}
