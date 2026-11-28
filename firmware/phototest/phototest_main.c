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
#include "float_types.h"
#include <inttypes.h>
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
static const mesh_radio_t* const gp_lora = &g_lora_mesh_radio;

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

static uint32_t p_now(void* p_ctx)
{
    (void)p_ctx;
    return to_ms_since_boot(get_absolute_time());
}
static uint32_t p_rand(void* p_ctx)
{
    (void)p_ctx;
    return get_rand_32();
}
static void p_log(void* p_ctx, const char* p_msg)
{
    (void)p_ctx;
    printf("%s\n", p_msg);
}
static const alfp_platform_t k_plat = {NULL, p_now, p_rand, p_log};

static uint16_t frags_for(uint32_t size)
{
    return (uint16_t)((size + 240u) / 241u);
}

static void link_quality(void)
{
    int8_t rssi;
    int8_t snr;
    lora_mesh_last_link_quality(&rssi, &snr);
    printf("  last packet heard: RSSI %d dBm, SNR %d dB\n", rssi, snr);
}

static void print_stats(void)
{
    mesh_stats_t stats;
    gp_lora->p_get_stats(&stats);
    const alfp_stats_t* p_stats = &g_alfp.stats;
    printf("LoRa: tx %" PRIu32 " rx %" PRIu32 " delivered %" PRIu32 " relayed %" PRIu32 " dup %" PRIu32 " rx_error %" PRIu32 " tx_fail %" PRIu32 " drop_injected %" PRIu32 "\n",
           (uint32_t)stats.tx_count, (uint32_t)stats.rx_count, (uint32_t)stats.delivered, (uint32_t)stats.relayed,
           (uint32_t)stats.dup_dropped, (uint32_t)stats.rx_error, (uint32_t)stats.tx_fail,
           (uint32_t)stats.drop_injected);
    printf("ALFP: packets in %" PRIu32 ", bad CRC-16 %" PRIu32 ", sent ok %" PRIu32 ", failed %" PRIu32 ", received ok %" PRIu32 ", CRC-32 fails %" PRIu32 "\n",
           (uint32_t)p_stats->pkts_in, (uint32_t)p_stats->pkts_bad_crc, (uint32_t)p_stats->tx_ok,
           (uint32_t)p_stats->tx_failed, (uint32_t)p_stats->rx_ok, (uint32_t)p_stats->rx_crc_fail);
    link_quality();
    uint32_t now_ms;
    uint8_t other = IS_RECEIVER ? 2 : RECEIVER_ID;
    if (gp_lora->p_last_heard(other, &now_ms))
    {
        printf("  node %u last heard %" PRIu32 " ms ago\n", other, (uint32_t)(p_now(NULL) - now_ms));
    }
    else
    {
        printf("  node %u not heard yet\n", other);
    }
}

/* ---- SD helpers ----------------------------------------------------------------------- */

static bool is_photo_name(const char* p_name)
{
    const char* p_dot = strrchr(p_name, '.');
    if (!p_dot)
    {
        return false;
    }
    char ext[8] = {0};
    for (int idx = 0; (idx < 7) && (p_dot[idx + 1]); idx++)
    {
        ext[idx] = (char)tolower((unsigned char)p_dot[idx + 1]);
    }
    return (!strcmp(ext, "jpg")) || (!strcmp(ext, "jpeg")) || (!strcmp(ext, "png")) || (!strcmp(ext, "bmp")) ||
           (!strcmp(ext, "gif"));
}

static const char* gp_scan_dir;

static void scan_cb(const char* p_name, void* p_user)
{
    (void)p_user;
    if ((g_n_photos >= MAX_PHOTOS) || (!is_photo_name(p_name)) || ('.' == p_name[0]))
    {
        return;
    }
    photo_t* p_photo = &g_photos[g_n_photos];
    if (gp_scan_dir[0])
    {
        snprintf(p_photo->path, sizeof(p_photo->path), "%s/%s", gp_scan_dir, p_name);
    }
    else
    {
        snprintf(p_photo->path, sizeof(p_photo->path), "%s", p_name);
    }
    const alfp_storage_t* p_store = storage_fatfs();
    int handle = p_store->p_open_read(p_store->p_ctx, p_photo->path);
    if (handle < 0)
    {
        return;
    }
    p_photo->size = (uint32_t)p_store->p_size(p_store->p_ctx, handle);
    p_store->p_close(p_store->p_ctx, handle);
    g_n_photos++;
}

