/*
 * main.c - node firmware for CSC09 trail mesh (all five nodes; node 1 is also the bridge).
 *          OWNER: Member 2 (integration + fragmentation + microSD)
 *
 * One image per node is built: csc09_node_1.uf2 (A, bridge) ... csc09_node_5.uf2 (E).
 * Pins: app/board.h.
 *
 * FreeRTOS tasks
 *   comms   (priority 3)  owns BOTH radios and the SD card. Every 2 ms: radio poll(), ALFP receive,
 *                         timers and pacing, outbox pump, capture requests, bridge uplink.
 *                         Single owner = no locks needed in the mesh code, and FatFs (not
 *                         thread-safe) is only ever touched here.
 *   sensors (priority 2)  Maryam's and Rhaylene's code (sensing/sensing.c): DHT22 + light + weather
 *                         tree every 2 s; when PIR AND the sound trigger are high together it records
 *                         5 s of audio features and calls app_request_capture(). The Maker button
 *                         (GP20) or the console 'c' forces a capture with real sensor values.
 *   console (priority 1)  USB serial commands, see print_help().
 *
 * microSD layout
 *   samples/<prefix>_NN.jpg  stand-in camera images, chosen by what the node heard: dog_00.jpg,
 *                            bird_00.jpg (group fallback), img_00.jpg (generic fallback)
 *                            (sensing/sound_image_pick.h; tools/make_label_images.py makes them)
 *   meta/next_id             next capture id, so ids never restart after a reboot (common/capture_id.h)
 *   outbox/q00000017.job     queued captures (store-and-forward), survive reboots
 *   rx/ n3_00042.part        bridge: partial transfers
 *   inbox/n3_00042.bin/.meta bridge: finished, verified files (+ metadata)
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "alfp.h"
#include "alfp_outbox.h"
#include "board.h"
#include "capture_api.h"
#include "capture_id.h"
#include "hardware/gpio.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "queue.h"
#include "radios.h"
#include "sensing.h"
#include "sound_classes.h"
#include "sound_image_pick.h"
#include "storage_fatfs.h"
#include "task.h"
#include "uplink.h"

#ifndef NODE_ID
#define NODE_ID 2
#endif
#define BRIDGE_ID 1
#define BRIDGE_FRESH_MS 10000u

/* Task A.2: send capture metadata as its own tiny ALFP transfer, separate from the image, so the
 * size rule (APP_RADIO_SIZE_THRESHOLD, alfp_config.h) routes it over LoRa while the image goes
 * over WiFi. 0 restores the single combined transfer this firmware always sent before (needed
 * while SOUND_LEGACY_MODE's demo flow is validated against the new path). */
#ifndef APP_SPLIT_TELEMETRY
#define APP_SPLIT_TELEMETRY 1
#endif
/* Explicit marker in the telemetry job's filename-tail (the bytes after the 31-byte v2 meta record,
 * see common/capture_meta.h), so the backend/tools/bridge_receiver.py side can tell "no image in
 * this transfer" apart from the pre-existing, still-valid case of a transfer with a real image
 * and simply no name tail at all (some callers never appended one before this round). Never a
 * real image name: no image this firmware ever picks ends without an extension. Kept in sync by
 * hand with tools/bridge_receiver.py's TELEMETRY_MARKER and backend/app.py's own copy. */
#define APP_TELEMETRY_MARKER "TELEMETRY"

/* TRANSFER_VISIBILITY.md 3.3: the bridge prints one "@EVT ..." line per receive event (session
 * start, manifest, every fragment, NACK, done, failure) so the dashboard on the bridge's laptop can
 * show a transfer while it is still arriving. 0 = off. */
#ifndef APP_TRANSFER_EVENTS
#define APP_TRANSFER_EVENTS 1
#endif
/* Print every Nth first-try fragment as an RXF line (resends and duplicates are always printed).
 * 1 = every fragment (~40 lines for a 9 KB image). Raise it if "longest comms loop" grows. */
#ifndef APP_RXF_EVERY
#define APP_RXF_EVERY 1
#endif
/* TRANSFER_VISIBILITY.md 3.5: every node prints, once a second and only when it relayed something,
 * how many packets it relayed and whose they were. For whoever stands at that node; 0 = off. */
#ifndef APP_RELAY_LOG
#define APP_RELAY_LOG 1
#endif

#define TEST_PAYLOADS_DIR "test_payloads" /* Task A.3: tools/make_test_payloads.py's size ladder */
#define TEST_PAYLOADS_MAX 16

static alfp_ctx_t g_alfp;
static alfp_outbox_t g_outbox;
static QueueHandle_t gp_capture_q;
static const mesh_radio_t* g_radios[2]; /* preference: WiFi first, then LoRa */
static uint32_t g_capture_id;           /* last id used; restored from the SD card at boot */
static uint32_t g_boot_first_id;        /* first capture id issued in this boot (ids only grow) */
static char g_payload_names[TEST_PAYLOADS_MAX][32];
static uint8_t g_n_payloads;
static uint8_t g_next_payload;
static sip_table_t g_images; /* samples/<prefix>_NN.jpg found at boot */
static volatile bool gb_want_stats;
static volatile bool gb_toggle_drop;
static volatile bool gb_force_capture;
static volatile bool gb_want_env;
static volatile bool gb_toggle_topo;
static volatile bool gb_want_test_payload; /* console 'p' (test A.3): send the next size-ladder file */
static volatile bool gb_toggle_hold;       /* console 'H' (test T6): comms stops taking captures off g_capture_q */
static volatile bool gb_comms_ready;

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

static const alfp_platform_t k_plat = { NULL, p_now, p_rand, p_log };

