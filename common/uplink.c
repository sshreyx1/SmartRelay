/*
 * uplink.c - see uplink.h.
 */
#include "uplink.h"

#include <stdio.h>
#include <string.h>

#include "alfp_crc.h"

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* "@D " + 64 base64 characters + "\n" + NUL, with room to spare; also holds the header line. */
#define LINE_MAX_CHARS (64 + 200)

static void b64_line(const uint8_t* p_input, int len, char* p_out)
{
    int off = 0;
    p_out[off++] = '@';
    p_out[off++] = 'D';
    p_out[off++] = ' ';
    for (int idx = 0; idx < len; idx += 3)
    {
        uint32_t value = (uint32_t)p_input[idx] << 16;
        if (idx + 1 < len)
        {
            value |= (uint32_t)p_input[idx + 1] << 8;
        }
        if (idx + 2 < len)
        {
            value |= p_input[idx + 2];
        }
        p_out[off++] = b64[(value >> 18) & 63];
        p_out[off++] = b64[(value >> 12) & 63];
        p_out[off++] = idx + 1 < len ? b64[(value >> 6) & 63] : '=';
        p_out[off++] = idx + 2 < len ? b64[value & 63] : '=';
    }
    p_out[off++] = '\n';
    p_out[off] = 0;
}

static void copy_report(uplink_job_t* p_jdx, const alfp_rx_report_t* p_rep)
{
    memset(p_jdx, 0, sizeof(*p_jdx));
    p_jdx->src = p_rep->src;
    p_jdx->sid = p_rep->sid;
    snprintf(p_jdx->radio, sizeof(p_jdx->radio), "%s", p_rep->p_radio ? p_rep->p_radio : "?");
    snprintf(p_jdx->path, sizeof(p_jdx->path), "%s", p_rep->p_path ? p_rep->p_path : "");
    p_jdx->file_size = p_rep->file_size;
    p_jdx->frag_count = p_rep->frag_count;
    p_jdx->file_crc32 = p_rep->file_crc32;
    p_jdx->first_ms = p_rep->first_ms;
    p_jdx->done_ms = p_rep->done_ms;
    p_jdx->nacks = p_rep->nacks_sent;
    p_jdx->dups = p_rep->duplicates;
    p_jdx->meta_len = p_rep->meta_len < ALFP_MAX_META ? p_rep->meta_len : (uint8_t)ALFP_MAX_META;
    if (p_rep->p_meta != NULL && p_jdx->meta_len != 0u)
    {
        memcpy(p_jdx->meta, p_rep->p_meta, p_jdx->meta_len);
    }
}

/* "@ALFP FILE ... meta=<hex>\n" as ONE write, so a log line printed between two writes can never land
 * inside it. meta= stays last (its hex runs to the end of the line). */
static void write_header(const uplink_job_t* p_jdx, uplink_write_fn p_out, void* p_user)
{
    char line[200 + 2 * ALFP_MAX_META];
    int len = snprintf(line, sizeof(line),
                     "@ALFP FILE v1 src=%u sid=%05u size=%lu crc=%08lx radio=%s frags=%u first=%lu done=%lu nacks=%u "
                     "dups=%u meta=",
                     (unsigned)p_jdx->src, (unsigned)p_jdx->sid, (unsigned long)p_jdx->file_size, (unsigned long)p_jdx->file_crc32,
                     p_jdx->radio, (unsigned)p_jdx->frag_count, (unsigned long)p_jdx->first_ms, (unsigned long)p_jdx->done_ms,
                     (unsigned)p_jdx->nacks, (unsigned)p_jdx->dups);
    size_t off = (len > 0 && (size_t)len < sizeof(line)) ? (size_t)len : 0u;
    for (unsigned idx = 0; idx < p_jdx->meta_len && off + 3u < sizeof(line); idx++)
    {
        snprintf(line + off, 3, "%02x", p_jdx->meta[idx]);
        off += 2u;
    }
    line[off++] = '\n';
    line[off] = 0;
    p_out(line, p_user);
}

/* Read the next chunk of the file into q->buf. The file stays open from the first chunk to the
 * END line (one SD handle; opening it again per chunk meant one directory lookup per chunk). */
static bool fill_chunk(uplink_queue_t* p_queue, const uplink_job_t* p_jdx)
{
    uint32_t left = p_jdx->file_size - p_queue->off;
    uint16_t len = (uint16_t)(left < UPLINK_CHUNK_BYTES ? left : UPLINK_CHUNK_BYTES);
    if (p_queue->handle < 0)
    {
        p_queue->handle = p_queue->p_store->p_open_read(p_queue->p_store->p_ctx, p_jdx->path);
        if (p_queue->handle < 0)
        {
            return false;
        }
    }
    bool b_is_ok = p_queue->p_store->p_read_at(p_queue->p_store->p_ctx, p_queue->handle, p_queue->off, p_queue->buf, len) == (int)len;
    p_queue->buf_len = b_is_ok ? len : 0u;
    p_queue->buf_pos = 0;
    return b_is_ok;
}

