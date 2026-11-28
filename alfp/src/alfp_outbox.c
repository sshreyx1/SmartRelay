/*
 * alfp_outbox.c - see alfp_outbox.h.
 */
#include <inttypes.h>
#include "alfp_outbox.h"

#include <stdio.h>
#include <string.h>

#define BACKOFF_MIN_MS 5000u
#define BACKOFF_MAX_MS 60000u
#define NO_RADIO_RECHECK_MS 1000u

static const alfp_storage_t* st(alfp_outbox_t* p_outbox)
{
    return p_outbox->p_alfp->cfg.p_storage;
}

static uint32_t now(alfp_outbox_t* p_outbox)
{
    const alfp_platform_t* p_plat = p_outbox->p_alfp->cfg.p_platform;
    return p_plat->p_now_ms(p_plat->p_ctx);
}

/* Size of a job's file without reading its contents: open, ask the storage port, close.
 * Returns -1 if the file cannot be opened or sized (caller falls back to alfp_select_radio()). */
static int32_t job_size(alfp_outbox_t* p_outbox, const char* p_path)
{
    int handle = st(p_outbox)->p_open_read(st(p_outbox)->p_ctx, p_path);
    if (handle < 0)
    {
        return -1;
    }
    int32_t size = st(p_outbox)->p_size(st(p_outbox)->p_ctx, handle);
    st(p_outbox)->p_close(st(p_outbox)->p_ctx, handle);
    return size;
}

static void log_size_choice(alfp_outbox_t* p_outbox, const char* p_path, int32_t size_bytes, const mesh_radio_t* p_radio,
                            const char* p_reason)
{
    const alfp_platform_t* p_plat = p_outbox->p_alfp->cfg.p_platform;
    if ((NULL == p_plat) || (NULL == p_plat->p_log))
    {
        return;
    }
    char line[160];
    snprintf(line, sizeof(line), "tx %s size=%" PRId32 " thr=%" PRIu32 " radio=%s reason=%s", p_path, (int32_t)size_bytes,
             (uint32_t)p_outbox->size_threshold, (NULL != p_radio->p_name) ? p_radio->p_name : "?", p_reason);
    p_plat->p_log(p_plat->p_ctx, line);
}

static bool parse_seq(const char* p_name, uint32_t* p_seq)
{
    uint32_t value = 0;
    char tail[8] = { 0 };
    if (('q' != p_name[0]) || (2 != sscanf(p_name + 1, "%8" SCNu32 "%7s", &value, tail)) || (0 != strcmp(tail, ".job")))
    {
        return false;
    }
    *p_seq = value;
    return true;
}

static void job_path(const alfp_outbox_t* p_outbox, uint32_t seq, char* p_out, size_t cap)
{
    snprintf(p_out, cap, "%s/q%08" PRIu32 ".job", p_outbox->p_dir, (uint32_t)seq);
}

typedef struct
{
    uint32_t min;
    uint32_t max;
    uint16_t count;
} scan_t;

static void scan_cb(const char* p_name, void* p_user)
{
    uint32_t seq;
    if (parse_seq(p_name, &seq))
    {
        scan_t* p_scan_res = p_user;
        if ((0 == p_scan_res->count) || (seq < p_scan_res->min))
        {
            p_scan_res->min = seq;
        }
        if (seq > p_scan_res->max)
        {
            p_scan_res->max = seq;
        }
        p_scan_res->count++;
    }
}

static scan_t scan(alfp_outbox_t* p_outbox)
{
    scan_t scan_res = { 0, 0, 0 };
    st(p_outbox)->p_list(st(p_outbox)->p_ctx, p_outbox->p_dir, scan_cb, &scan_res);
    return scan_res;
}

void alfp_outbox_init(alfp_outbox_t* p_outbox, alfp_ctx_t* p_alfp, const mesh_radio_t* const* pp_radios, uint8_t n_radios,
                      uint8_t bridge_id, uint32_t fresh_ms, const char* p_dir)
{
    memset(p_outbox, 0, sizeof(*p_outbox));
    p_outbox->p_alfp = p_alfp;
    p_outbox->pp_radios = pp_radios;
    p_outbox->n_radios = n_radios;
    p_outbox->bridge_id = bridge_id;
    p_outbox->fresh_ms = fresh_ms;
    p_outbox->p_dir = p_dir ? p_dir : "outbox";
    if (st(p_outbox)->p_mkdir)
    {
        st(p_outbox)->p_mkdir(st(p_outbox)->p_ctx, p_outbox->p_dir);
    }
    scan_t scan_res = scan(p_outbox);
    p_outbox->count = scan_res.count;
    p_outbox->next_seq = scan_res.max + 1u;
    p_outbox->retry_at_ms = now(p_outbox);
}