/* ---- callbacks ---------------------------------------------------------------------------- */

#define TEL_DIR "tel" /* tiny per-capture telemetry files made only by handle_capture(), see below */

static void on_tx_done(alfp_ctx_t* p_ctx, const alfp_tx_report_t* p_rep)
{
    (void)p_ctx;
    printf("tx %s: %s via %s, %u frags, %u rounds, %u resent, %.1f s\n", p_rep->p_path, alfp_tx_result_str(p_rep->result),
           p_rep->p_radio, p_rep->frag_count, p_rep->rounds, p_rep->data_resent, (p_rep->finished_ms - p_rep->started_ms) / 1000.0);
    if ((ALFP_TX_DONE == p_rep->result) && (0 == strncmp(p_rep->p_path, TEL_DIR "/", sizeof(TEL_DIR "/") - 1u)))
    {
        /* The telemetry job's "file" was a temp copy of its own meta made just for this transfer
         * (unlike the image, which is a reused sample on the SD card): remove it once sent, same
         * as the outbox already removes the job descriptor itself in alfp_outbox_on_tx_done(). */
        storage_fatfs()->p_remove(storage_fatfs()->p_ctx, p_rep->p_path);
    }
    alfp_outbox_on_tx_done(&g_outbox, p_rep);
}

/* ---- sender's own serial (TRANSFER_VISIBILITY.md 3.5): only the person at this node sees these ---- */

static void on_tx_start(alfp_ctx_t* p_ctx, const alfp_tx_t* p_tx_session)
{
    (void)p_ctx;
    capture_meta_t meta;
    if (0 != capture_meta_unpack(p_tx_session->meta, p_tx_session->meta_len, &meta))
    {
        printf("tx start: %s %" PRIu32 " B via %s, %u frags x %u B, sid %u\n", p_tx_session->path, (uint32_t)p_tx_session->file_size,
               p_tx_session->p_radio->p_name, p_tx_session->frag_count, p_tx_session->frag_size, p_tx_session->sid);
        return;
    }
    uint8_t rec = capture_meta_record_len(p_tx_session->meta, p_tx_session->meta_len);
    size_t total_len = strlen(APP_TELEMETRY_MARKER);
    bool b_tel = (p_tx_session->meta_len == rec + total_len) && (0 == memcmp(p_tx_session->meta + rec, APP_TELEMETRY_MARKER, total_len));
    char id_str[CAPTURE_ID_STR_MAX];
    capture_id_format(id_str, sizeof(id_str), meta.node_id, meta.capture_id);
    char waited[32] = "";
    if (CM_MISSING_U32 != meta.queued_ms)
    {
        snprintf(waited, sizeof(waited), ", waited %.1f s", meta.queued_ms / 1000.0);
    }
    printf("tx start: capture %s %s %" PRIu32 " B via %s, %u frags x %u B, sid %u%s\n", id_str, b_tel ? "telemetry" : "image",
           (uint32_t)p_tx_session->file_size, p_tx_session->p_radio->p_name, p_tx_session->frag_count, p_tx_session->frag_size, p_tx_session->sid, waited);
}

static void on_tx_nack(alfp_ctx_t* p_ctx, uint16_t sid, uint8_t round, uint16_t missing)
{
    (void)p_ctx;
    if (0u != missing)
    {
        printf("tx: bridge missing %u fragment%s (round %u, sid %u), resending\n", missing, 1u == missing ? "" : "s",
               round, sid);
    }
}

/* Once a second: "relay wifi: 26 pkts in last 1 s (from node 5: 26)", per radio, only if any. */
static void relay_log(void)
{
    static uint16_t last[2][6];
    static bool b_primed;
    for (int idx = 0; idx < 2; idx++)
    {
        char from[96] = "";
        size_t off = 0;
        unsigned total = 0;
        for (uint8_t src = 1; src <= 5; src++)
        {
            uint16_t now = radios_relayed_from(g_radios[idx], src);
            unsigned delta = (uint16_t)(now - last[idx][src]); /* wraps cleanly at 65536 */
            last[idx][src] = now;
            if (b_primed && (0u != delta) && (off < sizeof(from)))
            {
                int len = snprintf(from + off, sizeof(from) - off, "%snode %u: %u", total ? ", " : "", src, delta);
                off += len > 0 ? (size_t)len : 0u;
                total += delta;
            }
        }
        if (0u != total)
        {
            printf("relay %s: %u pkts in last 1 s (from %s)\n", g_radios[idx]->p_name, total, from);
        }
    }
    b_primed = true;
}

static void uplink_out(const char* p_str, void* p_user)
{
    (void)p_user;
    fputs(p_str, stdout);
}

/* Bridge -> laptop: finished files wait here and go out UPLINK_LINES_PER_PASS lines per comms
 * pass (uplink.h). Streaming the whole file from inside on_rx_done (the old way) stopped both
 * radios for seconds and held back the sender's ACK; see firmware_smp/app/main_smp.c. */
static uplink_queue_t g_uplink;
#define UPLINK_LINES_PER_PASS 8u
static uint32_t g_longest_comms_loop_ms;

/* ---- transfer events (bridge -> laptop, TRANSFER_VISIBILITY.md 3.3) --------------------------- */

/* Route of the packet alfp_handle_packet() is working on right now (set by comms_service()). */
static uint8_t g_pkt_hops;
static uint8_t g_pkt_prev;

/* One whole line in one write, through the same stdout the uplink uses, so an event line can sit
 * between two "@D" lines but can never end up inside one. */
