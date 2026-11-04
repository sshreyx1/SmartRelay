/*
 * main_smp.c - node firmware for the CSC09 trail mesh on uT-Kernel 3 SMP (all five nodes; node 1
 * is also the bridge). Same behaviour, console keys, microSD layout and serial output as the
 * FreeRTOS build's firmware/app/main.c, which stays the default firmware and is untouched; this is
 * the additive dual-core build (firmware_smp/buildglue/README.md).
 *
 * One image per node: uf2/csc09_smp_node_1.uf2 (A, bridge) ... csc09_smp_node_5.uf2 (E).
 * Pins: firmware/app/board.h (same wiring as the FreeRTOS build).
 *
 * Tasks (smaller number = higher priority; priority only matters between tasks on one core)
 *   core 0  comms      (RTOS_PRIO_COMMS)  owns BOTH radios and the SD card, exactly like main.c's
 *                      comms task: radio poll, ALFP receive, timers and pacing, outbox pump,
 *                      capture requests, bridge uplink. Single owner = no locks in the mesh code,
 *                      and FatFs (not thread-safe) is only ever touched here.
 *   core 0  usb_owner  (RTOS_PRIO_USB_OWNER) the console: reads the keys below and is the only
 *                      task that writes to USB (brief 3.6). Everything else printf()s into the
 *                      log ring (log.h), one whole line at a time.
 *   core 1  sensors    (RTOS_PRIO_SENSORS) Maryam's and Rhaylene's code (sensing/sensing.c), alone
 *                      on the second core: DHT22 + light + weather every 2 s; PIR AND sound
 *                      together record 5 s of audio and request a capture. The Maker button (GP20)
 *                      or 'c' forces one. Audio capture and the classifier no longer share a CPU
 *                      with the radios.
 *
 * Built SMP=0 (build_smp.sh), every task runs on the one core, unchanged otherwise.
 *
 * microSD layout: identical to main.c (samples/, meta/next_id, outbox/, tel/, rx/, inbox/,
 * test_payloads/).
 */
#include <stdio.h>
#include <string.h>

#include "alfp.h"
#include "alfp_outbox.h"
#include "board.h"
#include "capture_api.h"
#include "capture_id.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "log.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "pico_runtime_mtk3.h"
#include "radios.h"
#include "rtos_port.h"
#include "sensing.h"
#include "sound_classes.h"
#include "sound_image_pick.h"
#include "storage_fatfs.h"
#include "uplink.h"

/* Kernel USB console (third_party/mtk3smp_rp2040/lib/libtm/sysdepend/pico_rp2040/usb/
 * usb_console.c). Declared here instead of including its kernel header next to the Pico SDK ones
 * (the two define clashing names); only the usb_owner task calls these. */
extern uint32_t tm_usb_state(void);   /* 3 = enumerated and the host opened the port (DTR) */
extern uint32_t tm_usb_pending(void); /* bytes still in its 4 KB transmit ring */
extern void tm_usb_console_put(const uint8_t* buf, int32_t size);
extern volatile uint32_t tm_usb_dropped;
#define USB_TX_RING_BYTES 4096u
#define USB_STATE_OPEN 3u

#ifndef NODE_ID
#define NODE_ID 2
#endif
#define BRIDGE_ID 1
#define BRIDGE_FRESH_MS 10000u

/* Task A.2, same as main.c: capture metadata goes as its own tiny ALFP transfer (LoRa by the size
 * rule), the image as another (WiFi). 0 restores the single combined transfer. */
#ifndef APP_SPLIT_TELEMETRY
#define APP_SPLIT_TELEMETRY 1
#endif
/* Same marker as main.c; kept in sync by hand with tools/bridge_receiver.py's TELEMETRY_MARKER
 * and backend/app.py's own copy. */
#define APP_TELEMETRY_MARKER "TELEMETRY"

#define TEST_PAYLOADS_DIR "test_payloads" /* Task A.3: tools/make_test_payloads.py's size ladder */
#define TEST_PAYLOADS_MAX 16