static void finish_job(uplink_queue_t* p_queue, bool b_is_ok)
{
    if (p_queue->handle >= 0)
    {
        p_queue->p_store->p_close(p_queue->p_store->p_ctx, p_queue->handle);
        p_queue->handle = -1;
    }
    if (b_is_ok)
    {
        p_queue->sent_ok++;
    }
    else
    {
        p_queue->sent_bad++;
    }
    p_queue->head = (uint8_t)((p_queue->head + 1u) % UPLINK_QUEUE_DEPTH);
    p_queue->count--;
    p_queue->b_started = false;
}

void uplink_queue_init(uplink_queue_t* p_queue, const alfp_storage_t* p_store)
{
    memset(p_queue, 0, sizeof(*p_queue));
    p_queue->p_store = p_store;
    p_queue->handle = -1;
}

bool uplink_queue_add(uplink_queue_t* p_queue, const alfp_rx_report_t* p_rep)
{
    if (p_queue->count >= UPLINK_QUEUE_DEPTH)
    {
        p_queue->queue_full++;
        return false;
    }
    copy_report(&p_queue->jobs[(p_queue->head + p_queue->count) % UPLINK_QUEUE_DEPTH], p_rep);
    p_queue->count++;
    if (p_queue->count > p_queue->max_waiting)
    {
        p_queue->max_waiting = p_queue->count;
    }
    return true;
}

unsigned uplink_queue_waiting(const uplink_queue_t* p_queue)
{
    return p_queue->count;
}

unsigned uplink_queue_service(uplink_queue_t* p_queue, unsigned max_lines, uplink_write_fn p_out, void* p_user)
{
    unsigned lines = 0;
    char line[LINE_MAX_CHARS];
    while (lines < max_lines && p_queue->count != 0u)
    {
        const uplink_job_t* p_jdx = &p_queue->jobs[p_queue->head];
        if (!p_queue->b_started)
        {
            write_header(p_jdx, p_out, p_user);
            p_queue->b_started = true;
            p_queue->off = 0;
            p_queue->crc = ALFP_CRC32_INIT;
            p_queue->buf_len = 0;
            p_queue->buf_pos = 0;
            lines++;
            continue;
        }
        if (p_queue->off >= p_jdx->file_size)
        {
            snprintf(line, sizeof(line), "@ALFP END crc=%08lx\n", (unsigned long)p_queue->crc);
            p_out(line, p_user);
            lines++;
            finish_job(p_queue, p_queue->crc == p_jdx->file_crc32);
            continue;
        }
        if (p_queue->buf_pos >= p_queue->buf_len && !fill_chunk(p_queue, p_jdx))
        {
            p_out("@ALFP ABORT read error\n", p_user);
            lines++;
            finish_job(p_queue, false);
            continue;
        }
        uint16_t len = (uint16_t)(p_queue->buf_len - p_queue->buf_pos);
        if (len > UPLINK_LINE_BYTES)
        {
            len = UPLINK_LINE_BYTES;
        }
        p_queue->crc = alfp_crc32(p_queue->crc, p_queue->buf + p_queue->buf_pos, len);
        b64_line(p_queue->buf + p_queue->buf_pos, len, line);
        p_out(line, p_user);
        p_queue->buf_pos = (uint16_t)(p_queue->buf_pos + len);
        p_queue->off += len;
        lines++;
    }
    return lines;
}

int uplink_send_file(const alfp_storage_t* p_store, const alfp_rx_report_t* p_rep, uplink_write_fn p_out, void* p_user)
{
    /* the queue is large (one chunk buffer + jobs), so a static one; this is only ever called from
     * one task at a time (the bridge's comms task, the self-test, the host tests) */
    static uplink_queue_t queue;
    int handle = p_store->p_open_read(p_store->p_ctx, p_rep->p_path);
    if (handle < 0)
    {
        return -1;
    }
    p_store->p_close(p_store->p_ctx, handle);
    uplink_queue_init(&queue, p_store);
    (void)uplink_queue_add(&queue, p_rep);
    while (uplink_queue_waiting(&queue) != 0u)
    {
        (void)uplink_queue_service(&queue, 64u, p_out, p_user);
    }
    return queue.sent_ok == 1u ? 0 : -1;
}
