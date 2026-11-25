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
static QueueHandle_t g_capture_q;
static const mesh_radio_t* g_radios[2]; /* preference: WiFi first, then LoRa */
static uint32_t g_capture_id;           /* last id used; restored from the SD card at boot */
static uint32_t g_boot_first_id;        /* first capture id issued in this boot (ids only grow) */
static char g_payload_names[TEST_PAYLOADS_MAX][32];
static uint8_t g_n_payloads, g_next_payload;
static sip_table_t g_images; /* samples/<prefix>_NN.jpg found at boot */
static volatile bool g_want_stats, g_toggle_drop, g_force_capture, g_want_env, g_toggle_topo;
static volatile bool g_want_test_payload; /* console 'p' (test A.3): send the next size-ladder file */
static volatile bool g_toggle_hold;       /* console 'H' (test T6): comms stops taking captures off g_capture_q */
static volatile bool g_comms_ready;

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

static const alfp_platform_t k_plat = { NULL, p_now, p_rand, p_log };

/* ---- callbacks ---------------------------------------------------------------------------- */

#define TEL_DIR "tel" /* tiny per-capture telemetry files made only by handle_capture(), see below */

static void on_tx_done(alfp_ctx_t* ctx, const alfp_tx_report_t* r)
{
    (void)ctx;
    printf("tx %s: %s via %s, %u frags, %u rounds, %u resent, %.1f s\n", r->path, alfp_tx_result_str(r->result),
           r->radio, r->frag_count, r->rounds, r->data_resent, (r->finished_ms - r->started_ms) / 1000.0);
    if (r->result == ALFP_TX_DONE && strncmp(r->path, TEL_DIR "/", sizeof(TEL_DIR "/") - 1u) == 0)
    {
        /* The telemetry job's "file" was a temp copy of its own meta made just for this transfer
         * (unlike the image, which is a reused sample on the SD card): remove it once sent, same
         * as the outbox already removes the job descriptor itself in alfp_outbox_on_tx_done(). */
        storage_fatfs()->remove(storage_fatfs()->ctx, r->path);
    }
    alfp_outbox_on_tx_done(&g_outbox, r);
}

/* ---- sender's own serial (TRANSFER_VISIBILITY.md 3.5): only the person at this node sees these ---- */

static void on_tx_start(alfp_ctx_t* ctx, const alfp_tx_t* tx)
{
    (void)ctx;
    capture_meta_t m;
    if (capture_meta_unpack(tx->meta, tx->meta_len, &m) != 0)
    {
        printf("tx start: %s %lu B via %s, %u frags x %u B, sid %u\n", tx->path, (unsigned long)tx->file_size,
               tx->radio->name, tx->frag_count, tx->frag_size, tx->sid);
        return;
    }
    uint8_t rec = capture_meta_record_len(tx->meta, tx->meta_len);
    size_t ml = strlen(APP_TELEMETRY_MARKER);
    bool tel = tx->meta_len == rec + ml && memcmp(tx->meta + rec, APP_TELEMETRY_MARKER, ml) == 0;
    char id[CAPTURE_ID_STR_MAX];
    capture_id_format(id, sizeof(id), m.node_id, m.capture_id);
    char waited[32] = "";
    if (m.queued_ms != CM_MISSING_U32)
    {
        snprintf(waited, sizeof(waited), ", waited %.1f s", m.queued_ms / 1000.0);
    }
    printf("tx start: capture %s %s %lu B via %s, %u frags x %u B, sid %u%s\n", id, tel ? "telemetry" : "image",
           (unsigned long)tx->file_size, tx->radio->name, tx->frag_count, tx->frag_size, tx->sid, waited);
}

static void on_tx_nack(alfp_ctx_t* ctx, uint16_t sid, uint8_t round, uint16_t missing)
{
    (void)ctx;
    if (missing != 0u)
    {
        printf("tx: bridge missing %u fragment%s (round %u, sid %u), resending\n", missing, missing == 1u ? "" : "s",
               round, sid);
    }
}