static void evt_line(const char* p_fmt, ...)
{
    char line[300];
    va_list args;
    va_start(args, p_fmt);
    int printed = vsnprintf(line, sizeof(line) - 1u, p_fmt, args);
    va_end(args);
    if (printed < 0)
    {
        return;
    }
    size_t len = (size_t)printed < sizeof(line) - 2u ? (size_t)printed : sizeof(line) - 2u;
    line[len++] = '\n';
    line[len] = 0;
    fputs(line, stdout);
    fflush(stdout);
}

static void ev_rx_start(alfp_ctx_t* p_ctx, uint8_t src, uint16_t sid, const char* p_radio)
{
    (void)p_ctx;
    evt_line("@EVT RXS src=%u sid=%u radio=%s t=%" PRIu32, src, sid, p_radio, (uint32_t)p_now(NULL));
}

static void ev_rx_manifest(alfp_ctx_t* p_ctx, uint8_t src, uint16_t sid, uint16_t frag_count, uint16_t frag_size,
                           uint32_t file_size, uint32_t file_crc32, const uint8_t* p_meta, uint8_t meta_len)
{
    (void)p_ctx;
    char hex[2 * ALFP_MAX_META + 1];
    unsigned len = meta_len < ALFP_MAX_META ? meta_len : ALFP_MAX_META;
    for (unsigned idx = 0; idx < len; idx++)
    {
        snprintf(hex + 2 * idx, 3, "%02x", p_meta[idx]);
    }
    hex[2 * len] = 0;
    evt_line("@EVT RXM src=%u sid=%u frags=%u fsz=%u size=%" PRIu32 " crc=%08" PRIx32 " meta=%s t=%" PRIu32, src, sid, frag_count,
             frag_size, (uint32_t)file_size, (uint32_t)file_crc32, hex, (uint32_t)p_now(NULL));
}

static void ev_rx_frag(alfp_ctx_t* p_ctx, uint8_t src, uint16_t sid, uint16_t seq, uint8_t round, bool b_dup)
{
    (void)p_ctx;
    if ((APP_RXF_EVERY > 1) && (0 == round) && (!b_dup) && (0 != seq % APP_RXF_EVERY))
    {
        return;
    }
    evt_line("@EVT RXF src=%u sid=%u seq=%u r=%u h=%u v=%u d=%u t=%" PRIu32, src, sid, seq, round, g_pkt_hops, g_pkt_prev,
             b_dup ? 1u : 0u, (uint32_t)p_now(NULL));
}

static void ev_rx_nack(alfp_ctx_t* p_ctx, uint8_t src, uint16_t sid, uint8_t round, uint16_t missing)
{
    (void)p_ctx;
    evt_line("@EVT NACK src=%u sid=%u r=%u miss=%u t=%" PRIu32, src, sid, round, missing, (uint32_t)p_now(NULL));
}

static void ev_rx_fail(alfp_ctx_t* p_ctx, uint8_t src, uint16_t sid, const char* p_why)
{
    (void)p_ctx;
    evt_line("@EVT RXFAIL src=%u sid=%u why=%s t=%" PRIu32, src, sid, p_why, (uint32_t)p_now(NULL));
}

static void on_rx_done(alfp_ctx_t* p_ctx, const alfp_rx_report_t* p_rep)
{
    if (APP_TRANSFER_EVENTS)
    {
        evt_line("@EVT RXOK src=%u sid=%u first=%" PRIu32 " done=%" PRIu32 " nacks=%u dups=%u frags=%u t=%" PRIu32, p_rep->src, p_rep->sid,
                 (uint32_t)p_rep->first_ms, (uint32_t)p_rep->done_ms, p_rep->nacks_sent, p_rep->duplicates, p_rep->frag_count,
                 (uint32_t)p_now(NULL));
    }
    printf("rx from node %u: %s, %" PRIu32 " bytes via %s, %u dup\n", p_rep->src, p_rep->p_path, (uint32_t)p_rep->file_size, p_rep->p_radio,
           p_rep->duplicates);
    if (!uplink_queue_add(&g_uplink, p_rep))
    {
        printf("uplink: queue full (%u files), sending %s inline\n", (unsigned)UPLINK_QUEUE_DEPTH, p_rep->p_path);
        uplink_send_file(p_ctx->cfg.p_storage, p_rep, uplink_out, NULL);
        fflush(stdout);
    }
}

/* ---- capture requests (any task) --------------------------------------------------------- */

bool app_request_capture(const capture_meta_t* p_sensors)
{
    return gp_capture_q && (xQueueSend(gp_capture_q, p_sensors, 0) == pdTRUE);
}

static void count_samples_cb(const char* p_name, void* p_user)
{
    (void)p_user;
    sip_add_file(&g_images, p_name);
}

static void count_payloads_cb(const char* p_name, void* p_user)
{
    (void)p_user;
    if ('.' == p_name[0])
    {
        return; /* F1: macOS "._*" AppleDouble files and other hidden files are not payloads */
    }
    size_t name_len = strlen(p_name);
    if ((g_n_payloads < TEST_PAYLOADS_MAX) && (name_len < sizeof(g_payload_names[0])))
    {
        memcpy(g_payload_names[g_n_payloads], p_name, name_len + 1u); /* + the NUL strlen() does not count */
        g_n_payloads++;
    }
}

/* Task A.3: send one file from the size ladder (tools/make_test_payloads.py) with no capture
 * metadata, so a test can watch the tx log's size=/thr=/radio=/reason= line pick LoRa for the
 * small rungs and WiFi for the large ones. Cycles through every file found at boot. */
