/*
 * phototest_main.c - send real photos from one Pico's microSD card to another Pico over LoRa.
 *                    OWNER: Member 2.  Uses Mridula's LoRa mesh (third_party/lora_mesh) unchanged.
 *
 * Two images are built from this file:
 *   lora_photo_tx.uf2   node 2, the SENDER: lists the photos on its SD card, sends one on command
 *   lora_photo_rx.uf2   node 1, the RECEIVER (bridge): reassembles, checks CRC-32, saves to its
 *                       SD card and streams the photo to the laptop over USB
 *
 * Sender keys (serial monitor, 115200):
 *   l  list photos        1-9  send photo N        a  send all, one after another
 *   s  statistics         d    toggle 30% packet loss (demo)     g  toggle gap 150 / 500 ms
 * Receiver keys:
 *   l  list received files   s  statistics   d  toggle 30% packet loss
 *
 * How you know it worked (see docs/LORA_PHOTO_TEST.md):
 *   1. the receiver prints "RECEIVED ... CRC-32 <x> verified" and the sender prints "DONE ... CRC-32 <x>":
 *      the two CRC-32 values must be identical;
 *   2. tools/bridge_receiver.py on the laptop saves the photo again and re-checks the CRC; open it;
 *   3. optionally put the receiver's SD card in a laptop and compare inbox/ with the original.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "alfp.h"
#include "capture_meta.h"
#include "lora_mesh.h"
#include "lora_radio.h"
#include "pico/rand.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "board.h"
#include "storage_fatfs.h"
#include "uplink.h"

#ifndef NODE_ID
#define NODE_ID 2
#endif
#define RECEIVER_ID 1
#define IS_RECEIVER (NODE_ID == RECEIVER_ID)
#define MAX_PHOTOS 9
#define MAX_FILE_BYTES (ALFP_MAX_FRAGS * 241u) /* 123,392 bytes */

static alfp_ctx_t g_alfp;
static const mesh_radio_t* const g_lora = &lora_mesh_radio;

/* Timing for this two-board test. With only two Picos nothing relays, so a short gap is safe.
 * 'g' switches to the mesh default (500 ms) for tests with a relay node in between. */
static alfp_profile_t g_prof[1] = {
    {"lora", 150u, 150u, 1500u, 60000u, 40u, 10u, 30u, 1u, 4u, ALFP_RADIO_LORA},
};

typedef struct
{
    char path[ALFP_PATH_MAX];
    uint32_t size;
} photo_t;

static photo_t g_photos[MAX_PHOTOS];
static int g_n_photos;
static int g_send_all_next = -1; /* index of the next photo in an "a" run, -1 = none */
static int g_sending = -1;
static uint32_t g_capture_id;

/* ---- platform --------------------------------------------------------------------------- */

static uint32_t p_now(void* c)
{
    (void)c;
    return to_ms_since_boot(get_absolute_time());
}
static uint32_t p_rand(void* c)
{
    (void)c;
    return get_rand_32();
}
static void p_log(void* c, const char* m)
{
    (void)c;
    printf("%s\n", m);
}
static const alfp_platform_t k_plat = {NULL, p_now, p_rand, p_log};

static uint16_t frags_for(uint32_t size)
{
    return (uint16_t)((size + 240u) / 241u);
}

static void link_quality(void)
{
    int8_t rssi, snr;
    lora_mesh_last_link_quality(&rssi, &snr);
    printf("  last packet heard: RSSI %d dBm, SNR %d dB\n", rssi, snr);
}

static void print_stats(void)
{
    mesh_stats_t m;
    g_lora->get_stats(&m);
    const alfp_stats_t* a = &g_alfp.stats;
    printf("LoRa: tx %lu rx %lu delivered %lu relayed %lu dup %lu rx_error %lu tx_fail %lu drop_injected %lu\n",
           (unsigned long)m.tx, (unsigned long)m.rx, (unsigned long)m.delivered, (unsigned long)m.relayed,
           (unsigned long)m.dup_dropped, (unsigned long)m.rx_error, (unsigned long)m.tx_fail,
           (unsigned long)m.drop_injected);
    printf("ALFP: packets in %lu, bad CRC-16 %lu, sent ok %lu, failed %lu, received ok %lu, CRC-32 fails %lu\n",
           (unsigned long)a->pkts_in, (unsigned long)a->pkts_bad_crc, (unsigned long)a->tx_ok,
           (unsigned long)a->tx_failed, (unsigned long)a->rx_ok, (unsigned long)a->rx_crc_fail);
    link_quality();
    uint32_t t;
    uint8_t other = IS_RECEIVER ? 2 : RECEIVER_ID;
    if (g_lora->last_heard(other, &t))
    {
        printf("  node %u last heard %lu ms ago\n", other, (unsigned long)(p_now(NULL) - t));
    }
    else
    {
        printf("  node %u not heard yet\n", other);
    }
}