/* Once a second: "relay wifi: 26 pkts in last 1 s (from node 5: 26)", per radio, only if any. */
static void relay_log(void)
{
    static uint16_t last[2][6];
    static bool primed;
    for (int i = 0; i < 2; i++)
    {
        char from[96] = "";
        size_t o = 0;
        unsigned total = 0;
        for (uint8_t src = 1; src <= 5; src++)
        {
            uint16_t now = radios_relayed_from(g_radios[i], src);
            unsigned d = (uint16_t)(now - last[i][src]); /* wraps cleanly at 65536 */
            last[i][src] = now;
            if (primed && d != 0u && o < sizeof(from))
            {
                int n = snprintf(from + o, sizeof(from) - o, "%snode %u: %u", total ? ", " : "", src, d);
                o += n > 0 ? (size_t)n : 0u;
                total += d;
            }
        }
        if (total != 0u)
        {
            printf("relay %s: %u pkts in last 1 s (from %s)\n", g_radios[i]->name, total, from);
        }
    }
    primed = true;
}

static void uplink_out(const char* s, void* user)
{
    (void)user;
    fputs(s, stdout);
}

/* Bridge -> laptop: finished files wait here and go out UPLINK_LINES_PER_PASS lines per comms
 * pass (uplink.h). Streaming the whole file from inside on_rx_done (the old way) stopped both
 * radios for seconds and held back the sender's ACK; see firmware_smp/app/main_smp.c. */
static uplink_queue_t g_uplink;
#define UPLINK_LINES_PER_PASS 8u
static uint32_t g_longest_comms_loop_ms;

/* ---- transfer events (bridge -> laptop, TRANSFER_VISIBILITY.md 3.3) --------------------------- */

/* Route of the packet alfp_handle_packet() is working on right now (set by comms_service()). */
static uint8_t g_pkt_hops, g_pkt_prev;

/* One whole line in one write, through the same stdout the uplink uses, so an event line can sit
 * between two "@D" lines but can never end up inside one. */
static void evt_line(const char* fmt, ...)
{
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1u, fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        return;
    }
    size_t len = (size_t)n < sizeof(line) - 2u ? (size_t)n : sizeof(line) - 2u;
    line[len++] = '\n';
    line[len] = 0;
    fputs(line, stdout);
    fflush(stdout);
}

static void ev_rx_start(alfp_ctx_t* ctx, uint8_t src, uint16_t sid, const char* radio)
{
    (void)ctx;
    evt_line("@EVT RXS src=%u sid=%u radio=%s t=%lu", src, sid, radio, (unsigned long)p_now(NULL));
}

static void ev_rx_manifest(alfp_ctx_t* ctx, uint8_t src, uint16_t sid, uint16_t frag_count, uint16_t frag_size,
                           uint32_t file_size, uint32_t file_crc32, const uint8_t* meta, uint8_t meta_len)
{
    (void)ctx;
    char hex[2 * ALFP_MAX_META + 1];
    unsigned n = meta_len < ALFP_MAX_META ? meta_len : ALFP_MAX_META;
    for (unsigned i = 0; i < n; i++)
    {
        snprintf(hex + 2 * i, 3, "%02x", meta[i]);
    }
    hex[2 * n] = 0;
    evt_line("@EVT RXM src=%u sid=%u frags=%u fsz=%u size=%lu crc=%08lx meta=%s t=%lu", src, sid, frag_count,
             frag_size, (unsigned long)file_size, (unsigned long)file_crc32, hex, (unsigned long)p_now(NULL));
}

static void ev_rx_frag(alfp_ctx_t* ctx, uint8_t src, uint16_t sid, uint16_t seq, uint8_t round, bool dup)
{
    (void)ctx;
    if (APP_RXF_EVERY > 1 && round == 0 && !dup && seq % APP_RXF_EVERY != 0)
    {
        return;
    }
    evt_line("@EVT RXF src=%u sid=%u seq=%u r=%u h=%u v=%u d=%u t=%lu", src, sid, seq, round, g_pkt_hops, g_pkt_prev,
             dup ? 1u : 0u, (unsigned long)p_now(NULL));
}

static void ev_rx_nack(alfp_ctx_t* ctx, uint8_t src, uint16_t sid, uint8_t round, uint16_t missing)
{
    (void)ctx;
    evt_line("@EVT NACK src=%u sid=%u r=%u miss=%u t=%lu", src, sid, round, missing, (unsigned long)p_now(NULL));
}

static void ev_rx_fail(alfp_ctx_t* ctx, uint8_t src, uint16_t sid, const char* why)
{
    (void)ctx;
    evt_line("@EVT RXFAIL src=%u sid=%u why=%s t=%lu", src, sid, why, (unsigned long)p_now(NULL));
}