static void send_next_test_payload(void)
{
    if (!g_n_payloads)
    {
        printf("test payloads: none found on %s/\n", TEST_PAYLOADS_DIR);
        return;
    }
    char path[ALFP_PATH_MAX];
    snprintf(path, sizeof(path), TEST_PAYLOADS_DIR "/%s", g_payload_names[g_next_payload]);
    if (alfp_outbox_add(&g_outbox, path, NULL, 0))
    {
        printf("test payload queued: %s\n", path);
    }
    else
    {
        printf("test payload: SD write failed (%s)\n", path);
    }
    g_next_payload = (uint8_t)((g_next_payload + 1u) % g_n_payloads);
}

/* ---- persistent capture id (comms only: it owns the SD card) ------------------------------ */

#define ID_FILE "meta/next_id"
#define ID_TMP "meta/next_id.tmp"

static uint32_t read_u32_file(const alfp_storage_t* p_store, const char* p_path)
{
    int handle = p_store->p_open_read(p_store->p_ctx, p_path);
    if (handle < 0)
    {
        return 0;
    }
    char txt[16] = { 0 };
    int len = p_store->p_read_at(p_store->p_ctx, handle, 0, (uint8_t*)txt, sizeof(txt) - 1);
    p_store->p_close(p_store->p_ctx, handle);
    uint32_t value = 0;
    return (len > 0) && (1 == sscanf(txt, "%" SCNu32, &value)) ? value : 0;
}

/* write to a temp file, then rename over the real one: a power cut leaves one of the two intact */
static void store_next_id(const alfp_storage_t* p_store, uint32_t next)
{
    char txt[16];
    int len = snprintf(txt, sizeof(txt), "%" PRIu32 "\n", (uint32_t)next);
    int handle = p_store->p_open_write(p_store->p_ctx, ID_TMP);
    bool b_is_ok = (handle >= 0) && (p_store->p_write_at(p_store->p_ctx, handle, 0, (const uint8_t*)txt, (uint16_t)len) == len);
    if (handle >= 0)
    {
        p_store->p_close(p_store->p_ctx, handle);
    }
    if ((!b_is_ok) || (0 != p_store->p_rename(p_store->p_ctx, ID_TMP, ID_FILE)))
    {
        printf("capture id: could not save %s (ids may repeat after a reboot)\n", ID_FILE);
    }
}

static uint32_t s_job_seq[ALFP_OUTBOX_MAX + 8];
static uint32_t s_n_jobs;
static uint32_t s_highest_seen;

static void scan_outbox_cb(const char* p_name, void* p_user)
{
    (void)p_user;
    uint32_t seq;
    if (('q' == p_name[0]) && (1 == sscanf(p_name + 1, "%8lu", &seq)) && (s_n_jobs < sizeof(s_job_seq) / sizeof(s_job_seq[0])))
    {
        s_job_seq[s_n_jobs++] = (uint32_t)seq;
    }
}

static void scan_clips_cb(const char* p_name, void* p_user)
{
    (void)p_user;
    uint8_t node;
    uint32_t cap_id;
    if ((capture_id_parse(p_name, &node, &cap_id)) && (NODE_ID == node) && (cap_id > s_highest_seen))
    {
        s_highest_seen = cap_id;
    }
}

/* Highest capture id still on the card: queued jobs (their meta) and stored clips (their names). */
static uint32_t highest_id_on_card(const alfp_storage_t* p_store)
{
    s_n_jobs = 0;
    s_highest_seen = 0;
    p_store->p_list(p_store->p_ctx, "outbox", scan_outbox_cb, NULL);
    for (uint32_t idx = 0; idx < s_n_jobs; idx++)
    {
        char path[32];
        snprintf(path, sizeof(path), "outbox/q%08" PRIu32 ".job", (uint32_t)s_job_seq[idx]);
        uint8_t buf[2 + 1 + ALFP_PATH_MAX + 1 + ALFP_MAX_META];
        int handle = p_store->p_open_read(p_store->p_ctx, path);
        if (handle < 0)
        {
            continue;
        }
        int len = p_store->p_read_at(p_store->p_ctx, handle, 0, buf, sizeof(buf));
        p_store->p_close(p_store->p_ctx, handle);
        /* job file (alfp_outbox.c): 'Q' '1' | path_len | path | meta_len | meta */
        if ((len < 4) || ('Q' != buf[0]) || ('1' != buf[1]) || (4 + buf[2] > len))
        {
            continue;
        }
        const uint8_t* p_meta = buf + 4 + buf[2];
        uint8_t total_len = buf[3 + buf[2]];
        capture_meta_t meta;
        if ((p_meta + total_len <= buf + len) && (0 == capture_meta_unpack(p_meta, total_len, &meta)) && (meta.capture_id > s_highest_seen))
        {
            s_highest_seen = meta.capture_id;
        }
    }
    p_store->p_list(p_store->p_ctx, "clips", scan_clips_cb, NULL);
    return s_highest_seen;
}

/* Outbox hook (TRANSFER_VISIBILITY.md 3.4): just before a job is sent, write how long it waited
 * since its trigger into the v2 record's queued_ms. Only for captures made in THIS boot: uptime_ms
 * of a job queued before a reboot is from the old boot, so its wait cannot be measured and stays
 * missing (0xFFFFFFFF) instead of a made-up number. */
static void stamp_queued_ms(uint8_t* p_meta, uint8_t len, void* p_user)
{
    (void)p_user;
    capture_meta_t meta;
    if ((len < CAPTURE_META_LEN_V2) || (2u != p_meta[0]) || (0 != capture_meta_unpack(p_meta, len, &meta)))
    {
        return;
    }
    uint32_t now = p_now(NULL);
    if ((NODE_ID == meta.node_id) && (meta.capture_id >= g_boot_first_id) && (0u != g_boot_first_id) && (now >= meta.uptime_ms))
    {
        cm_put32(p_meta + CAPTURE_META_QUEUED_OFF, now - meta.uptime_ms);
    }
}