/* ---- SD helpers ----------------------------------------------------------------------- */

static bool is_photo_name(const char* n)
{
    const char* dot = strrchr(n, '.');
    if (!dot)
    {
        return false;
    }
    char ext[8] = {0};
    for (int i = 0; i < 7 && dot[i + 1]; i++)
    {
        ext[i] = (char)tolower((unsigned char)dot[i + 1]);
    }
    return !strcmp(ext, "jpg") || !strcmp(ext, "jpeg") || !strcmp(ext, "png") || !strcmp(ext, "bmp") ||
           !strcmp(ext, "gif");
}

static const char* g_scan_dir;

static void scan_cb(const char* name, void* user)
{
    (void)user;
    if (g_n_photos >= MAX_PHOTOS || !is_photo_name(name) || name[0] == '.')
    {
        return;
    }
    photo_t* p = &g_photos[g_n_photos];
    if (g_scan_dir[0])
    {
        snprintf(p->path, sizeof(p->path), "%s/%s", g_scan_dir, name);
    }
    else
    {
        snprintf(p->path, sizeof(p->path), "%s", name);
    }
    const alfp_storage_t* st = storage_fatfs();
    int h = st->open_read(st->ctx, p->path);
    if (h < 0)
    {
        return;
    }
    p->size = (uint32_t)st->size(st->ctx, h);
    st->close(st->ctx, h);
    g_n_photos++;
}

static void scan_photos(void)
{
    const alfp_storage_t* st = storage_fatfs();
    g_n_photos = 0;
    g_scan_dir = "";
    st->list(st->ctx, "", scan_cb, NULL);
    g_scan_dir = "photos";
    st->list(st->ctx, "photos", scan_cb, NULL);
    g_scan_dir = "samples";
    st->list(st->ctx, "samples", scan_cb, NULL);
}

static void list_photos(void)
{
    printf("\nPhotos on this SD card (root, photos/, samples/):\n");
    if (!g_n_photos)
    {
        printf("  none found. Copy .jpg files onto the card and press l again.\n");
        return;
    }
    for (int i = 0; i < g_n_photos; i++)
    {
        uint16_t f = frags_for(g_photos[i].size);
        double est = f * (0.40 + g_prof[0].gap_ms / 1000.0) + 2.0;
        if (g_photos[i].size > MAX_FILE_BYTES)
        {
            printf("  %d  %-32s %7lu bytes  TOO BIG: max %u bytes, resize it (tools/make_sample_images.py)\n",
                   i + 1, g_photos[i].path, (unsigned long)g_photos[i].size, (unsigned)MAX_FILE_BYTES);
        }
        else
        {
            printf("  %d  %-32s %7lu bytes  %3u fragments  ~%.0f s over LoRa\n", i + 1, g_photos[i].path,
                   (unsigned long)g_photos[i].size, f, est);
        }
    }
    printf("Press 1-%d to send one, a to send all.\n\n", g_n_photos);
}

/* ---- sender ------------------------------------------------------------------------------ */

static bool start_photo(int i)
{
    if (i < 0 || i >= g_n_photos)
    {
        printf("no photo %d (press l)\n", i + 1);
        return false;
    }
    if (alfp_tx_active(&g_alfp))
    {
        printf("busy sending, wait for DONE\n");
        return false;
    }
    /* Metadata: the standard 27-byte capture record, then the original file name. */
    capture_meta_t m;
    capture_meta_clear(&m);
    m.node_id = NODE_ID;
    m.capture_id = ++g_capture_id;
    m.uptime_ms = p_now(NULL);
    m.image_index = (uint8_t)i;
    uint8_t meta[ALFP_MAX_META];
    uint8_t ml = capture_meta_pack(&m, meta);
    const char* base = strrchr(g_photos[i].path, '/');
    base = base ? base + 1 : g_photos[i].path;
    size_t bl = strlen(base);
    if (bl > ALFP_MAX_META - ml)
    {
        bl = ALFP_MAX_META - ml;
    }
    memcpy(meta + ml, base, bl);
    ml = (uint8_t)(ml + bl);

    alfp_err_t e = alfp_send_file(&g_alfp, g_lora, RECEIVER_ID, g_photos[i].path, meta, ml);
    if (e != ALFP_OK)
    {
        const char* why = e == ALFP_E_TOO_BIG ? "too big (resize to under 120 kB)"
                          : e == ALFP_E_OPEN  ? "cannot open file"
                          : e == ALFP_E_EMPTY ? "file is empty"
                                              : "error";
        printf("cannot send %s: %s\n", g_photos[i].path, why);
        return false;
    }
    g_sending = i;
    printf("\nSENDING %s: %lu bytes as %u fragments of 241 bytes to node %u over LoRa\n", g_photos[i].path,
           (unsigned long)g_photos[i].size, frags_for(g_photos[i].size), RECEIVER_ID);
    return true;
}