static void on_rx_done(alfp_ctx_t* ctx, const alfp_rx_report_t* r)
{
    if (APP_TRANSFER_EVENTS)
    {
        evt_line("@EVT RXOK src=%u sid=%u first=%lu done=%lu nacks=%u dups=%u frags=%u t=%lu", r->src, r->sid,
                 (unsigned long)r->first_ms, (unsigned long)r->done_ms, r->nacks_sent, r->duplicates, r->frag_count,
                 (unsigned long)p_now(NULL));
    }
    printf("rx from node %u: %s, %lu bytes via %s, %u dup\n", r->src, r->path, (unsigned long)r->file_size, r->radio,
           r->duplicates);
    if (!uplink_queue_add(&g_uplink, r))
    {
        printf("uplink: queue full (%u files), sending %s inline\n", (unsigned)UPLINK_QUEUE_DEPTH, r->path);
        uplink_send_file(ctx->cfg.storage, r, uplink_out, NULL);
        fflush(stdout);
    }
}

/* ---- capture requests (any task) --------------------------------------------------------- */

bool app_request_capture(const capture_meta_t* sensors)
{
    return g_capture_q && xQueueSend(g_capture_q, sensors, 0) == pdTRUE;
}

static void count_samples_cb(const char* name, void* user)
{
    (void)user;
    sip_add_file(&g_images, name);
}