static void restore_capture_id(const alfp_storage_t* p_store)
{
    p_store->p_mkdir(p_store->p_ctx, "meta");
    uint32_t next = read_u32_file(p_store, ID_FILE);
    uint32_t tmp = read_u32_file(p_store, ID_TMP);
    next = tmp > next ? tmp : next;
    uint32_t seen = highest_id_on_card(p_store);
    g_capture_id = next > seen + 1u ? next - 1u : seen;
    g_boot_first_id = g_capture_id + 1u;
    char id_str[CAPTURE_ID_STR_MAX];
    capture_id_format(id_str, sizeof(id_str), NODE_ID, g_capture_id + 1u);
    printf("capture id: next is %s (saved next %" PRIu32 ", highest on card %" PRIu32 ")\n", id_str, (uint32_t)next,
           (uint32_t)seen);
}

static void handle_capture(const capture_meta_t* p_request)
{
    const alfp_storage_t* p_store = storage_fatfs();
    capture_meta_t meta = *p_request;
    meta.node_id = NODE_ID;
    meta.capture_id = ++g_capture_id;
    store_next_id(p_store, g_capture_id + 1u);
    if (0 == meta.uptime_ms)
    {
        meta.uptime_ms = p_now(NULL);
    }
    char id_str[CAPTURE_ID_STR_MAX];
    capture_id_format(id_str, sizeof(id_str), NODE_ID, meta.capture_id);

    /* the image follows the sound: label -> its group -> generic img_NN (legacy: always img_NN, round robin) */
    /* Follow-up F6: sip_pick() now falls back to any group, so it fails only on a card with no
     * <prefix>_NN.jpg at all; even then the telemetry below is still sent. */
    char name[ALFP_MAX_META - CAPTURE_META_LEN + 1];
    int image_num = sip_pick(&g_images, SOUND_LEGACY_MODE ? SC_UNKNOWN : meta.sound_class, name, sizeof(name));
    char path[ALFP_PATH_MAX] = "";
    if (image_num >= 0)
    {
        meta.image_index = (uint8_t)image_num;
        snprintf(path, sizeof(path), "samples/%s", name);
    }
    const char* p_shown = image_num >= 0 ? path : "no image (samples/ has no <prefix>_NN.jpg), telemetry only";
    if (CM_UNKNOWN_U8 == meta.sound_class)
    {
        printf("capture %s: heard unknown (recording failed) -> %s\n", id_str, p_shown);
    }
    else if (CM_UNKNOWN_U8 == meta.sound_conf) /* legacy thresholds give no confidence */
    {
        printf("capture %s: heard %s -> %s\n", id_str, sound_class_name(meta.sound_class), p_shown);
    }
    else
    {
        printf("capture %s: heard %s (%u%%) -> %s\n", id_str, sound_class_name(meta.sound_class), meta.sound_conf, p_shown);
    }

    /* meta = the 31-byte v2 record + the image's file name (the optional-name tail, <= 64 bytes in all) */
    uint8_t meta_buf[ALFP_MAX_META];
    uint8_t meta_len = capture_meta_pack(&meta, meta_buf);

    if (APP_SPLIT_TELEMETRY || (image_num < 0))
    {
        /* Telemetry: a tiny transfer tagged with APP_TELEMETRY_MARKER instead of an image name,
         * small enough that the size rule always sends it over LoRa. The "file" is just a temp
         * copy of the same meta bytes -- removed from the SD card by on_tx_done() once it is
         * confirmed sent. */
        char tel_path[ALFP_PATH_MAX];
        char stem[CAPTURE_ID_STR_MAX];
        capture_id_file_stem(stem, sizeof(stem), NODE_ID, meta.capture_id);
        snprintf(tel_path, sizeof(tel_path), TEL_DIR "/%s.bin", stem);
        int tel_handle = p_store->p_open_write(p_store->p_ctx, tel_path);
        bool b_tel_ok = (tel_handle >= 0) && (p_store->p_write_at(p_store->p_ctx, tel_handle, 0, meta_buf, meta_len) == meta_len);
        if (tel_handle >= 0)
        {
            p_store->p_close(p_store->p_ctx, tel_handle);
        }
        uint8_t tel_meta[ALFP_MAX_META];
        memcpy(tel_meta, meta_buf, meta_len);
        size_t marker_len = strlen(APP_TELEMETRY_MARKER);
        memcpy(tel_meta + meta_len, APP_TELEMETRY_MARKER, marker_len);
        uint8_t tel_meta_len = (uint8_t)(meta_len + marker_len);
        if (b_tel_ok && (alfp_outbox_add(&g_outbox, tel_path, tel_meta, tel_meta_len)))
        {
            printf("capture %s telemetry queued (%s)\n", id_str, tel_path);
        }
        else
        {
            printf("capture %s: telemetry SD write failed%s\n", id_str, image_num >= 0 ? ", image still sent" : "");
            p_store->p_remove(p_store->p_ctx, tel_path);
        }
    }
    if (image_num < 0)
    {
        return;
    }

    /* Image: meta + the image's file name (the optional-name tail), exactly as before this round. */
    size_t name_len = strlen(name);
    memcpy(meta_buf + meta_len, name, name_len);
    uint8_t total_len = (uint8_t)(meta_len + name_len);
    if (alfp_outbox_add(&g_outbox, path, meta_buf, total_len))
    {
        printf("capture %s queued (%s), outbox %u\n", id_str, path, alfp_outbox_count(&g_outbox));
    }
    else
    {
        printf("capture %s: SD write failed\n", id_str);
    }
}