static void drop_job(alfp_outbox_t* p_outbox, uint32_t seq)
{
    char path[ALFP_PATH_MAX + 16];
    job_path(p_outbox, seq, path, sizeof(path));
    if ((st(p_outbox)->p_remove(st(p_outbox)->p_ctx, path) >= 0) && (p_outbox->count))
    {
        p_outbox->count--;
    }
}

bool alfp_outbox_add(alfp_outbox_t* p_outbox, const char* p_image_path, const uint8_t* p_meta, uint8_t meta_len)
{
    size_t path_len = strlen(p_image_path);
    if ((path_len >= ALFP_PATH_MAX) || (meta_len > ALFP_MAX_META))
    {
        return false;
    }
    while (p_outbox->count >= ALFP_OUTBOX_MAX)
    {
        scan_t scan_res = scan(p_outbox);
        if (!scan_res.count)
        {
            p_outbox->count = 0;
            break;
        }
        uint32_t victim = scan_res.min;
        if ((victim == p_outbox->active_seq) && (scan_res.count > 1))
        {
            break; /* never drop the job being sent; accept one over the cap briefly */
        }
        drop_job(p_outbox, victim);
        p_outbox->dropped_full++;
        /* Follow-up F2: a dropped capture must be visible, not only counted. */
        const alfp_platform_t* p_plat = p_outbox->p_alfp ? p_outbox->p_alfp->cfg.p_platform : NULL;
        if ((NULL != p_plat) && (NULL != p_plat->p_log))
        {
            char line[96];
            snprintf(line, sizeof(line), "outbox: FULL (%u jobs), dropped oldest job q%08" PRIu32 " (%" PRIu32 " dropped so far)",
                     (unsigned)ALFP_OUTBOX_MAX, (uint32_t)victim, (uint32_t)p_outbox->dropped_full);
            p_plat->p_log(p_plat->p_ctx, line);
        }
    }
    uint8_t buf[2 + 1 + ALFP_PATH_MAX + 1 + ALFP_MAX_META];
    size_t len = 0;
    buf[len++] = 'Q';
    buf[len++] = '1';
    buf[len++] = (uint8_t)path_len;
    memcpy(buf + len, p_image_path, path_len);
    len += path_len;
    buf[len++] = meta_len;
    if (meta_len)
    {
        memcpy(buf + len, p_meta, meta_len);
        len += meta_len;
    }
    char path[ALFP_PATH_MAX + 16];
    uint32_t seq = p_outbox->next_seq++;
    job_path(p_outbox, seq, path, sizeof(path));
    int handle = st(p_outbox)->p_open_write(st(p_outbox)->p_ctx, path);
    if (handle < 0)
    {
        return false;
    }
    bool b_is_ok = st(p_outbox)->p_write_at(st(p_outbox)->p_ctx, handle, 0, buf, (uint16_t)len) == (int)len;
    st(p_outbox)->p_close(st(p_outbox)->p_ctx, handle);
    if (!b_is_ok)
    {
        st(p_outbox)->p_remove(st(p_outbox)->p_ctx, path);
        return false;
    }
    p_outbox->count++;
    return true;
}

static bool load_job(alfp_outbox_t* p_outbox, uint32_t seq, char* p_path, uint8_t* p_meta, uint8_t* p_meta_len)
{
    char path[ALFP_PATH_MAX + 16];
    uint8_t buf[2 + 1 + ALFP_PATH_MAX + 1 + ALFP_MAX_META];
    job_path(p_outbox, seq, path, sizeof(path));
    int handle = st(p_outbox)->p_open_read(st(p_outbox)->p_ctx, path);
    if (handle < 0)
    {
        return false;
    }
    int len = st(p_outbox)->p_read_at(st(p_outbox)->p_ctx, handle, 0, buf, sizeof(buf));
    st(p_outbox)->p_close(st(p_outbox)->p_ctx, handle);
    if ((len < 4) || ('Q' != buf[0]) || ('1' != buf[1]))
    {
        return false;
    }
    uint8_t path_len = buf[2];
    if ((path_len >= ALFP_PATH_MAX) || (3 + path_len + 1 > len))
    {
        return false;
    }
    memcpy(p_path, buf + 3, path_len);
    p_path[path_len] = 0;
    uint8_t meta_len = buf[3 + path_len];
    if ((meta_len > ALFP_MAX_META) || (4 + path_len + meta_len > len))
    {
        return false;
    }
    memcpy(p_meta, buf + 4 + path_len, meta_len);
    *p_meta_len = meta_len;
    return true;
}