/* ---- console -> comms/sensors signalling (3.3: event flags, not volatile bools) ---- */
#define EVT_WANT_STATS (1u << 0)
#define EVT_TOGGLE_DROP (1u << 1)
#define EVT_TOGGLE_TOPO (1u << 2)
#define EVT_FORCE_CAPTURE (1u << 3)
#define EVT_WANT_ENV (1u << 4)
#define EVT_COMMS_READY (1u << 5) /* comms -> sensors, set once */
#define EVT_TOGGLE_HOLD (1u << 6)
#define EVT_TEST_PAYLOAD (1u << 7)
#define EVT_COMMS_MASK (EVT_WANT_STATS | EVT_TOGGLE_DROP | EVT_TOGGLE_TOPO | EVT_TOGGLE_HOLD | EVT_TEST_PAYLOAD)

#define STACK_COMMS 8192u
#define STACK_USB_OWNER 3072u
#define STACK_SENSORS 8192u

static alfp_ctx_t g_alfp;
static alfp_outbox_t g_outbox;
static rtos_id_t g_capture_q = -1;
#define CAPTURE_Q_DEPTH 4
static uint8_t g_capture_q_buf[RTOS_MSGQ_BYTES(sizeof(capture_meta_t), CAPTURE_Q_DEPTH)];
static volatile uint32_t g_capture_q_waiting; /* for the 'H' message; the kernel queue has no count call */
static const mesh_radio_t* g_radios[2]; /* preference: WiFi first, then LoRa */
static uint32_t g_capture_id;           /* last id used; restored from the SD card at boot */
static char g_payload_names[TEST_PAYLOADS_MAX][32];
static uint8_t g_n_payloads, g_next_payload;
static sip_table_t g_images; /* samples/<prefix>_NN.jpg found at boot */
static rtos_id_t g_flags = -1;

static rtos_id_t g_comms_task_id = -1;
static rtos_id_t g_sensors_task_id = -1;
static rtos_id_t g_owner_task_id = -1;

/* Section 7 stats additions. */
static volatile uint32_t g_capture_queue_full_count;
static uint32_t g_longest_comms_loop_ms;
/* Slowest single call of each comms-loop step, for 's': which step a long loop was spent in. */
enum
{
    STEP_WIFI_POLL,
    STEP_LORA_POLL,
    STEP_ALFP_SERVICE,
    STEP_CAPTURES,
    STEP_OUTBOX,
    STEP_COUNT
};
static const char* const k_step_names[STEP_COUNT] = {"wifi poll", "lora poll", "alfp service", "captures", "outbox"};
static uint32_t g_step_max_ms[STEP_COUNT];

static void step_done(int step, uint32_t* t)
{
    uint32_t now = rtos_now_ms();
    uint32_t d = now - *t;
    if (d > g_step_max_ms[step])
    {
        g_step_max_ms[step] = d;
    }
    *t = now;
}
static uint32_t g_usb_lines_dropped;

/* ---- stdout: every printf/puts/putchar byte (pico_runtime_mtk3.c) ------------------------ */

void smartrelay_stdout_putc(char c)
{
    log_putc(c);
}

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
        /* The telemetry job's "file" was a temp copy of its own meta made just for this transfer:
         * remove it once sent, as main.c does. */
        storage_fatfs()->remove(storage_fatfs()->ctx, r->path);
    }
    alfp_outbox_on_tx_done(&g_outbox, r);
}

static void uplink_out(const char* s, void* user)
{
    (void)user;
    /* uplink_send_file() writes a line in pieces ("@ALFP FILE ...meta=", the hex, "\n"); log_putc
     * collects them per task, so each uplink line still reaches the host whole. */
    while (*s != '\0')
    {
        log_putc(*s++);
    }
}

static void on_rx_done(alfp_ctx_t* ctx, const alfp_rx_report_t* r)
{
    printf("rx from node %u: %s, %lu bytes via %s, %u dup\n", r->src, r->path, (unsigned long)r->file_size, r->radio,
           r->duplicates);
    uplink_send_file(ctx->cfg.storage, r, uplink_out, NULL);
}