/* ---- tasks -------------------------------------------------------------------------------- */

/* alfp_service() with the route of each packet noted first (TRANSFER_VISIBILITY.md 3.3): same bound
 * (16 packets per radio per pass) and the same copy-out, since ALFP reuses its packet buffer for
 * replies. The receive callbacks read g_pkt_hops / g_pkt_prev for the packet being handled. */
static void comms_service(void)
{
    static uint8_t input[ALFP_MAX_PKT];
    for (int idx = 0; idx < 2; idx++)
    {
        const mesh_radio_t* p_radio = g_radios[idx];
        for (int kdx = 0; kdx < 16; kdx++)
        {
            uint8_t src;
            int len = p_radio->p_recv(&src, input, sizeof(input));
            if (MESH_RECV_NONE == len)
            {
                break;
            }
            if (len < 0)
            {
                g_alfp.stats.pkts_bad_other++;
                continue;
            }
            if (!radios_last_recv_info(p_radio, &g_pkt_hops, &g_pkt_prev))
            {
                g_pkt_hops = 0;
                g_pkt_prev = 0;
            }
            alfp_handle_packet(&g_alfp, p_radio, src, input, (uint16_t)len);
        }
    }
    alfp_step(&g_alfp);
}

static void print_stats(void)
{
    const alfp_stats_t* p_stats = &g_alfp.stats;
    printf("node %u | alfp in %" PRIu32 " badcrc %" PRIu32 " | tx ok %" PRIu32 " fail %" PRIu32 " | rx ok %" PRIu32 " crcfail %" PRIu32 " expired %" PRIu32 " | outbox %u\n",
           (unsigned)NODE_ID, (uint32_t)p_stats->pkts_in, (uint32_t)p_stats->pkts_bad_crc, (uint32_t)p_stats->tx_ok,
           (uint32_t)p_stats->tx_failed, (uint32_t)p_stats->rx_ok, (uint32_t)p_stats->rx_crc_fail,
           (uint32_t)p_stats->rx_expired, alfp_outbox_count(&g_outbox));
    for (int idx = 0; idx < 2; idx++)
    {
        mesh_stats_t stats;
        g_radios[idx]->p_get_stats(&stats);
        printf("  %s: tx %" PRIu32 " rx %" PRIu32 " delivered %" PRIu32 " relayed %" PRIu32 " dup %" PRIu32 " rxerr %" PRIu32 "\n", g_radios[idx]->p_name,
               (uint32_t)stats.tx_count, (uint32_t)stats.rx_count, (uint32_t)stats.delivered, (uint32_t)stats.relayed,
               (uint32_t)stats.dup_dropped, (uint32_t)stats.rx_error);
    }
    const sensing_env_t* p_env = sensing_env();
    printf("  dht22 good %" PRIu32 " fail %" PRIu32 " | lora %s | wifi %s\n", (uint32_t)p_env->dht_good, (uint32_t)p_env->dht_fail,
           radios_lora_ok() ? "up" : "DOWN", radios_wifi_ok() ? "up" : "DOWN");
    uint32_t now_ms;
    for (int idx = 0; idx < 2; idx++)
    {
        if (g_radios[idx]->p_last_heard(BRIDGE_ID, &now_ms))
        {
            printf("  bridge last heard on %s %" PRIu32 " ms ago\n", g_radios[idx]->p_name, (uint32_t)(p_now(NULL) - now_ms));
        }
        else
        {
            printf("  bridge never heard on %s\n", g_radios[idx]->p_name);
        }
    }
    printf("  comms stack free: %" PRIu32 " words, heap free %" PRIu32 " bytes\n", (uint32_t)uxTaskGetStackHighWaterMark(NULL),
           (uint32_t)xPortGetFreeHeapSize());
    printf("  longest comms loop %" PRIu32 " ms | outbox dropped (full) %" PRIu32, (uint32_t)g_longest_comms_loop_ms,
           (uint32_t)g_outbox.dropped_full);
    if (NODE_ID == BRIDGE_ID)
    {
        printf(" | uplink sent %" PRIu32 " bad %" PRIu32 " waiting %u (max %u) inline %" PRIu32, (uint32_t)g_uplink.sent_ok,
               (uint32_t)g_uplink.sent_bad, uplink_queue_waiting(&g_uplink), (unsigned)g_uplink.max_waiting,
               (uint32_t)g_uplink.queue_full);
    }
    printf("\n");
}