void alfp_outbox_pump(alfp_outbox_t* p_outbox)
{
    if ((!p_outbox->count) || (alfp_tx_active(p_outbox->p_alfp)) || (p_outbox->active_seq))
    {
        return;
    }
    uint32_t now_ms = now(p_outbox);
    if ((int32_t)(now_ms - p_outbox->retry_at_ms) < 0)
    {
        return;
    }
    scan_t scan_res = scan(p_outbox);
    p_outbox->count = scan_res.count;
    if (!scan_res.count)
    {
        return;
    }
    const mesh_radio_t* p_radio;
    char path[ALFP_PATH_MAX];
    uint8_t meta[ALFP_MAX_META];
    uint8_t meta_len = 0;
    if (0 == p_outbox->size_threshold)
    {
        /* Legacy behaviour, byte-identical to before size-based choice existed: pick a radio by
         * preference order first, and only touch the SD card if one is available. */
        p_radio = alfp_select_radio(p_outbox->pp_radios, p_outbox->n_radios, p_outbox->bridge_id, now_ms, p_outbox->fresh_ms);
        if (!p_radio)
        {
            p_outbox->no_radio_waits++;
            p_outbox->retry_at_ms = now_ms + NO_RADIO_RECHECK_MS; /* keep it on SD; check again shortly */
            return;
        }
        if (!load_job(p_outbox, scan_res.min, path, meta, &meta_len))
        {
            drop_job(p_outbox, scan_res.min); /* unreadable job file */
            return;
        }
    }
    else
    {
        /* Size decides the preferred radio, so the job's path (and therefore its size) must be
         * known before selecting. */
        if (!load_job(p_outbox, scan_res.min, path, meta, &meta_len))
        {
            drop_job(p_outbox, scan_res.min); /* unreadable job file */
            return;
        }
        int32_t size_bytes = job_size(p_outbox, path);
        const char* p_reason;
        if (size_bytes >= 0)
        {
            p_radio = alfp_select_radio_for_size(p_outbox->pp_radios, p_outbox->n_radios, p_outbox->bridge_id, now_ms, p_outbox->fresh_ms,
                                               (uint32_t)size_bytes, p_outbox->size_threshold);
            p_reason = ((NULL != p_radio)
                      && (alfp_radio_was_size_preferred(p_outbox->pp_radios, p_outbox->n_radios, (uint32_t)size_bytes,
                                                       p_outbox->size_threshold, p_radio)))
                ? "size"
                : "fallback";
        }
        else
        {
            /* Size could not be read: fall back to the old, size-unaware selection. */
            p_radio = alfp_select_radio(p_outbox->pp_radios, p_outbox->n_radios, p_outbox->bridge_id, now_ms, p_outbox->fresh_ms);
            p_reason = "fallback";
        }
        if (!p_radio)
        {
            p_outbox->no_radio_waits++;
            p_outbox->retry_at_ms = now_ms + NO_RADIO_RECHECK_MS;
            return;
        }
        log_size_choice(p_outbox, path, size_bytes, p_radio, p_reason);
    }
    if ((NULL != p_outbox->p_prepare_meta) && (0u != meta_len))
    {
        p_outbox->p_prepare_meta(meta, meta_len, p_outbox->p_prepare_user);
    }
    alfp_err_t err = alfp_send_file(p_outbox->p_alfp, p_radio, p_outbox->bridge_id, path, meta, meta_len);
    if (ALFP_OK == err)
    {
        p_outbox->active_seq = scan_res.min;
    }
    else if ((ALFP_E_OPEN == err) || (ALFP_E_EMPTY == err) || (ALFP_E_TOO_BIG == err) || (ALFP_E_ARG == err))
    {
        drop_job(p_outbox, scan_res.min); /* can never succeed */
    }
    else
    {
        /* transient error (e.g. busy): keep the job and retry on a later pump */
    }
}

void alfp_outbox_set_size_threshold(alfp_outbox_t* p_outbox, uint32_t threshold)
{
    p_outbox->size_threshold = threshold;
}

void alfp_outbox_on_tx_done(alfp_outbox_t* p_outbox, const alfp_tx_report_t* p_rep)
{
    if (!p_outbox->active_seq)
    {
        return;
    }
    uint32_t now_ms = now(p_outbox);
    if (ALFP_TX_DONE == p_rep->result)
    {
        drop_job(p_outbox, p_outbox->active_seq);
        p_outbox->sent_ok++;
        p_outbox->backoff_ms = 0;
        p_outbox->retry_at_ms = now_ms;
    }
    else if ((ALFP_TX_REFUSED == p_rep->result) && (ALFP_ACK_TOO_BIG == p_rep->ack_status))
    {
        drop_job(p_outbox, p_outbox->active_seq);
        p_outbox->retry_at_ms = now_ms;
    }
    else
    {
        /* Bridge unreachable or busy: back off (5 s, 10 s, 20 s ... 60 s) and try again. */
        p_outbox->backoff_ms = p_outbox->backoff_ms ? p_outbox->backoff_ms * 2u : BACKOFF_MIN_MS;
        if (p_outbox->backoff_ms > BACKOFF_MAX_MS)
        {
            p_outbox->backoff_ms = BACKOFF_MAX_MS;
        }
        p_outbox->retry_at_ms = now_ms + p_outbox->backoff_ms;
    }
    p_outbox->active_seq = 0;
}

uint16_t alfp_outbox_count(const alfp_outbox_t* p_outbox)
{
    return p_outbox->count;
}