static void scan_photos(void)
{
    const alfp_storage_t* p_store = storage_fatfs();
    g_n_photos = 0;
    gp_scan_dir = "";
    p_store->p_list(p_store->p_ctx, "", scan_cb, NULL);
    gp_scan_dir = "photos";
    p_store->p_list(p_store->p_ctx, "photos", scan_cb, NULL);
    gp_scan_dir = "samples";
    p_store->p_list(p_store->p_ctx, "samples", scan_cb, NULL);
}

static void list_photos(void)
{
    printf("\nPhotos on this SD card (root, photos/, samples/):\n");
    if (!g_n_photos)
    {
        printf("  none found. Copy .jpg files onto the card and press l again.\n");
        return;
    }
    for (int idx = 0; idx < g_n_photos; idx++)
    {
        uint16_t frags = frags_for(g_photos[idx].size);
        float64_t est = frags * (0.40 + g_prof[0].gap_ms / 1000.0) + 2.0;
        if (g_photos[idx].size > MAX_FILE_BYTES)
        {
            printf("  %d  %-32s %7" PRIu32 " bytes  TOO BIG: max %u bytes, resize it (tools/make_sample_images.py)\n",
                   idx + 1, g_photos[idx].path, (uint32_t)g_photos[idx].size, (unsigned)MAX_FILE_BYTES);
        }
        else
        {
            printf("  %d  %-32s %7" PRIu32 " bytes  %3u fragments  ~%.0f s over LoRa\n", idx + 1, g_photos[idx].path,
                   (uint32_t)g_photos[idx].size, frags, est);
        }
    }
    printf("Press 1-%d to send one, a to send all.\n\n", g_n_photos);
}

/* ---- sender ------------------------------------------------------------------------------ */

static bool start_photo(int idx)
{
    if ((idx < 0) || (idx >= g_n_photos))
    {
        printf("no photo %d (press l)\n", idx + 1);
        return false;
    }
    if (alfp_tx_active(&g_alfp))
    {
        printf("busy sending, wait for DONE\n");
        return false;
    }
    /* Metadata: the standard 27-byte capture record, then the original file name. */
    capture_meta_t cap_meta;
    capture_meta_clear(&cap_meta);
    cap_meta.node_id = NODE_ID;
    cap_meta.capture_id = ++g_capture_id;
    cap_meta.uptime_ms = p_now(NULL);
    cap_meta.image_index = (uint8_t)idx;
    uint8_t meta[ALFP_MAX_META];
    uint8_t meta_len = capture_meta_pack(&cap_meta, meta);
    const char* p_base = strrchr(g_photos[idx].path, '/');
    p_base = p_base ? p_base + 1 : g_photos[idx].path;
    size_t base_len = strlen(p_base);
    if (base_len > ALFP_MAX_META - meta_len)
    {
        base_len = ALFP_MAX_META - meta_len;
    }
    memcpy(meta + meta_len, p_base, base_len);
    meta_len = (uint8_t)(meta_len + base_len);

    alfp_err_t err = alfp_send_file(&g_alfp, gp_lora, RECEIVER_ID, g_photos[idx].path, meta, meta_len);
    if (ALFP_OK != err)
    {
        const char* p_why = ALFP_E_TOO_BIG == err ? "too big (resize to under 120 kB)"
                          : ALFP_E_OPEN == err  ? "cannot open file"
                          : ALFP_E_EMPTY == err ? "file is empty"
                                              : "error";
        printf("cannot send %s: %s\n", g_photos[idx].path, p_why);
        return false;
    }
    g_sending = idx;
    printf("\nSENDING %s: %" PRIu32 " bytes as %u fragments of 241 bytes to node %u over LoRa\n", g_photos[idx].path,
           (uint32_t)g_photos[idx].size, frags_for(g_photos[idx].size), RECEIVER_ID);
    return true;
}