static void comms_task(void* p_arg)
{
    (void)p_arg;
    /* The SD card goes first: it must be switched to SPI mode before any LoRa traffic appears on
     * the shared SPI1 bus (board.h). */
    while (!storage_fatfs_mount())
    {
        printf("sd: mount failed (card in the Maker slot? FAT32?), retrying\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    const alfp_storage_t* p_store = storage_fatfs();
    p_store->p_mkdir(p_store->p_ctx, "samples");
    p_store->p_mkdir(p_store->p_ctx, TEL_DIR); /* also used without APP_SPLIT_TELEMETRY when a capture has no image */
    sip_reset(&g_images);
    p_store->p_list(p_store->p_ctx, "samples", count_samples_cb, NULL);
    unsigned n_img = 0;
    for (uint32_t idx = 0; idx < g_images.count; idx++)
    {
        n_img += g_images.prefixes[idx].count;
    }
    printf("sd: mounted, %u sample images in %u groups:", n_img, g_images.count);
    for (uint32_t idx = 0; idx < g_images.count; idx++)
    {
        printf(" %s %u", g_images.prefixes[idx].prefix, g_images.prefixes[idx].count);
    }
    printf("\n");
    p_store->p_list(p_store->p_ctx, TEST_PAYLOADS_DIR, count_payloads_cb, NULL);
    printf("sd: %u test payload file(s) in %s/\n", g_n_payloads, TEST_PAYLOADS_DIR);
    restore_capture_id(p_store);

    if (!radios_init(NODE_ID))
    {
        printf("radios: NEITHER radio started. Captures will wait on the SD card.\n");
    }
    g_radios[0] = radio_wifi();
    g_radios[1] = radio_lora();

    alfp_cfg_t cfg = {
        .node_id = NODE_ID,
        .bridge_id = BRIDGE_ID,
        .p_storage = p_store,
        .p_platform = &k_plat,
        .p_inbox_dir = "rcv", /* rcv/<xx>/, see alfp.h; an old flat inbox/ is left alone, never read */
        .p_tmp_dir = "rx",
        .p_on_tx_done = on_tx_done,
        .p_on_rx_done = on_rx_done,
    };
    if (NODE_ID != BRIDGE_ID)
    {
        cfg.p_on_tx_start = on_tx_start;
        cfg.p_on_tx_nack = on_tx_nack;
    }
    if (APP_TRANSFER_EVENTS && (NODE_ID == BRIDGE_ID))
    {
        cfg.p_on_rx_start = ev_rx_start;
        cfg.p_on_rx_manifest = ev_rx_manifest;
        cfg.p_on_rx_frag = ev_rx_frag;
        cfg.p_on_rx_nack = ev_rx_nack;
        cfg.p_on_rx_fail = ev_rx_fail;
    }
    alfp_init(&g_alfp, &cfg);
    uplink_queue_init(&g_uplink, p_store);
    alfp_outbox_init(&g_outbox, &g_alfp, g_radios, 2, BRIDGE_ID, BRIDGE_FRESH_MS, "outbox");
    alfp_outbox_set_size_threshold(&g_outbox, APP_RADIO_SIZE_THRESHOLD);
    g_outbox.p_prepare_meta = stamp_queued_ms;
    printf("node %u ready (%s), outbox has %u queued, radio size threshold %u B\n", (unsigned)NODE_ID,
           NODE_ID == BRIDGE_ID ? "bridge" : "field node", alfp_outbox_count(&g_outbox),
           (unsigned)APP_RADIO_SIZE_THRESHOLD);

    gb_comms_ready = true;
    bool b_dropping = false;
    bool b_topo = true;
    bool b_hold = false;
    uint32_t next_relay_log = p_now(NULL) + 1000u;
    for (;;)
    {
        uint32_t loop_start = p_now(NULL);
        g_radios[0]->p_poll();
        g_radios[1]->p_poll();
        comms_service();
        if (NODE_ID != BRIDGE_ID)
        {
            capture_meta_t req;
            while ((!b_hold) && (xQueueReceive(gp_capture_q, &req, 0) == pdTRUE))
            {
                handle_capture(&req);
            }
            alfp_outbox_pump(&g_outbox); /* radios and the outbox keep running while held */
        }
        else if (0u != uplink_queue_waiting(&g_uplink))
        {
            (void)uplink_queue_service(&g_uplink, UPLINK_LINES_PER_PASS, uplink_out, NULL);
            fflush(stdout);
        }
        else
        {
            /* bridge with nothing queued for the laptop: nothing to do this pass */
        }
        if (APP_RELAY_LOG && ((int32_t)(p_now(NULL) - next_relay_log) >= 0))
        {
            next_relay_log += 1000u;
            relay_log();
        }
        uint32_t loop_ms = p_now(NULL) - loop_start;
        if (loop_ms > g_longest_comms_loop_ms)
        {
            g_longest_comms_loop_ms = loop_ms;
        }
        if (gb_want_stats)
        {
            gb_want_stats = false;
            print_stats();
        }
        if (gb_toggle_drop)
        {
            gb_toggle_drop = false;
            b_dropping = !b_dropping;
            g_radios[0]->p_set_drop_rate(b_dropping ? 0.3f : 0.0f);
            g_radios[1]->p_set_drop_rate(b_dropping ? 0.3f : 0.0f);
            printf("injected loss %s\n", b_dropping ? "30%" : "off");
        }
        if (gb_toggle_hold)
        {
            gb_toggle_hold = false;
            b_hold = !b_hold;
            printf("capture queue %s (%u waiting)\n",
                   b_hold ? "HELD for test T6: captures stay in the 4-slot queue" : "released, sending what waited",
                   (unsigned)uxQueueMessagesWaiting(gp_capture_q));
        }
        if (gb_want_test_payload)
        {
            gb_want_test_payload = false;
            send_next_test_payload();
        }
        if (gb_toggle_topo)
        {
            gb_toggle_topo = false;
            b_topo = !b_topo;
            radios_set_wifi_topology_filter(b_topo);
            printf("wifi topology filter %s\n", b_topo ? "ON (demo layout, forces multi-hop)" : "OFF (hear everyone)");
        }
        /* vTaskDelay, not vTaskDelayUntil: after a slow iteration (SD busy, USB uplink) DelayUntil
         * would not sleep at all while it caught up, starving the sensors task */
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/* ---- sensors (Maryam + Rhaylene) ---------------------------------------------------------- */

static void rtos_wait_ms(uint32_t msec)
{
    vTaskDelay(pdMS_TO_TICKS(msec ? msec : 1));
}

/* dht22.c times the sensor's bits by counting loop iterations, so it must not be interrupted by
 * the comms task for the ~5 ms of the read. Raising this task above comms for that moment does
 * it without touching her code (its 20 ms start pulse still sleeps normally). */
static UBaseType_t s_saved_prio;
static void dht_begin(void)
{
    s_saved_prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 5);
}
static void dht_end(void)
{
    vTaskPrioritySet(NULL, s_saved_prio);
}

static void print_env(void)
{
    const sensing_env_t* p_env = sensing_env();
    if (p_env->b_dht_ok)
    {
        printf("[ENV] Temp: %.1f C | Hum: %.1f%% | Light: %u | Weather: %s | PIR %d Sound %d\n", p_env->temp_c, p_env->hum_pct,
               p_env->light, sensing_weather_name(p_env->weather), sensing_pir(), sensing_sound_trigger());
    }
    else
    {
        printf("[ENV] DHT22: no reading (%" PRIu32 " fails) | Light: %u | Weather: unknown | PIR %d Sound %d\n",
               (uint32_t)p_env->dht_fail, p_env->light, sensing_pir(), sensing_sound_trigger());
    }
}

static void sensors_task(void* p_arg)
{
    (void)p_arg;
    const sensing_hooks_t hooks = { rtos_wait_ms, dht_begin, dht_end };
    sensing_init(&hooks);
    gpio_init(BOARD_BUTTON_GPIO);
    gpio_set_dir(BOARD_BUTTON_GPIO, GPIO_IN);
    gpio_pull_up(BOARD_BUTTON_GPIO);
    while (!gb_comms_ready)
    {
        vTaskDelay(pdMS_TO_TICKS(100)); /* let the radios print their start-up lines first */
    }
    printf("sensors: PIR GP0, sound GP14, mic GP26, DHT22 GP4, light GP27, button GP20\n");

    bool b_was_down = false;
    uint32_t last_env_print = 0;
    for (;;)
    {
        uint32_t now = p_now(NULL);
        sensing_env_poll(now);
        if (gb_want_env || (now - last_env_print >= 10000))
        {
            gb_want_env = false;
            last_env_print = now;
            print_env();
        }

        bool b_down = !gpio_get(BOARD_BUTTON_GPIO);
        bool b_pressed = b_down && (!b_was_down);
        b_was_down = b_down;
        bool b_forced = b_pressed || gb_force_capture;
        gb_force_capture = false;
        bool b_dual = sensing_dual_trigger(now);

        if (b_dual || b_forced)
        {
            uint8_t flags
                = (uint8_t)((sensing_pir() ? CM_TRIG_PIR : 0) | (sensing_sound_trigger() ? CM_TRIG_SOUND : 0));
            if (NODE_ID == BRIDGE_ID)
            {
                printf("\n[ALERT] trigger on the bridge: the bridge only receives, no capture is sent\n");
            }
            printf("\n[ALERT] %s (PIR %d, sound %d). Recording %us of audio...\n",
                   b_dual ? "Dual trigger fired" : "Manual capture", 0 != (flags & CM_TRIG_PIR),
                   0 != (flags & CM_TRIG_SOUND), (unsigned)AUDIO_SECONDS);
            capture_meta_t meta;
            audio_result_t audio;
            sensing_capture(now, flags, &meta, &audio);
            printf("[AUDIO] legacy mic.c: %s energy %.1f zcr %.3f mean %u, %" PRIu32 " samples\n", audio.p_label, audio.energy, audio.zcr,
                   audio.mean_adc, (uint32_t)audio.samples);
            sensing_log_sound();
            print_env();
            if ((NODE_ID != BRIDGE_ID) && (!app_request_capture(&meta)))
            {
                printf("[ALERT] capture queue full, dropped\n");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10)); /* their main.c polls every 10 ms; also debounces the button */
    }
}

static void print_help(void)
{
    printf("keys: c capture now | e sensor readings | s stats | d toggle 30%% loss | t toggle WiFi topology "
           "filter | H hold capture queue (T6) | p send next size-ladder test payload (A.3) | h help\n");
}

static void console_task(void* p_arg)
{
    (void)p_arg;
    for (;;)
    {
        int chan = getchar_timeout_us(0);
        if ('c' == chan)
        {
            gb_force_capture = true;
        }
        else if ('e' == chan)
        {
            gb_want_env = true;
        }
        else if ('t' == chan)
        {
            gb_toggle_topo = true;
        }
        else if ('H' == chan)
        {
            gb_toggle_hold = true;
        }
        else if (('h' == chan) || ('?' == chan))
        {
            print_help();
        }
        else if ('s' == chan)
        {
            gb_want_stats = true;
        }
        else if ('d' == chan)
        {
            gb_toggle_drop = true;
        }
        else if ('p' == chan)
        {
            gb_want_test_payload = true;
        }
        else
        {
            /* no key, or a key with no command: ignore */
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

int main(void)
{
    board_spi1_idle(); /* LoRa CS and SD CS high before anything touches SPI1 */
    stdio_init_all();
    sleep_ms(1500); /* give the USB serial a moment so the first lines are not lost */
    printf("\nCSC09 trail mesh, node %u, ALFP v%u\n", (unsigned)NODE_ID, ALFP_VERSION);
    if (SOUND_LEGACY_MODE)
    {
        printf("sound: LEGACY mode (mic.c thresholds, img_NN.jpg round robin), not the on-node classifier\n");
    }
    gp_capture_q = xQueueCreate(4, sizeof(capture_meta_t));
    xTaskCreate(comms_task, "comms", 2048, NULL, 3, NULL);
    xTaskCreate(sensors_task, "sensors", 1024, NULL, 2, NULL);
    print_help();
    xTaskCreate(console_task, "console", 512, NULL, 1, NULL);
    vTaskStartScheduler();
    for (;;)
    {
    }
}