static void count_payloads_cb(const char* name, void* user)
{
    (void)user;
    if (name[0] == '.')
    {
        return; /* F1: macOS "._*" AppleDouble files and other hidden files are not payloads */
    }
    size_t nl = strlen(name);
    if (g_n_payloads < TEST_PAYLOADS_MAX && nl < sizeof(g_payload_names[0]))
    {
        memcpy(g_payload_names[g_n_payloads], name, nl + 1u); /* + the NUL strlen() does not count */
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

static uint32_t read_u32_file(const alfp_storage_t* st, const char* path)
{
    int h = st->open_read(st->ctx, path);
    if (h < 0)
    {
        return 0;
    }
    char txt[16] = { 0 };
    int n = st->read_at(st->ctx, h, 0, (uint8_t*)txt, sizeof(txt) - 1);
    st->close(st->ctx, h);
    unsigned long v = 0;
    return n > 0 && sscanf(txt, "%lu", &v) == 1 ? (uint32_t)v : 0;
}

/* write to a temp file, then rename over the real one: a power cut leaves one of the two intact */
static void store_next_id(const alfp_storage_t* st, uint32_t next)
{
    char txt[16];
    int n = snprintf(txt, sizeof(txt), "%lu\n", (unsigned long)next);
    int h = st->open_write(st->ctx, ID_TMP);
    bool ok = h >= 0 && st->write_at(st->ctx, h, 0, (const uint8_t*)txt, (uint16_t)n) == n;
    if (h >= 0)
    {
        st->close(st->ctx, h);
    }
    if (!ok || st->rename(st->ctx, ID_TMP, ID_FILE) != 0)
    {
        printf("capture id: could not save %s (ids may repeat after a reboot)\n", ID_FILE);
    }
}

static uint32_t s_job_seq[ALFP_OUTBOX_MAX + 8];
static uint32_t s_n_jobs, s_highest_seen;

static void scan_outbox_cb(const char* name, void* user)
{
    (void)user;
    unsigned long seq;
    if (name[0] == 'q' && sscanf(name + 1, "%8lu", &seq) == 1 && s_n_jobs < sizeof(s_job_seq) / sizeof(s_job_seq[0]))
    {
        s_job_seq[s_n_jobs++] = (uint32_t)seq;
    }
}

static void scan_clips_cb(const char* name, void* user)
{
    (void)user;
    uint8_t node;
    uint32_t id;
    if (capture_id_parse(name, &node, &id) && node == NODE_ID && id > s_highest_seen)
    {
        s_highest_seen = id;
    }
}

/* Highest capture id still on the card: queued jobs (their meta) and stored clips (their names). */
static uint32_t highest_id_on_card(const alfp_storage_t* st)
{
    s_n_jobs = 0;
    s_highest_seen = 0;
    st->list(st->ctx, "outbox", scan_outbox_cb, NULL);
    for (uint32_t i = 0; i < s_n_jobs; i++)
    {
        char p[32];
        snprintf(p, sizeof(p), "outbox/q%08lu.job", (unsigned long)s_job_seq[i]);
        uint8_t buf[2 + 1 + ALFP_PATH_MAX + 1 + ALFP_MAX_META];
        int h = st->open_read(st->ctx, p);
        if (h < 0)
        {
            continue;
        }
        int n = st->read_at(st->ctx, h, 0, buf, sizeof(buf));
        st->close(st->ctx, h);
        /* job file (alfp_outbox.c): 'Q' '1' | path_len | path | meta_len | meta */
        if (n < 4 || buf[0] != 'Q' || buf[1] != '1' || 4 + buf[2] > n)
        {
            continue;
        }
        const uint8_t* meta = buf + 4 + buf[2];
        uint8_t ml = buf[3 + buf[2]];
        capture_meta_t m;
        if (meta + ml <= buf + n && capture_meta_unpack(meta, ml, &m) == 0 && m.capture_id > s_highest_seen)
        {
            s_highest_seen = m.capture_id;
        }
    }
    st->list(st->ctx, "clips", scan_clips_cb, NULL);
    return s_highest_seen;
}

/* Outbox hook (TRANSFER_VISIBILITY.md 3.4): just before a job is sent, write how long it waited
 * since its trigger into the v2 record's queued_ms. Only for captures made in THIS boot: uptime_ms
 * of a job queued before a reboot is from the old boot, so its wait cannot be measured and stays
 * missing (0xFFFFFFFF) instead of a made-up number. */
static void stamp_queued_ms(uint8_t* meta, uint8_t len, void* user)
{
    (void)user;
    capture_meta_t m;
    if (len < CAPTURE_META_LEN_V2 || meta[0] != 2u || capture_meta_unpack(meta, len, &m) != 0)
    {
        return;
    }
    uint32_t now = p_now(NULL);
    if (m.node_id == NODE_ID && m.capture_id >= g_boot_first_id && g_boot_first_id != 0u && now >= m.uptime_ms)
    {
        cm_put32(meta + CAPTURE_META_QUEUED_OFF, now - m.uptime_ms);
    }
}

static void restore_capture_id(const alfp_storage_t* st)
{
    st->mkdir(st->ctx, "meta");
    uint32_t next = read_u32_file(st, ID_FILE);
    uint32_t tmp = read_u32_file(st, ID_TMP);
    next = tmp > next ? tmp : next;
    uint32_t seen = highest_id_on_card(st);
    g_capture_id = next > seen + 1u ? next - 1u : seen;
    g_boot_first_id = g_capture_id + 1u;
    char id[CAPTURE_ID_STR_MAX];
    capture_id_format(id, sizeof(id), NODE_ID, g_capture_id + 1u);
    printf("capture id: next is %s (saved next %lu, highest on card %lu)\n", id, (unsigned long)next,
           (unsigned long)seen);
}

static void handle_capture(const capture_meta_t* in)
{
    const alfp_storage_t* st = storage_fatfs();
    capture_meta_t m = *in;
    m.node_id = NODE_ID;
    m.capture_id = ++g_capture_id;
    store_next_id(st, g_capture_id + 1u);
    if (m.uptime_ms == 0)
    {
        m.uptime_ms = p_now(NULL);
    }
    char id[CAPTURE_ID_STR_MAX];
    capture_id_format(id, sizeof(id), NODE_ID, m.capture_id);

    /* the image follows the sound: label -> its group -> generic img_NN (legacy: always img_NN, round robin) */
    /* Follow-up F6: sip_pick() now falls back to any group, so it fails only on a card with no
     * <prefix>_NN.jpg at all; even then the telemetry below is still sent. */
    char name[ALFP_MAX_META - CAPTURE_META_LEN + 1];
    int nn = sip_pick(&g_images, SOUND_LEGACY_MODE ? SC_UNKNOWN : m.sound_class, name, sizeof(name));
    char path[ALFP_PATH_MAX] = "";
    if (nn >= 0)
    {
        m.image_index = (uint8_t)nn;
        snprintf(path, sizeof(path), "samples/%s", name);
    }
    const char* shown = nn >= 0 ? path : "no image (samples/ has no <prefix>_NN.jpg), telemetry only";
    if (m.sound_class == CM_UNKNOWN_U8)
    {
        printf("capture %s: heard unknown (recording failed) -> %s\n", id, shown);
    }
    else if (m.sound_conf == CM_UNKNOWN_U8) /* legacy thresholds give no confidence */
    {
        printf("capture %s: heard %s -> %s\n", id, sound_class_name(m.sound_class), shown);
    }
    else
    {
        printf("capture %s: heard %s (%u%%) -> %s\n", id, sound_class_name(m.sound_class), m.sound_conf, shown);
    }

    /* meta = the 31-byte v2 record + the image's file name (the optional-name tail, <= 64 bytes in all) */
    uint8_t mb[ALFP_MAX_META];
    uint8_t meta_len = capture_meta_pack(&m, mb);

    if (APP_SPLIT_TELEMETRY || nn < 0)
    {
        /* Telemetry: a tiny transfer tagged with APP_TELEMETRY_MARKER instead of an image name,
         * small enough that the size rule always sends it over LoRa. The "file" is just a temp
         * copy of the same meta bytes -- removed from the SD card by on_tx_done() once it is
         * confirmed sent. */
        char tel_path[ALFP_PATH_MAX];
        char stem[CAPTURE_ID_STR_MAX];
        capture_id_file_stem(stem, sizeof(stem), NODE_ID, m.capture_id);
        snprintf(tel_path, sizeof(tel_path), TEL_DIR "/%s.bin", stem);
        int th = st->open_write(st->ctx, tel_path);
        bool tel_ok = th >= 0 && st->write_at(st->ctx, th, 0, mb, meta_len) == meta_len;
        if (th >= 0)
        {
            st->close(st->ctx, th);
        }
        uint8_t tel_meta[ALFP_MAX_META];
        memcpy(tel_meta, mb, meta_len);
        size_t marker_len = strlen(APP_TELEMETRY_MARKER);
        memcpy(tel_meta + meta_len, APP_TELEMETRY_MARKER, marker_len);
        uint8_t tel_meta_len = (uint8_t)(meta_len + marker_len);
        if (tel_ok && alfp_outbox_add(&g_outbox, tel_path, tel_meta, tel_meta_len))
        {
            printf("capture %s telemetry queued (%s)\n", id, tel_path);
        }
        else
        {
            printf("capture %s: telemetry SD write failed%s\n", id, nn >= 0 ? ", image still sent" : "");
            st->remove(st->ctx, tel_path);
        }
    }
    if (nn < 0)
    {
        return;
    }

    /* Image: meta + the image's file name (the optional-name tail), exactly as before this round. */
    size_t nl = strlen(name);
    memcpy(mb + meta_len, name, nl);
    uint8_t ml = (uint8_t)(meta_len + nl);
    if (alfp_outbox_add(&g_outbox, path, mb, ml))
    {
        printf("capture %s queued (%s), outbox %u\n", id, path, alfp_outbox_count(&g_outbox));
    }
    else
    {
        printf("capture %s: SD write failed\n", id);
    }
}

/* ---- tasks -------------------------------------------------------------------------------- */

/* alfp_service() with the route of each packet noted first (TRANSFER_VISIBILITY.md 3.3): same bound
 * (16 packets per radio per pass) and the same copy-out, since ALFP reuses its packet buffer for
 * replies. The receive callbacks read g_pkt_hops / g_pkt_prev for the packet being handled. */
static void comms_service(void)
{
    static uint8_t in[ALFP_MAX_PKT];
    for (int i = 0; i < 2; i++)
    {
        const mesh_radio_t* r = g_radios[i];
        for (int k = 0; k < 16; k++)
        {
            uint8_t src;
            int n = r->recv(&src, in, sizeof(in));
            if (n == MESH_RECV_NONE)
            {
                break;
            }
            if (n < 0)
            {
                g_alfp.stats.pkts_bad_other++;
                continue;
            }
            if (!radios_last_recv_info(r, &g_pkt_hops, &g_pkt_prev))
            {
                g_pkt_hops = 0;
                g_pkt_prev = 0;
            }
            alfp_handle_packet(&g_alfp, r, src, in, (uint16_t)n);
        }
    }
    alfp_step(&g_alfp);
}

static void print_stats(void)
{
    const alfp_stats_t* s = &g_alfp.stats;
    printf("node %u | alfp in %lu badcrc %lu | tx ok %lu fail %lu | rx ok %lu crcfail %lu expired %lu | outbox %u\n",
           (unsigned)NODE_ID, (unsigned long)s->pkts_in, (unsigned long)s->pkts_bad_crc, (unsigned long)s->tx_ok,
           (unsigned long)s->tx_failed, (unsigned long)s->rx_ok, (unsigned long)s->rx_crc_fail,
           (unsigned long)s->rx_expired, alfp_outbox_count(&g_outbox));
    for (int i = 0; i < 2; i++)
    {
        mesh_stats_t m;
        g_radios[i]->get_stats(&m);
        printf("  %s: tx %lu rx %lu delivered %lu relayed %lu dup %lu rxerr %lu\n", g_radios[i]->name,
               (unsigned long)m.tx, (unsigned long)m.rx, (unsigned long)m.delivered, (unsigned long)m.relayed,
               (unsigned long)m.dup_dropped, (unsigned long)m.rx_error);
    }
    const sensing_env_t* e = sensing_env();
    printf("  dht22 good %lu fail %lu | lora %s | wifi %s\n", (unsigned long)e->dht_good, (unsigned long)e->dht_fail,
           radios_lora_ok() ? "up" : "DOWN", radios_wifi_ok() ? "up" : "DOWN");
    uint32_t t;
    for (int i = 0; i < 2; i++)
    {
        if (g_radios[i]->last_heard(BRIDGE_ID, &t))
        {
            printf("  bridge last heard on %s %lu ms ago\n", g_radios[i]->name, (unsigned long)(p_now(NULL) - t));
        }
        else
        {
            printf("  bridge never heard on %s\n", g_radios[i]->name);
        }
    }
    printf("  comms stack free: %lu words, heap free %lu bytes\n", (unsigned long)uxTaskGetStackHighWaterMark(NULL),
           (unsigned long)xPortGetFreeHeapSize());
    printf("  longest comms loop %lu ms | outbox dropped (full) %lu", (unsigned long)g_longest_comms_loop_ms,
           (unsigned long)g_outbox.dropped_full);
    if (NODE_ID == BRIDGE_ID)
    {
        printf(" | uplink sent %lu bad %lu waiting %u (max %u) inline %lu", (unsigned long)g_uplink.sent_ok,
               (unsigned long)g_uplink.sent_bad, uplink_queue_waiting(&g_uplink), (unsigned)g_uplink.max_waiting,
               (unsigned long)g_uplink.queue_full);
    }
    printf("\n");
}

static void comms_task(void* arg)
{
    (void)arg;
    /* The SD card goes first: it must be switched to SPI mode before any LoRa traffic appears on
     * the shared SPI1 bus (board.h). */
    while (!storage_fatfs_mount())
    {
        printf("sd: mount failed (card in the Maker slot? FAT32?), retrying\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    const alfp_storage_t* st = storage_fatfs();
    st->mkdir(st->ctx, "samples");
    st->mkdir(st->ctx, TEL_DIR); /* also used without APP_SPLIT_TELEMETRY when a capture has no image */
    sip_reset(&g_images);
    st->list(st->ctx, "samples", count_samples_cb, NULL);
    unsigned n_img = 0;
    for (uint32_t i = 0; i < g_images.n; i++)
    {
        n_img += g_images.p[i].count;
    }
    printf("sd: mounted, %u sample images in %u groups:", n_img, g_images.n);
    for (uint32_t i = 0; i < g_images.n; i++)
    {
        printf(" %s %u", g_images.p[i].prefix, g_images.p[i].count);
    }
    printf("\n");
    st->list(st->ctx, TEST_PAYLOADS_DIR, count_payloads_cb, NULL);
    printf("sd: %u test payload file(s) in %s/\n", g_n_payloads, TEST_PAYLOADS_DIR);
    restore_capture_id(st);

    if (!radios_init(NODE_ID))
    {
        printf("radios: NEITHER radio started. Captures will wait on the SD card.\n");
    }
    g_radios[0] = radio_wifi();
    g_radios[1] = radio_lora();

    alfp_cfg_t cfg = {
        .node_id = NODE_ID,
        .bridge_id = BRIDGE_ID,
        .storage = st,
        .platform = &k_plat,
        .inbox_dir = "rcv", /* rcv/<xx>/, see alfp.h; an old flat inbox/ is left alone, never read */
        .tmp_dir = "rx",
        .on_tx_done = on_tx_done,
        .on_rx_done = on_rx_done,
    };
    if (NODE_ID != BRIDGE_ID)
    {
        cfg.on_tx_start = on_tx_start;
        cfg.on_tx_nack = on_tx_nack;
    }
    if (APP_TRANSFER_EVENTS && NODE_ID == BRIDGE_ID)
    {
        cfg.on_rx_start = ev_rx_start;
        cfg.on_rx_manifest = ev_rx_manifest;
        cfg.on_rx_frag = ev_rx_frag;
        cfg.on_rx_nack = ev_rx_nack;
        cfg.on_rx_fail = ev_rx_fail;
    }
    alfp_init(&g_alfp, &cfg);
    uplink_queue_init(&g_uplink, st);
    alfp_outbox_init(&g_outbox, &g_alfp, g_radios, 2, BRIDGE_ID, BRIDGE_FRESH_MS, "outbox");
    alfp_outbox_set_size_threshold(&g_outbox, APP_RADIO_SIZE_THRESHOLD);
    g_outbox.prepare_meta = stamp_queued_ms;
    printf("node %u ready (%s), outbox has %u queued, radio size threshold %u B\n", (unsigned)NODE_ID,
           NODE_ID == BRIDGE_ID ? "bridge" : "field node", alfp_outbox_count(&g_outbox),
           (unsigned)APP_RADIO_SIZE_THRESHOLD);

    g_comms_ready = true;
    bool dropping = false, topo = true, hold = false;
    uint32_t next_relay_log = p_now(NULL) + 1000u;
    for (;;)
    {
        uint32_t loop_start = p_now(NULL);
        g_radios[0]->poll();
        g_radios[1]->poll();
        comms_service();
        if (NODE_ID != BRIDGE_ID)
        {
            capture_meta_t req;
            while (!hold && xQueueReceive(g_capture_q, &req, 0) == pdTRUE)
            {
                handle_capture(&req);
            }
            alfp_outbox_pump(&g_outbox); /* radios and the outbox keep running while held */
        }
        else if (uplink_queue_waiting(&g_uplink) != 0u)
        {
            (void)uplink_queue_service(&g_uplink, UPLINK_LINES_PER_PASS, uplink_out, NULL);
            fflush(stdout);
        }
        else
        {
            /* bridge with nothing queued for the laptop: nothing to do this pass */
        }
        if (APP_RELAY_LOG && (int32_t)(p_now(NULL) - next_relay_log) >= 0)
        {
            next_relay_log += 1000u;
            relay_log();
        }
        uint32_t loop_ms = p_now(NULL) - loop_start;
        if (loop_ms > g_longest_comms_loop_ms)
        {
            g_longest_comms_loop_ms = loop_ms;
        }
        if (g_want_stats)
        {
            g_want_stats = false;
            print_stats();
        }
        if (g_toggle_drop)
        {
            g_toggle_drop = false;
            dropping = !dropping;
            g_radios[0]->set_drop_rate(dropping ? 0.3f : 0.0f);
            g_radios[1]->set_drop_rate(dropping ? 0.3f : 0.0f);
            printf("injected loss %s\n", dropping ? "30%" : "off");
        }
        if (g_toggle_hold)
        {
            g_toggle_hold = false;
            hold = !hold;
            printf("capture queue %s (%u waiting)\n",
                   hold ? "HELD for test T6: captures stay in the 4-slot queue" : "released, sending what waited",
                   (unsigned)uxQueueMessagesWaiting(g_capture_q));
        }
        if (g_want_test_payload)
        {
            g_want_test_payload = false;
            send_next_test_payload();
        }
        if (g_toggle_topo)
        {
            g_toggle_topo = false;
            topo = !topo;
            radios_set_wifi_topology_filter(topo);
            printf("wifi topology filter %s\n", topo ? "ON (demo layout, forces multi-hop)" : "OFF (hear everyone)");
        }
        /* vTaskDelay, not vTaskDelayUntil: after a slow iteration (SD busy, USB uplink) DelayUntil
         * would not sleep at all while it caught up, starving the sensors task */
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/* ---- sensors (Maryam + Rhaylene) ---------------------------------------------------------- */

static void rtos_wait_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}

/* DHT22.c times the sensor's bits by counting loop iterations, so it must not be interrupted by
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
    const sensing_env_t* e = sensing_env();
    if (e->dht_ok)
    {
        printf("[ENV] Temp: %.1f C | Hum: %.1f%% | Light: %u | Weather: %s | PIR %d Sound %d\n", e->temp_c, e->hum_pct,
               e->light, sensing_weather_name(e->weather), sensing_pir(), sensing_sound_trigger());
    }
    else
    {
        printf("[ENV] DHT22: no reading (%lu fails) | Light: %u | Weather: unknown | PIR %d Sound %d\n",
               (unsigned long)e->dht_fail, e->light, sensing_pir(), sensing_sound_trigger());
    }
}

static void sensors_task(void* arg)
{
    (void)arg;
    const sensing_hooks_t hooks = { rtos_wait_ms, dht_begin, dht_end };
    sensing_init(&hooks);
    gpio_init(BOARD_BUTTON_GPIO);
    gpio_set_dir(BOARD_BUTTON_GPIO, GPIO_IN);
    gpio_pull_up(BOARD_BUTTON_GPIO);
    while (!g_comms_ready)
    {
        vTaskDelay(pdMS_TO_TICKS(100)); /* let the radios print their start-up lines first */
    }
    printf("sensors: PIR GP0, sound GP14, mic GP26, DHT22 GP4, light GP27, button GP20\n");

    bool was_down = false;
    uint32_t last_env_print = 0;
    for (;;)
    {
        uint32_t now = p_now(NULL);
        sensing_env_poll(now);
        if (g_want_env || now - last_env_print >= 10000)
        {
            g_want_env = false;
            last_env_print = now;
            print_env();
        }

        bool down = !gpio_get(BOARD_BUTTON_GPIO);
        bool pressed = down && !was_down;
        was_down = down;
        bool forced = pressed || g_force_capture;
        g_force_capture = false;
        bool dual = sensing_dual_trigger(now);

        if (dual || forced)
        {
            uint8_t flags
                = (uint8_t)((sensing_pir() ? CM_TRIG_PIR : 0) | (sensing_sound_trigger() ? CM_TRIG_SOUND : 0));
            if (NODE_ID == BRIDGE_ID)
            {
                printf("\n[ALERT] trigger on the bridge: the bridge only receives, no capture is sent\n");
            }
            printf("\n[ALERT] %s (PIR %d, sound %d). Recording %us of audio...\n",
                   dual ? "Dual trigger fired" : "Manual capture", (flags & CM_TRIG_PIR) != 0,
                   (flags & CM_TRIG_SOUND) != 0, (unsigned)AUDIO_SECONDS);
            capture_meta_t m;
            audio_result_t a;
            sensing_capture(now, flags, &m, &a);
            printf("[AUDIO] legacy mic.c: %s energy %.1f zcr %.3f mean %u, %lu samples\n", a.label, a.energy, a.zcr,
                   a.mean_adc, (unsigned long)a.samples);
            sensing_log_sound();
            print_env();
            if (NODE_ID != BRIDGE_ID && !app_request_capture(&m))
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

static void console_task(void* arg)
{
    (void)arg;
    for (;;)
    {
        int ch = getchar_timeout_us(0);
        if (ch == 'c')
        {
            g_force_capture = true;
        }
        else if (ch == 'e')
        {
            g_want_env = true;
        }
        else if (ch == 't')
        {
            g_toggle_topo = true;
        }
        else if (ch == 'H')
        {
            g_toggle_hold = true;
        }
        else if (ch == 'h' || ch == '?')
        {
            print_help();
        }
        else if (ch == 's')
        {
            g_want_stats = true;
        }
        else if (ch == 'd')
        {
            g_toggle_drop = true;
        }
        else if (ch == 'p')
        {
            g_want_test_payload = true;
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
    g_capture_q = xQueueCreate(4, sizeof(capture_meta_t));
    xTaskCreate(comms_task, "comms", 2048, NULL, 3, NULL);
    xTaskCreate(sensors_task, "sensors", 1024, NULL, 2, NULL);
    print_help();
    xTaskCreate(console_task, "console", 512, NULL, 1, NULL);
    vTaskStartScheduler();
    for (;;)
    {
    }
}