static void on_tx_done(alfp_ctx_t* p_ctx, const alfp_tx_report_t* p_rep)
{
    (void)p_ctx;
    float64_t secs = (p_rep->finished_ms - p_rep->started_ms) / 1000.0;
    if (ALFP_TX_DONE == p_rep->result)
    {
        printf("DONE  %s: receiver confirmed it. %" PRIu32 " bytes, CRC-32 %08" PRIx32 ", %u fragments, %u rounds, %u resent, "
               "%.1f s (%.0f bytes/s)\n",
               p_rep->p_path, (uint32_t)p_rep->file_size, (uint32_t)p_rep->file_crc32, p_rep->frag_count, p_rep->rounds,
               p_rep->data_resent, secs, secs > 0 ? p_rep->file_size / secs : 0.0);
    }
    else
    {
        printf("FAILED %s: %s after %u rounds, %.1f s. %s\n", p_rep->p_path, alfp_tx_result_str(p_rep->result), p_rep->rounds,
               secs,
               1 == p_rep->why ? "No answer from the receiver: is it powered, flashed with lora_photo_rx, in range?"
                           : "");
    }
    g_sending = -1;
}

/* ---- receiver ---------------------------------------------------------------------------- */

static void uplink_out(const char* p_str, void* p_user)
{
    (void)p_user;
    fputs(p_str, stdout);
}

static void on_rx_done(alfp_ctx_t* p_ctx, const alfp_rx_report_t* p_rep)
{
    const alfp_storage_t* p_store = p_ctx->cfg.p_storage;
    /* Give the file back its original name: inbox/n2_00042_photo1.jpg */
    char name[40] = "photo.bin";
    uint8_t rec = capture_meta_record_len(p_rep->p_meta, p_rep->meta_len); /* 27 (v1) or 31 (v2) */
    if ((0u != rec) && (p_rep->meta_len > rec))
    {
        size_t len = p_rep->meta_len - rec;
        if (len >= sizeof(name))
        {
            len = sizeof(name) - 1;
        }
        memcpy(name, p_rep->p_meta + rec, len);
        name[len] = 0;
    }
    char final_path[ALFP_PATH_MAX + 48];
    snprintf(final_path, sizeof(final_path), "inbox/n%u_%05u_%s", p_rep->src, p_rep->sid, name);
    const char* p_path = 0 == p_store->p_rename(p_store->p_ctx, p_rep->p_path, final_path) ? final_path : p_rep->p_path;

    float64_t secs = (p_rep->done_ms - p_rep->first_ms) / 1000.0;
    printf("\nRECEIVED %s from node %u: %" PRIu32 " bytes, CRC-32 %08" PRIx32 " verified, %u fragments, %u duplicates ignored, "
           "%.1f s\n",
           p_path, p_rep->src, (uint32_t)p_rep->file_size, (uint32_t)p_rep->file_crc32, p_rep->frag_count, p_rep->duplicates,
           secs);
    link_quality();

    alfp_rx_report_t copy = *p_rep;
    copy.p_path = p_path;
    uplink_send_file(p_store, &copy, uplink_out, NULL); /* the laptop's bridge_receiver.py saves this */
    fflush(stdout);
}

static void list_cb(const char* p_name, void* p_user)
{
    (void)p_user;
    char path[ALFP_PATH_MAX + 8];
    snprintf(path, sizeof(path), "inbox/%s", p_name);
    const alfp_storage_t* p_store = storage_fatfs();
    int handle = p_store->p_open_read(p_store->p_ctx, path);
    if (handle >= 0)
    {
        printf("  %-40s %7" PRId32 " bytes\n", path, (int32_t)p_store->p_size(p_store->p_ctx, handle));
        p_store->p_close(p_store->p_ctx, handle);
    }
}

static void receiver_progress(void)
{
    for (unsigned idx = 0; idx < ALFP_RX_SESSIONS; idx++)
    {
        const alfp_rx_t* p_session = &g_alfp.rx_sessions[idx];
        if ((RX_ACTIVE == p_session->state) && (!p_session->b_pending_ack) && (p_session->frag_count))
        {
            printf("  receiving from node %u: %u of %u fragments%s\n", p_session->src, p_session->n_have, p_session->frag_count,
                   p_session->b_have_manifest ? "" : " (manifest not yet received)");
        }
    }
}

/* ---- main ----------------------------------------------------------------------------------- */