static void on_tx_done(alfp_ctx_t* ctx, const alfp_tx_report_t* r)
{
    (void)ctx;
    double secs = (r->finished_ms - r->started_ms) / 1000.0;
    if (r->result == ALFP_TX_DONE)
    {
        printf("DONE  %s: receiver confirmed it. %lu bytes, CRC-32 %08lx, %u fragments, %u rounds, %u resent, "
               "%.1f s (%.0f bytes/s)\n",
               r->path, (unsigned long)r->file_size, (unsigned long)r->file_crc32, r->frag_count, r->rounds,
               r->data_resent, secs, secs > 0 ? r->file_size / secs : 0.0);
    }
    else
    {
        printf("FAILED %s: %s after %u rounds, %.1f s. %s\n", r->path, alfp_tx_result_str(r->result), r->rounds,
               secs,
               r->why == 1 ? "No answer from the receiver: is it powered, flashed with lora_photo_rx, in range?"
                           : "");
    }
    g_sending = -1;
}

/* ---- receiver ---------------------------------------------------------------------------- */

static void uplink_out(const char* s, void* user)
{
    (void)user;
    fputs(s, stdout);
}

static void on_rx_done(alfp_ctx_t* ctx, const alfp_rx_report_t* r)
{
    const alfp_storage_t* st = ctx->cfg.storage;
    /* Give the file back its original name: inbox/n2_00042_photo1.jpg */
    char name[40] = "photo.bin";
    uint8_t rec = capture_meta_record_len(r->meta, r->meta_len); /* 27 (v1) or 31 (v2) */
    if (rec != 0u && r->meta_len > rec)
    {
        size_t n = r->meta_len - rec;
        if (n >= sizeof(name))
        {
            n = sizeof(name) - 1;
        }
        memcpy(name, r->meta + rec, n);
        name[n] = 0;
    }
    char final_path[ALFP_PATH_MAX + 48];
    snprintf(final_path, sizeof(final_path), "inbox/n%u_%05u_%s", r->src, r->sid, name);
    const char* path = st->rename(st->ctx, r->path, final_path) == 0 ? final_path : r->path;

    double secs = (r->done_ms - r->first_ms) / 1000.0;
    printf("\nRECEIVED %s from node %u: %lu bytes, CRC-32 %08lx verified, %u fragments, %u duplicates ignored, "
           "%.1f s\n",
           path, r->src, (unsigned long)r->file_size, (unsigned long)r->file_crc32, r->frag_count, r->duplicates,
           secs);
    link_quality();

    alfp_rx_report_t copy = *r;
    copy.path = path;
    uplink_send_file(st, &copy, uplink_out, NULL); /* the laptop's bridge_receiver.py saves this */
    fflush(stdout);
}

static void list_cb(const char* name, void* user)
{
    (void)user;
    char p[ALFP_PATH_MAX + 8];
    snprintf(p, sizeof(p), "inbox/%s", name);
    const alfp_storage_t* st = storage_fatfs();
    int h = st->open_read(st->ctx, p);
    if (h >= 0)
    {
        printf("  %-40s %7ld bytes\n", p, (long)st->size(st->ctx, h));
        st->close(st->ctx, h);
    }
}

static void receiver_progress(void)
{
    for (unsigned i = 0; i < ALFP_RX_SESSIONS; i++)
    {
        const alfp_rx_t* r = &g_alfp.rx[i];
        if (r->state == RX_ACTIVE && !r->pending_ack && r->frag_count)
        {
            printf("  receiving from node %u: %u of %u fragments%s\n", r->src, r->n_have, r->frag_count,
                   r->have_manifest ? "" : " (manifest not yet received)");
        }
    }
}

/* ---- main ----------------------------------------------------------------------------------- */

