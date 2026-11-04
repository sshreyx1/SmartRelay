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
/* Explicit marker in the telemetry job's filename-tail (the bytes after the 27-byte meta record,
 * see common/capture_meta.h), so the backend/tools/bridge_receiver.py side can tell "no image in
 * this transfer" apart from the pre-existing, still-valid case of a transfer with a real image
 * and simply no name tail at all (some callers never appended one before this round). Never a
 * real image name: no image this firmware ever picks ends without an extension. Kept in sync by
 * hand with tools/bridge_receiver.py's TELEMETRY_MARKER and backend/app.py's own copy. */
#define APP_TELEMETRY_MARKER "TELEMETRY"

#define TEST_PAYLOADS_DIR "test_payloads" /* Task A.3: tools/make_test_payloads.py's size ladder */
#define TEST_PAYLOADS_MAX 16

static alfp_ctx_t g_alfp;
static alfp_outbox_t g_outbox;
static QueueHandle_t g_capture_q;
static const mesh_radio_t* g_radios[2]; /* preference: WiFi first, then LoRa */
static uint32_t g_capture_id; /* last id used; restored from the SD card at boot */
static char g_payload_names[TEST_PAYLOADS_MAX][32];
static uint8_t g_n_payloads, g_next_payload;
static sip_table_t g_images;  /* samples/<prefix>_NN.jpg found at boot */
static volatile bool g_want_stats, g_toggle_drop, g_force_capture, g_want_env, g_toggle_topo;
static volatile bool g_want_test_payload; /* console 'p' (test A.3): send the next size-ladder file */
static volatile bool g_toggle_hold; /* console 'H' (test T6): comms stops taking captures off g_capture_q */
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

static const alfp_platform_t k_plat = {NULL, p_now, p_rand, p_log};

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

static void uplink_out(const char* s, void* user)
{
    (void)user;
    fputs(s, stdout);
}

static void on_rx_done(alfp_ctx_t* ctx, const alfp_rx_report_t* r)
{
    printf("rx from node %u: %s, %lu bytes via %s, %u dup\n", r->src, r->path, (unsigned long)r->file_size, r->radio,
           r->duplicates);
    uplink_send_file(ctx->cfg.storage, r, uplink_out, NULL);
    fflush(stdout);
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
    char txt[16] = {0};
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

static void restore_capture_id(const alfp_storage_t* st)
{
    st->mkdir(st->ctx, "meta");
    uint32_t next = read_u32_file(st, ID_FILE);
    uint32_t tmp = read_u32_file(st, ID_TMP);
    next = tmp > next ? tmp : next;
    uint32_t seen = highest_id_on_card(st);
    g_capture_id = next > seen + 1u ? next - 1u : seen;
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
    char name[ALFP_MAX_META - CAPTURE_META_LEN + 1];
    int nn = sip_pick(&g_images, SOUND_LEGACY_MODE ? SC_UNKNOWN : m.sound_class, name, sizeof(name));
    if (nn < 0)
    {
        printf("capture %s: no samples/*_NN.jpg on the SD card\n", id);
        return;
    }
    m.image_index = (uint8_t)nn;
    char path[ALFP_PATH_MAX];
    snprintf(path, sizeof(path), "samples/%s", name);
    if (m.sound_class == CM_UNKNOWN_U8)
    {
        printf("capture %s: heard unknown (recording failed) -> %s\n", id, path);
    }
    else if (m.sound_conf == CM_UNKNOWN_U8) /* legacy thresholds give no confidence */
    {
        printf("capture %s: heard %s -> %s\n", id, sound_class_name(m.sound_class), path);
    }
    else
    {
        printf("capture %s: heard %s (%u%%) -> %s\n", id, sound_class_name(m.sound_class), m.sound_conf, path);
    }

    /* meta = the 27-byte record + the image's file name (the optional-name tail, <= 64 bytes in all) */
    uint8_t mb[ALFP_MAX_META];
    uint8_t meta_len = capture_meta_pack(&m, mb);

    if (APP_SPLIT_TELEMETRY)
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
            printf("capture %s: telemetry SD write failed, image still sent\n", id);
            st->remove(st->ctx, tel_path);
        }
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

static void print_stats(void)
{
    const alfp_stats_t* s = &g_alfp.stats;
    printf("node %u | alfp in %lu badcrc %lu | tx ok %lu fail %lu | rx ok %lu crcfail %lu expired %lu | outbox %u\n",
           NODE_ID, (unsigned long)s->pkts_in, (unsigned long)s->pkts_bad_crc, (unsigned long)s->tx_ok,
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
    if (APP_SPLIT_TELEMETRY)
    {
        st->mkdir(st->ctx, TEL_DIR);
    }
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
        .inbox_dir = "inbox",
        .tmp_dir = "rx",
        .on_tx_done = on_tx_done,
        .on_rx_done = on_rx_done,
    };
    alfp_init(&g_alfp, &cfg);
    alfp_outbox_init(&g_outbox, &g_alfp, g_radios, 2, BRIDGE_ID, BRIDGE_FRESH_MS, "outbox");
    alfp_outbox_set_size_threshold(&g_outbox, APP_RADIO_SIZE_THRESHOLD);
    printf("node %u ready (%s), outbox has %u queued, radio size threshold %u B\n", NODE_ID,
           NODE_ID == BRIDGE_ID ? "bridge" : "field node", alfp_outbox_count(&g_outbox),
           (unsigned)APP_RADIO_SIZE_THRESHOLD);

    g_comms_ready = true;
    bool dropping = false, topo = true, hold = false;
    for (;;)
    {
        g_radios[0]->poll();
        g_radios[1]->poll();
        alfp_service(&g_alfp, g_radios, 2);
        if (NODE_ID != BRIDGE_ID)
        {
            capture_meta_t req;
            while (!hold && xQueueReceive(g_capture_q, &req, 0) == pdTRUE)
            {
                handle_capture(&req);
            }
            alfp_outbox_pump(&g_outbox); /* radios and the outbox keep running while held */
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
    const sensing_hooks_t hooks = {rtos_wait_ms, dht_begin, dht_end};
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
            uint8_t flags = (uint8_t)((sensing_pir() ? CM_TRIG_PIR : 0) | (sensing_sound_trigger() ? CM_TRIG_SOUND : 0));
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
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

int main(void)
{
    board_spi1_idle(); /* LoRa CS and SD CS high before anything touches SPI1 */
    stdio_init_all();
    sleep_ms(1500); /* give the USB serial a moment so the first lines are not lost */
    printf("\nCSC09 trail mesh, node %u, ALFP v%u\n", NODE_ID, ALFP_VERSION);
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
