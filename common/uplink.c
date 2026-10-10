/*
 * uplink.c - see uplink.h.
 */
#include "uplink.h"

#include <stdio.h>
#include <string.h>

#include "alfp_crc.h"

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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

int uplink_send_file(const alfp_storage_t* st, const alfp_rx_report_t* r, uplink_write_fn out, void* user)
{
    int h = st->open_read(st->ctx, r->path);
    if (h < 0)
    {
        return -1;
    }
    char line[64 + 200];
    char meta_hex[2 * ALFP_MAX_META + 1];
    for (unsigned i = 0; i < r->meta_len && i < ALFP_MAX_META; i++)
    {
        snprintf(meta_hex + 2 * i, 3, "%02x", r->meta[i]);
    }
    meta_hex[2 * (r->meta_len < ALFP_MAX_META ? r->meta_len : ALFP_MAX_META)] = 0;
    snprintf(line, sizeof(line), "@ALFP FILE v1 src=%u sid=%05u size=%lu crc=%08lx radio=%s frags=%u meta=",
             (unsigned)r->src, (unsigned)r->sid, (unsigned long)r->file_size, (unsigned long)r->file_crc32,
             r->radio ? r->radio : "?", (unsigned)r->frag_count);
    out(line, user);
    out(meta_hex, user);
    out("\n", user);

    uint8_t buf[48];
    uint32_t off = 0, crc = ALFP_CRC32_INIT;
    while (off < r->file_size)
    {
        uint32_t left = r->file_size - off;
        uint16_t n = (uint16_t)(left < sizeof(buf) ? left : sizeof(buf));
        if (st->read_at(st->ctx, h, off, buf, n) != (int)n)
        {
            st->close(st->ctx, h);
            out("@ALFP ABORT read error\n", user);
            return -1;
        }
        crc = alfp_crc32(crc, buf, n);
        b64_line(buf, n, line);
        out(line, user);
        off += n;
    }
    st->close(st->ctx, h);
    snprintf(line, sizeof(line), "@ALFP END crc=%08lx\n", (unsigned long)crc);
    out(line, user);
    return crc == r->file_crc32 ? 0 : -1;
}
