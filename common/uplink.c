/*
 * uplink.c - see uplink.h.
 */
#include "uplink.h"

#include <stdio.h>
#include <string.h>

#include "alfp_crc.h"

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* "@D " + 64 base64 characters + "\n" + NUL, with room to spare; also holds the header line. */
#define LINE_MAX_CHARS (64 + 200)

static void b64_line(const uint8_t* in, int n, char* out)
{
    int o = 0;
    out[o++] = '@';
    out[o++] = 'D';
    out[o++] = ' ';
    for (int i = 0; i < n; i += 3)
    {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n)
        {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (i + 2 < n)
        {
            v |= in[i + 2];
        }
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o++] = '\n';
    out[o] = 0;
}

static void copy_report(uplink_job_t* j, const alfp_rx_report_t* r)
{
    memset(j, 0, sizeof(*j));
    j->src = r->src;
    j->sid = r->sid;
    snprintf(j->radio, sizeof(j->radio), "%s", r->radio ? r->radio : "?");
    snprintf(j->path, sizeof(j->path), "%s", r->path ? r->path : "");
    j->file_size = r->file_size;
    j->frag_count = r->frag_count;
    j->file_crc32 = r->file_crc32;
    j->meta_len = r->meta_len < ALFP_MAX_META ? r->meta_len : (uint8_t)ALFP_MAX_META;
    if (r->meta != NULL && j->meta_len != 0u)
    {
        memcpy(j->meta, r->meta, j->meta_len);
    }
}

/* "@ALFP FILE ... meta=" + hex + "\n", written as three pieces exactly like the original code. */
static void write_header(const uplink_job_t* j, uplink_write_fn out, void* user)
{
    char line[LINE_MAX_CHARS];
    char meta_hex[2 * ALFP_MAX_META + 1];
    for (unsigned i = 0; i < j->meta_len; i++)
    {
        snprintf(meta_hex + 2 * i, 3, "%02x", j->meta[i]);
    }
    meta_hex[2 * j->meta_len] = 0;
    snprintf(line, sizeof(line),
             "@ALFP FILE v1 src=%u sid=%05u size=%lu crc=%08lx radio=%s frags=%u meta=", (unsigned)j->src,
             (unsigned)j->sid, (unsigned long)j->file_size, (unsigned long)j->file_crc32, j->radio,
             (unsigned)j->frag_count);
    out(line, user);
    out(meta_hex, user);
    out("\n", user);
}

/* Read the next chunk of the file into q->buf. The file stays open from the first chunk to the
 * END line (one SD handle; opening it again per chunk meant one directory lookup per chunk). */
static bool fill_chunk(uplink_queue_t* q, const uplink_job_t* j)
{
    uint32_t left = j->file_size - q->off;
    uint16_t n = (uint16_t)(left < UPLINK_CHUNK_BYTES ? left : UPLINK_CHUNK_BYTES);
    if (q->h < 0)
    {
        q->h = q->st->open_read(q->st->ctx, j->path);
        if (q->h < 0)
        {
            return false;
        }
    }
    bool ok = q->st->read_at(q->st->ctx, q->h, q->off, q->buf, n) == (int)n;
    q->buf_len = ok ? n : 0u;
    q->buf_pos = 0;
    return ok;
}

static void finish_job(uplink_queue_t* q, bool ok)
{
    if (q->h >= 0)
    {
        q->st->close(q->st->ctx, q->h);
        q->h = -1;
    }
    if (ok)
    {
        q->sent_ok++;
    }
    else
    {
        q->sent_bad++;
    }
    q->head = (uint8_t)((q->head + 1u) % UPLINK_QUEUE_DEPTH);
    q->count--;
    q->started = false;
}

void uplink_queue_init(uplink_queue_t* q, const alfp_storage_t* st)
{
    memset(q, 0, sizeof(*q));
    q->st = st;
    q->h = -1;
}

bool uplink_queue_add(uplink_queue_t* q, const alfp_rx_report_t* r)
{
    if (q->count >= UPLINK_QUEUE_DEPTH)
    {
        q->queue_full++;
        return false;
    }
    copy_report(&q->jobs[(q->head + q->count) % UPLINK_QUEUE_DEPTH], r);
    q->count++;
    if (q->count > q->max_waiting)
    {
        q->max_waiting = q->count;
    }
    return true;
}

unsigned uplink_queue_waiting(const uplink_queue_t* q)
{
    return q->count;
}

unsigned uplink_queue_service(uplink_queue_t* q, unsigned max_lines, uplink_write_fn out, void* user)
{
    unsigned lines = 0;
    char line[LINE_MAX_CHARS];
    while (lines < max_lines && q->count != 0u)
    {
        const uplink_job_t* j = &q->jobs[q->head];
        if (!q->started)
        {
            write_header(j, out, user);
            q->started = true;
            q->off = 0;
            q->crc = ALFP_CRC32_INIT;
            q->buf_len = 0;
            q->buf_pos = 0;
            lines++;
            continue;
        }
        if (q->off >= j->file_size)
        {
            snprintf(line, sizeof(line), "@ALFP END crc=%08lx\n", (unsigned long)q->crc);
            out(line, user);
            lines++;
            finish_job(q, q->crc == j->file_crc32);
            continue;
        }
        if (q->buf_pos >= q->buf_len && !fill_chunk(q, j))
        {
            out("@ALFP ABORT read error\n", user);
            lines++;
            finish_job(q, false);
            continue;
        }
        uint16_t n = (uint16_t)(q->buf_len - q->buf_pos);
        if (n > UPLINK_LINE_BYTES)
        {
            n = UPLINK_LINE_BYTES;
        }
        q->crc = alfp_crc32(q->crc, q->buf + q->buf_pos, n);
        b64_line(q->buf + q->buf_pos, n, line);
        out(line, user);
        q->buf_pos = (uint16_t)(q->buf_pos + n);
        q->off += n;
        lines++;
    }
    return lines;
}

int uplink_send_file(const alfp_storage_t* st, const alfp_rx_report_t* r, uplink_write_fn out, void* user)
{
    /* the queue is large (one chunk buffer + jobs), so a static one; this is only ever called from
     * one task at a time (the bridge's comms task, the self-test, the host tests) */
    static uplink_queue_t q;
    int h = st->open_read(st->ctx, r->path);
    if (h < 0)
    {
        return -1;
    }
    st->close(st->ctx, h);
    uplink_queue_init(&q, st);
    (void)uplink_queue_add(&q, r);
    while (uplink_queue_waiting(&q) != 0u)
    {
        (void)uplink_queue_service(&q, 64u, out, user);
    }
    return q.sent_ok == 1u ? 0 : -1;
}