/* ---- capture requests (any task) --------------------------------------------------------- */

bool app_request_capture(const capture_meta_t* sensors)
{
    if (g_capture_q < 0)
    {
        return false;
    }
    if (rtos_msgq_send_nowait(g_capture_q, sensors, sizeof(*sensors)))
    {
        rtos_atomic_add_u32(&g_capture_q_waiting, 1);
        return true;
    }
    /* app_request_capture runs on sensors (core 1) and usb_owner (core 0, 'x' test key) */
    rtos_atomic_add_u32(&g_capture_queue_full_count, 1);
    return false;
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
        memcpy(g_payload_names[g_n_payloads], name, nl + 1u);
        g_n_payloads++;
    }
}

/* Task A.3: send one file from the size ladder with no capture metadata (see main.c). */
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
         * small enough that the size rule always sends it over LoRa (see main.c). */
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

    /* Image: meta + the image's file name (the optional-name tail), exactly as main.c. */
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

    /* --- Section 7: per-core / per-task view of the dual-core build --- */
    struct
    {
        const char* name;
        rtos_id_t id;
        unsigned core;
        unsigned prio;
        uint32_t stack;
    } tasks[3] = {
        {"comms", g_comms_task_id, RTOS_CORE_0, RTOS_PRIO_COMMS, STACK_COMMS},
        {"usb_owner", g_owner_task_id, RTOS_CORE_0, RTOS_PRIO_USB_OWNER, STACK_USB_OWNER},
        {"sensors", g_sensors_task_id, RTOS_CORE_1, RTOS_PRIO_SENSORS, STACK_SENSORS},
    };
    for (int i = 0; i < 3; i++)
    {
        printf("  task %s: core %u prio %u stack free %lu of %lu B\n", tasks[i].name, tasks[i].core, tasks[i].prio,
               (unsigned long)rtos_task_stack_free(tasks[i].id), (unsigned long)tasks[i].stack);
    }
    printf("  longest comms loop %lu ms | capture q full %lu | log dropped %lu lines | usb dropped %lu lines %lu B"
           " | heap used %lu B\n",
           (unsigned long)g_longest_comms_loop_ms, (unsigned long)g_capture_queue_full_count,
           (unsigned long)log_dropped_count(), (unsigned long)g_usb_lines_dropped, (unsigned long)tm_usb_dropped,
           (unsigned long)smartrelay_heap_used());
    printf("  slowest comms step (ms):");
    for (int i = 0; i < STEP_COUNT; i++)
    {
        printf(" %s %lu%s", k_step_names[i], (unsigned long)g_step_max_ms[i], i + 1 < STEP_COUNT ? " |" : "\n");
    }
}