int main(void)
{
    board_spi1_idle(); /* LoRa CS + SD CS high: they share SPI1 */
    stdio_init_all();
    uint32_t start_t = p_now(NULL);
    while ((!stdio_usb_connected()) && (p_now(NULL) - start_t < 4000))
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
    const alfp_storage_t* p_store = storage_fatfs();
    printf("SD card mounted\n");

    while (!lora_mesh_init(NODE_ID))
    {
        printf("LoRa init failed: check the Core1121 wiring (RESET GP5, SCK GP10, MOSI GP11, MISO GP12, CS GP13, "
               "BUSY GP6, DIO9 GP7) and power. Retrying in 2 s\n");
        sleep_ms(2000);
    }
    board_spi1_after_lora();
    printf("LoRa ready: %u bytes per packet, a full packet takes %" PRIu32 " ms on air\n", gp_lora->p_max_payload(),
           (uint32_t)lora_radio_time_on_air_ms(255));

    alfp_cfg_t cfg = {
        .node_id = NODE_ID,
        .bridge_id = RECEIVER_ID,
        .p_storage = p_store,
        .p_platform = &k_plat,
        .p_inbox_dir = "inbox",
        .p_tmp_dir = "rx",
        .p_on_tx_done = on_tx_done,
        .p_on_rx_done = on_rx_done,
        .p_profiles = g_prof,
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

    const mesh_radio_t* radios[1] = {gp_lora};
    bool b_dropping = false;
    uint32_t next_progress = p_now(NULL) + 3000;
    uint16_t last_sent_print = 0;

    for (;;)
    {
        gp_lora->p_poll();
        alfp_service(&g_alfp, radios, 1);

        /* "a": start the next photo once the previous one has finished */
        if ((!IS_RECEIVER) && (g_send_all_next >= 0) && (!alfp_tx_active(&g_alfp)))
        {
            int idx = g_send_all_next;
            g_send_all_next = (idx + 1 < g_n_photos) ? idx + 1 : -1;
            start_photo(idx);
        }

        uint32_t now = p_now(NULL);
        if ((int32_t)(now - next_progress) >= 0)
        {
            next_progress = now + 3000;
            if (IS_RECEIVER)
            {
                receiver_progress();
            }
            else if ((alfp_tx_active(&g_alfp)) && (g_alfp.tx_session.rep.data_sent != last_sent_print))
            {
                last_sent_print = g_alfp.tx_session.rep.data_sent;
                printf("  sent %u data packets (round %u, %u of them resends)\n", g_alfp.tx_session.rep.data_sent,
                       g_alfp.tx_session.round, g_alfp.tx_session.rep.data_resent);
            }
            else
            {
                /* sender idle, or nothing new sent since the last print */
            }
        }

        int chr = getchar_timeout_us(0);
        if ('s' == chr)
        {
            print_stats();
        }
        else if ('d' == chr)
        {
            b_dropping = !b_dropping;
            gp_lora->p_set_drop_rate(b_dropping ? 0.3f : 0.0f);
            printf("injected packet loss on this node: %s\n", b_dropping ? "30%" : "off");
        }
        else if ('l' == chr)
        {
            if (IS_RECEIVER)
            {
                printf("Received files on this SD card:\n");
                p_store->p_list(p_store->p_ctx, "inbox", list_cb, NULL);
            }
            else
            {
                scan_photos();
                list_photos();
            }
        }
        else if ((!IS_RECEIVER) && (chr >= '1') && (chr <= '9'))
        {
            start_photo(chr - '1');
        }
        else if ((!IS_RECEIVER) && ('a' == chr))
        {
            g_send_all_next = g_n_photos ? 0 : -1;
            printf("sending all %d photos\n", g_n_photos);
        }
        else if ((!IS_RECEIVER) && ('g' == chr))
        {
            uint32_t gap_ms = 150u == g_prof[0].gap_ms ? 500u : 150u;
            g_prof[0].gap_ms = g_prof[0].gap_max_ms = gap_ms;
            printf("gap between packets: %" PRIu32 " ms (applies to the next photo)\n", (uint32_t)gap_ms);
        }
        else
        {
            /* no key, or a key with no command: ignore */
        }
        sleep_ms(1);
    }
}