int main(void)
{
    board_spi1_idle(); /* LoRa CS + SD CS high: they share SPI1 */
    stdio_init_all();
    uint32_t t0 = p_now(NULL);
    while (!stdio_usb_connected() && p_now(NULL) - t0 < 4000)
    {
        sleep_ms(10);
    }
    printf("\n=== LoRa photo test: node %d, %s ===\n", NODE_ID, IS_RECEIVER ? "RECEIVER" : "SENDER");
    printf("SD card on SPI%d: SCK GP%d, MOSI GP%d, MISO GP%d, CS GP%d\n", SD_SPI_INST, SD_SCK, SD_MOSI, SD_MISO,
           SD_CS);

    while (!storage_fatfs_mount())
    {
        printf("  check: card inserted, FAT32, wired to the pins above (not the Maker Pi Pico slot)\n");
        sleep_ms(3000);
    }
    const alfp_storage_t* st = storage_fatfs();
    printf("SD card mounted\n");

    while (!lora_mesh_init(NODE_ID))
    {
        printf("LoRa init failed: check the Core1121 wiring (RESET GP5, SCK GP10, MOSI GP11, MISO GP12, CS GP13, "
               "BUSY GP6, DIO9 GP7) and power. Retrying in 2 s\n");
        sleep_ms(2000);
    }
    board_spi1_after_lora();
    printf("LoRa ready: %u bytes per packet, a full packet takes %lu ms on air\n", g_lora->max_payload(),
           (unsigned long)lora_radio_time_on_air_ms(255));

    alfp_cfg_t cfg = {
        .node_id = NODE_ID,
        .bridge_id = RECEIVER_ID,
        .storage = st,
        .platform = &k_plat,
        .inbox_dir = "inbox",
        .tmp_dir = "rx",
        .on_tx_done = on_tx_done,
        .on_rx_done = on_rx_done,
        .profiles = g_prof,
        .n_profiles = 1,
    };
    alfp_init(&g_alfp, &cfg);

    if (IS_RECEIVER)
    {
        lora_mesh_set_heartbeat(3000); /* lets the sender see us in its stats ("last heard") */
        printf("Waiting for photos. Keys: l list received, s stats, d toggle 30%% loss\n\n");
    }
    else
    {
        scan_photos();
        list_photos();
        printf("Keys: l list, 1-9 send, a send all, s stats, d toggle 30%% loss, g toggle gap 150/500 ms\n\n");
    }

    const mesh_radio_t* radios[1] = {g_lora};
    bool dropping = false;
    uint32_t next_progress = p_now(NULL) + 3000;
    uint16_t last_sent_print = 0;

    for (;;)
    {
        g_lora->poll();
        alfp_service(&g_alfp, radios, 1);

        /* "a": start the next photo once the previous one has finished */
        if (!IS_RECEIVER && g_send_all_next >= 0 && !alfp_tx_active(&g_alfp))
        {
            int i = g_send_all_next;
            g_send_all_next = (i + 1 < g_n_photos) ? i + 1 : -1;
            start_photo(i);
        }

        uint32_t now = p_now(NULL);
        if ((int32_t)(now - next_progress) >= 0)
        {
            next_progress = now + 3000;
            if (IS_RECEIVER)
            {
                receiver_progress();
            }
            else if (alfp_tx_active(&g_alfp) && g_alfp.tx.rep.data_sent != last_sent_print)
            {
                last_sent_print = g_alfp.tx.rep.data_sent;
                printf("  sent %u data packets (round %u, %u of them resends)\n", g_alfp.tx.rep.data_sent,
                       g_alfp.tx.round, g_alfp.tx.rep.data_resent);
            }
            else
            {
                /* sender idle, or nothing new sent since the last print */
            }
        }

        int c = getchar_timeout_us(0);
        if (c == 's')
        {
            print_stats();
        }
        else if (c == 'd')
        {
            dropping = !dropping;
            g_lora->set_drop_rate(dropping ? 0.3f : 0.0f);
            printf("injected packet loss on this node: %s\n", dropping ? "30%" : "off");
        }
        else if (c == 'l')
        {
            if (IS_RECEIVER)
            {
                printf("Received files on this SD card:\n");
                st->list(st->ctx, "inbox", list_cb, NULL);
            }
            else
            {
                scan_photos();
                list_photos();
            }
        }
        else if (!IS_RECEIVER && c >= '1' && c <= '9')
        {
            start_photo(c - '1');
        }
        else if (!IS_RECEIVER && c == 'a')
        {
            g_send_all_next = g_n_photos ? 0 : -1;
            printf("sending all %d photos\n", g_n_photos);
        }
        else if (!IS_RECEIVER && c == 'g')
        {
            uint32_t g = g_prof[0].gap_ms == 150u ? 500u : 150u;
            g_prof[0].gap_ms = g_prof[0].gap_max_ms = g;
            printf("gap between packets: %lu ms (applies to the next photo)\n", (unsigned long)g);
        }
        else
        {
            /* no key, or a key with no command: ignore */
        }
        sleep_ms(1);
    }
}