static void comms_task(void* arg)
{
    (void)arg;
    printf("comms: core %u\n", rtos_core_id());

    /* The SD card goes first: it must be switched to SPI mode before any LoRa traffic appears on
     * the shared SPI1 bus (board.h). */
    while (!storage_fatfs_mount())
    {
        printf("sd: mount failed (card in the Maker slot? FAT32?), retrying\n");
        rtos_delay_ms(2000);
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
    {
        char line[LOG_LINE_MAX];
        int len = snprintf(line, sizeof(line), "sd: mounted, %u sample images in %u groups:", n_img,
                           (unsigned)g_images.n);
        for (uint32_t i = 0; i < g_images.n && len > 0 && (size_t)len < sizeof(line); i++)
        {
            len += snprintf(line + len, sizeof(line) - (size_t)len, " %s %u", g_images.p[i].prefix,
                            g_images.p[i].count);
        }
        printf("%s\n", line);
    }
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

    rtos_flags_set(g_flags, EVT_COMMS_READY);
    bool dropping = false, topo = true, hold = false;
    for (;;)
    {
        uint32_t loop_start = rtos_now_ms();
        uint32_t t = loop_start;
        g_radios[0]->poll();
        step_done(STEP_WIFI_POLL, &t);
        g_radios[1]->poll();
        step_done(STEP_LORA_POLL, &t);
        alfp_service(&g_alfp, g_radios, 2);
        step_done(STEP_ALFP_SERVICE, &t);
        if (NODE_ID != BRIDGE_ID)
        {
            capture_meta_t req;
            while (!hold && rtos_msgq_recv(g_capture_q, &req, sizeof(req), RTOS_NO_WAIT))
            {
                rtos_atomic_add_u32(&g_capture_q_waiting, (uint32_t)-1);
                handle_capture(&req);
            }
            step_done(STEP_CAPTURES, &t);
            alfp_outbox_pump(&g_outbox); /* radios and the outbox keep running while held */
            step_done(STEP_OUTBOX, &t);
        }

        uint32_t bits = 0;
        if (rtos_flags_wait_any(g_flags, EVT_COMMS_MASK, &bits, RTOS_NO_WAIT))
        {
            if (bits & EVT_WANT_STATS)
            {
                print_stats();
            }
            if (bits & EVT_TOGGLE_DROP)
            {
                dropping = !dropping;
                g_radios[0]->set_drop_rate(dropping ? 0.3f : 0.0f);
                g_radios[1]->set_drop_rate(dropping ? 0.3f : 0.0f);
                printf("injected loss %s\n", dropping ? "30%" : "off");
            }
            if (bits & EVT_TOGGLE_HOLD)
            {
                hold = !hold;
                printf("capture queue %s (%u waiting)\n",
                       hold ? "HELD for test T6: captures stay in the 4-slot queue" : "released, sending what waited",
                       (unsigned)g_capture_q_waiting);
            }
            if (bits & EVT_TEST_PAYLOAD)
            {
                send_next_test_payload();
            }
            if (bits & EVT_TOGGLE_TOPO)
            {
                topo = !topo;
                radios_set_wifi_topology_filter(topo);
                printf("wifi topology filter %s\n", topo ? "ON (demo layout, forces multi-hop)" : "OFF (hear everyone)");
            }
        }

        uint32_t loop_ms = rtos_elapsed_ms(loop_start);
        if (loop_ms > g_longest_comms_loop_ms)
        {
            g_longest_comms_loop_ms = loop_ms;
        }
        /* Same reasoning as main.c: a fixed delay, not a delay-until, so a slow iteration (SD
         * busy, USB uplink) does not try to make up lost time by starving usb_owner on this core. */
        rtos_delay_ms(2);
    }
}

/* ---- sensors (Maryam + Rhaylene) ---------------------------------------------------------- */

static void rtos_wait_ms(uint32_t ms)
{
    rtos_delay_ms(ms);
}

/* DHT22.c times the sensor's bits by counting loop iterations and must not be interrupted for the
 * ~5 ms of the read. Brief 3.8 Gap 14: kept as the priority boost (not interrupt masking), as in
 * main.c, translated to the kernel's priority call. */
static unsigned s_saved_prio;
static void dht_begin(void)
{
    s_saved_prio = rtos_priority_self_get();
    rtos_priority_self_set(1); /* MIN_TSKPRI: briefly the highest-priority task on its core */
}
static void dht_end(void)
{
    rtos_priority_self_set(s_saved_prio);
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
    printf("sensors: core %u\n", rtos_core_id());
    const sensing_hooks_t hooks = {rtos_wait_ms, dht_begin, dht_end};
    sensing_init(&hooks);
    gpio_init(BOARD_BUTTON_GPIO);
    gpio_set_dir(BOARD_BUTTON_GPIO, GPIO_IN);
    gpio_pull_up(BOARD_BUTTON_GPIO);

    uint32_t bits;
    rtos_flags_wait_any(g_flags, EVT_COMMS_READY, &bits, RTOS_WAIT_FOREVER);
    printf("sensors: PIR GP0, sound GP14, mic GP26, DHT22 GP4, light GP27, button GP20\n");

    bool was_down = false;
    uint32_t last_env_print = 0;
    for (;;)
    {
        uint32_t now = p_now(NULL);
        sensing_env_poll(now);
        bool want_env = rtos_flags_wait_any(g_flags, EVT_WANT_ENV, &bits, RTOS_NO_WAIT);
        if (want_env || now - last_env_print >= 10000)
        {
            last_env_print = now;
            print_env();
        }

        bool down = !gpio_get(BOARD_BUTTON_GPIO);
        bool pressed = down && !was_down;
        was_down = down;
        bool forced = pressed || rtos_flags_wait_any(g_flags, EVT_FORCE_CAPTURE, &bits, RTOS_NO_WAIT);
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
        rtos_delay_ms(10); /* their main.c polls every 10 ms; also debounces the button */
    }
}

/* ---- console + USB owner (merged, brief 3.6) ---------------------------------------------- */

static void print_help(void)
{
    printf("keys: c capture now | e sensor readings | s stats | d toggle 30%% loss | t toggle WiFi topology "
           "filter | H hold capture queue (T6) | p send next size-ladder test payload (A.3) | x T6: fire 5 "
           "captures fast | z T8: stall the console 5 s | B reboot to BOOTSEL (reflash) | h help\n");
}

/* T6 helper (brief Section 8): fires 5 capture requests back to back from the console task, so
 * the 5th meets a full 4-slot queue. Not perfectly deterministic (comms could drain in between);
 * 'H' then 'x' is. */
static void t6_fire_captures(void)
{
    capture_meta_t m;
    capture_meta_clear(&m);
    for (int i = 0; i < 5; i++)
    {
        bool ok = app_request_capture(&m);
        printf("[T6] capture request %d: %s\n", i + 1, ok ? "queued" : "refused");
    }
}

/* One log line to the host. Waits (up to 500 ms, like the FreeRTOS build's USB stdio) for room in
 * the kernel's USB transmit ring while a host has the port open; with no host, the line is
 * discarded at once, as USB stdio does, so nothing upstream ever backs up. */
static void usb_write_line(const char* s)
{
    size_t n = strlen(s);
    uint32_t t0 = rtos_now_ms();
    while (tm_usb_state() >= USB_STATE_OPEN)
    {
        uint32_t pending = tm_usb_pending();
        uint32_t room = USB_TX_RING_BYTES - 1u - (pending < USB_TX_RING_BYTES ? pending : USB_TX_RING_BYTES - 1u);
        if (room >= n + 2u)
        {
            tm_usb_console_put((const uint8_t*)s, (int32_t)n);
            tm_usb_console_put((const uint8_t*)"\r\n", 2);
            return;
        }
        if (rtos_elapsed_ms(t0) > 500u)
        {
            g_usb_lines_dropped++;
            return;
        }
        rtos_delay_ms(1);
    }
}

static void usb_owner_task(void* arg)
{
    (void)arg;
    log_set_drainer(rtos_self_id());
    /* Hold the start-up lines (they wait in the log ring) until a host opens the port, for up to
     * 3 s -- the FreeRTOS build sleeps 1.5 s before its first line for the same reason. */
    for (int i = 0; i < 300 && tm_usb_state() < USB_STATE_OPEN; i++)
    {
        rtos_delay_ms(10);
    }
    printf("usb_owner: core %u\n", rtos_core_id());
    print_help();

    uint32_t stall_until_ms = 0;
    char line[LOG_LINE_MAX];
    for (;;)
    {
        int ch;
        while ((ch = rtos_console_getchar()) >= 0)
        {
            switch (ch)
            {
            case 'c':
                rtos_flags_set(g_flags, EVT_FORCE_CAPTURE);
                break;
            case 'e':
                rtos_flags_set(g_flags, EVT_WANT_ENV);
                break;
            case 't':
                rtos_flags_set(g_flags, EVT_TOGGLE_TOPO);
                break;
            case 'H':
                rtos_flags_set(g_flags, EVT_TOGGLE_HOLD);
                break;
            case 'h':
            case '?':
                print_help();
                break;
            case 's':
                rtos_flags_set(g_flags, EVT_WANT_STATS);
                break;
            case 'd':
                rtos_flags_set(g_flags, EVT_TOGGLE_DROP);
                break;
            case 'p':
                rtos_flags_set(g_flags, EVT_TEST_PAYLOAD);
                break;
            case 'x':
                t6_fire_captures();
                break;
            case 'z':
                /* T8: stop draining for 5 s, as if the terminal had stopped reading. comms and
                 * sensors keep running; their lines wait, then drop and are counted. */
                stall_until_ms = rtos_now_ms() + 5000u;
                break;
            case 'B':
            {
                static const char msg[] = "rebooting to BOOTSEL (RPI-RP2 drive) for reflashing\r\n";
                tm_usb_console_put((const uint8_t*)msg, (int32_t)(sizeof(msg) - 1u));
                rtos_delay_ms(200);
                smartrelay_reboot_to_bootsel();
                break;
            }
            default:
                break;
            }
        }

        if (stall_until_ms != 0u && (int32_t)(rtos_now_ms() - stall_until_ms) < 0)
        {
            rtos_delay_ms(20);
            continue;
        }
        stall_until_ms = 0u;
        /* Block briefly for the next line, so output leaves within a few ms of being printed and
         * keys are still read every 10 ms when nothing is printing. */
        if (log_drain_one(line, 10))
        {
            usb_write_line(line);
            while (log_drain_one(line, RTOS_NO_WAIT))
            {
                usb_write_line(line);
            }
        }
    }
}

void app_main_smp(void)
{
    rtos_atomic_init();
    log_init();

    /* Round 2 review fix A4: confirm both queues hold exactly the depth they should, before any
     * task starts using either one. */
    char selftest_line[LOG_LINE_MAX];
    log_selftest(selftest_line, sizeof(selftest_line));
    log_write_line(selftest_line);

    board_spi1_idle(); /* LoRa CS and SD CS high before anything touches SPI1 */
    printf("\nCSC09 trail mesh, node %u, ALFP v%u (uT-Kernel 3 SMP build, %s)\n", NODE_ID, ALFP_VERSION,
#if defined(CNF_SMP) && CNF_SMP /* build_make/pico_rp2040.mk adds -DCNF_SMP=1 for SMP=1 */
           "dual core"
#else
           "single core"
#endif
    );
    printf("clocks (measured): sys %lu peri %lu usb %lu adc %lu Hz\n", (unsigned long)clock_get_hz(clk_sys),
           (unsigned long)clock_get_hz(clk_peri), (unsigned long)clock_get_hz(clk_usb),
           (unsigned long)clock_get_hz(clk_adc));
    if (SOUND_LEGACY_MODE)
    {
        printf("sound: LEGACY mode (mic.c thresholds, img_NN.jpg round robin), not the on-node classifier\n");
    }

    g_flags = rtos_flags_create();
    g_capture_q = rtos_msgq_create(sizeof(capture_meta_t), CAPTURE_Q_DEPTH, g_capture_q_buf);
    rtos_msgq_selftest(g_capture_q, sizeof(capture_meta_t), CAPTURE_Q_DEPTH, selftest_line, sizeof(selftest_line));
    log_write_line(selftest_line);

    g_owner_task_id = rtos_task_create("usb_owner", usb_owner_task, NULL, RTOS_PRIO_USB_OWNER, STACK_USB_OWNER,
                                       RTOS_CORE_0);
    g_comms_task_id = rtos_task_create("comms", comms_task, NULL, RTOS_PRIO_COMMS, STACK_COMMS, RTOS_CORE_0);
    g_sensors_task_id = rtos_task_create("sensors", sensors_task, NULL, RTOS_PRIO_SENSORS, STACK_SENSORS, RTOS_CORE_1);
    if (g_owner_task_id <= 0 || g_comms_task_id <= 0 || g_sensors_task_id <= 0)
    {
        printf("FATAL: task creation failed (usb_owner %ld comms %ld sensors %ld)\n", (long)g_owner_task_id,
               (long)g_comms_task_id, (long)g_sensors_task_id);
    }
}
