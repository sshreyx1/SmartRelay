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
#include "hardware/sync.h"
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
extern void tm_usb_console_put(const uint8_t* p_buf, int32_t size);
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
static const mesh_radio_t* g_radios[2];       /* preference: WiFi first, then LoRa */
static uint32_t g_capture_id;                 /* last id used; restored from the SD card at boot */
static char g_payload_names[TEST_PAYLOADS_MAX][32];
static uint8_t g_n_payloads;
static uint8_t g_next_payload;
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
    STEP_UPLINK,
    STEP_COUNT
};
static const char* const k_step_names[STEP_COUNT]
    = { "wifi poll", "lora poll", "alfp service", "captures", "outbox", "uplink" };
static uint32_t g_step_max_ms[STEP_COUNT];

static void step_done(int step, uint32_t* p_start_ms)
{
    uint32_t now = rtos_now_ms();
    uint32_t delta = now - *p_start_ms;
    if (delta > g_step_max_ms[step])
    {
        g_step_max_ms[step] = delta;
    }
    *p_start_ms = now;
}
static uint32_t g_usb_lines_dropped;

/* Bridge -> laptop: finished files wait here and go out a few lines per comms pass (uplink.h). */
static uplink_queue_t g_uplink;
#define UPLINK_LINES_PER_PASS 8u /* one SD read of UPLINK_CHUNK_BYTES */
#define UPLINK_LOG_RESERVE 16u   /* ring slots always left for every other task's lines */

/* Defect B.1: 's' printed a hard-coded core table. Each task now writes the core the kernel says
 * it is running on (tk_get_prc via rtos_core_id) on every pass of its loop. */
enum
{
    TASK_COMMS,
    TASK_USB_OWNER,
    TASK_SENSORS,
    TASK_COUNT
};
static volatile uint8_t g_task_core[TASK_COUNT] = { 0xFF, 0xFF, 0xFF };

/* ---- SD timing: every storage call the comms task makes, timed (bridge freeze measurement) ----
 * A thin wrapper over storage_fatfs() that records the slowest call of each kind and the single
 * slowest call overall with its file, for 's'. Costs two clock reads per call. */
enum
{
    SD_OPEN_R,
    SD_OPEN_W,
    SD_READ,
    SD_WRITE,
    SD_SIZE,
    SD_CLOSE,
    SD_REMOVE,
    SD_RENAME,
    SD_LIST,
    SD_MKDIR,
    SD_OPS
};
static const char* const k_sd_names[SD_OPS]
    = { "open_r", "open_w", "read", "write", "size", "close", "remove", "rename", "list", "mkdir" };
#define SD_HANDLES 8 /* storage_fatfs.c MAX_OPEN */
static uint32_t g_sd_max_ms[SD_OPS];
static uint32_t g_sd_worst_ms;
static uint8_t g_sd_worst_op;
static char g_sd_worst_path[ALFP_PATH_MAX + 8];
static char g_sd_handle_path[SD_HANDLES][ALFP_PATH_MAX + 8];

static const alfp_storage_t* sd_inner(void)
{
    return storage_fatfs();
}

static const char* sd_handle_path(int handle)
{
    return ((handle >= 0) && (handle < SD_HANDLES)) ? g_sd_handle_path[handle] : "?";
}

static void sd_note(int op_name, uint32_t start_t, const char* p_path)
{
    uint32_t delta = rtos_elapsed_ms(start_t);
    if (delta > g_sd_max_ms[op_name])
    {
        g_sd_max_ms[op_name] = delta;
    }
    if (delta > g_sd_worst_ms)
    {
        g_sd_worst_ms = delta;
        g_sd_worst_op = (uint8_t)op_name;
        snprintf(g_sd_worst_path, sizeof(g_sd_worst_path), "%s", p_path ? p_path : "");
    }
}

static int sd_open_common(int op_name, const char* p_path)
{
    uint32_t start_t = rtos_now_ms();
    int handle = SD_OPEN_R == op_name ? sd_inner()->p_open_read(sd_inner()->p_ctx, p_path)
                            : sd_inner()->p_open_write(sd_inner()->p_ctx, p_path);
    if ((handle >= 0) && (handle < SD_HANDLES))
    {
        snprintf(g_sd_handle_path[handle], sizeof(g_sd_handle_path[handle]), "%s", p_path);
    }
    sd_note(op_name, start_t, p_path);
    return handle;
}
static int sd_open_read(void* p_ctx, const char* p_path)
{
    (void)p_ctx;
    return sd_open_common(SD_OPEN_R, p_path);
}
static int sd_open_write(void* p_ctx, const char* p_path)
{
    (void)p_ctx;
    return sd_open_common(SD_OPEN_W, p_path);
}
static int sd_read_at(void* p_ctx, int handle, uint32_t off, uint8_t* p_buf, uint16_t len)
{
    (void)p_ctx;
    uint32_t start_t = rtos_now_ms();
    int ret = sd_inner()->p_read_at(sd_inner()->p_ctx, handle, off, p_buf, len);
    sd_note(SD_READ, start_t, sd_handle_path(handle));
    return ret;
}
static int sd_write_at(void* p_ctx, int handle, uint32_t off, const uint8_t* p_buf, uint16_t len)
{
    (void)p_ctx;
    uint32_t start_t = rtos_now_ms();
    int ret = sd_inner()->p_write_at(sd_inner()->p_ctx, handle, off, p_buf, len);
    sd_note(SD_WRITE, start_t, sd_handle_path(handle));
    return ret;
}
static int32_t sd_size(void* p_ctx, int handle)
{
    (void)p_ctx;
    uint32_t start_t = rtos_now_ms();
    int32_t ret = sd_inner()->p_size(sd_inner()->p_ctx, handle);
    sd_note(SD_SIZE, start_t, sd_handle_path(handle));
    return ret;
}
static int sd_close(void* p_ctx, int handle)
{
    (void)p_ctx;
    uint32_t start_t = rtos_now_ms();
    int ret = sd_inner()->p_close(sd_inner()->p_ctx, handle);
    sd_note(SD_CLOSE, start_t, sd_handle_path(handle));
    return ret;
}
static int sd_remove(void* p_ctx, const char* p_path)
{
    (void)p_ctx;
    uint32_t start_t = rtos_now_ms();
    int ret = sd_inner()->p_remove(sd_inner()->p_ctx, p_path);
    sd_note(SD_REMOVE, start_t, p_path);
    return ret;
}
static int sd_rename(void* p_ctx, const char* p_from, const char* p_to_path)
{
    (void)p_ctx;
    uint32_t start_t = rtos_now_ms();
    int ret = sd_inner()->p_rename(sd_inner()->p_ctx, p_from, p_to_path);
    sd_note(SD_RENAME, start_t, p_to_path);
    return ret;
}
static int sd_list(void* p_ctx, const char* p_dir, void (*p_callback)(const char* p_name, void* p_user), void* p_user)
{
    (void)p_ctx;
    uint32_t start_t = rtos_now_ms();
    int ret = sd_inner()->p_list(sd_inner()->p_ctx, p_dir, p_callback, p_user);
    sd_note(SD_LIST, start_t, p_dir);
    return ret;
}
static int sd_mkdir(void* p_ctx, const char* p_dir)
{
    (void)p_ctx;
    uint32_t start_t = rtos_now_ms();
    int ret = sd_inner()->p_mkdir(sd_inner()->p_ctx, p_dir);
    sd_note(SD_MKDIR, start_t, p_dir);
    return ret;
}
static const alfp_storage_t k_timed_storage = { NULL,     sd_open_read, sd_open_write, sd_read_at, sd_write_at, sd_size,
                                                sd_close, sd_remove,    sd_rename,     sd_list,    sd_mkdir };

/* ---- stdout: every printf/puts/putchar byte (pico_runtime_mtk3.c) ------------------------ */

void smartrelay_stdout_putc(char chr)
{
    log_putc(chr);
}

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
    if (ALFP_TX_REFUSED == p_rep->result)
    {
        static const char* const k_ack[] = { "OK", "CRC_FAIL", "BUSY", "STORAGE (bridge SD)", "TOO_BIG" };
        printf("tx %s: bridge refused it with status %u = %s\n", p_rep->p_path, p_rep->ack_status,
               p_rep->ack_status < 5u ? k_ack[p_rep->ack_status] : "?");
    }
    printf("tx %s: %s via %s, %u frags, %u rounds, %u resent, %.1f s\n", p_rep->p_path, alfp_tx_result_str(p_rep->result),
           p_rep->p_radio, p_rep->frag_count, p_rep->rounds, p_rep->data_resent, (p_rep->finished_ms - p_rep->started_ms) / 1000.0);
    if ((ALFP_TX_DONE == p_rep->result) && (0 == strncmp(p_rep->p_path, TEL_DIR "/", sizeof(TEL_DIR "/") - 1u)))
    {
        /* The telemetry job's "file" was a temp copy of its own meta made just for this transfer:
         * remove it once sent, as main.c does. */
        k_timed_storage.p_remove(k_timed_storage.p_ctx, p_rep->p_path);
    }
    alfp_outbox_on_tx_done(&g_outbox, p_rep);
}

static void uplink_out(const char* p_str, void* p_user)
{
    (void)p_user;
    /* uplink_send_file() writes a line in pieces ("@ALFP FILE ...meta=", the hex, "\n"); log_putc
     * collects them per task, so each uplink line still reaches the host whole. */
    while ('\0' != *p_str)
    {
        log_putc(*p_str++);
    }
}

/* Runs inside alfp_service() the moment a file is complete. It used to stream the whole file to the
 * laptop right here (~200 SD reads + ~200 log lines per image, 3.4-4 s with both radios unpolled
 * and the sender's ACK held back). Now it only queues the report; the comms loop sends it a few
 * lines per pass (uplink_pump). Only if the queue is full does it fall back to the old way. */
static void on_rx_done(alfp_ctx_t* p_ctx, const alfp_rx_report_t* p_rep)
{
    printf("rx from node %u: %s, %lu bytes via %s, %u dup\n", p_rep->src, p_rep->p_path, (unsigned long)p_rep->file_size, p_rep->p_radio,
           p_rep->duplicates);
    if (!uplink_queue_add(&g_uplink, p_rep))
    {
        printf("uplink: queue full (%u files), sending %s inline\n", (unsigned)UPLINK_QUEUE_DEPTH, p_rep->p_path);
        uplink_send_file(p_ctx->cfg.p_storage, p_rep, uplink_out, NULL);
    }
}

/* Bridge only, once per comms pass: send up to UPLINK_LINES_PER_PASS lines of waiting files, but
 * only as many as the log ring has room for, so comms never waits on a full ring (or a slow USB
 * host) here. */
static void uplink_pump(void)
{
    if (0u == uplink_queue_waiting(&g_uplink))
    {
        return;
    }
    uint32_t room = log_free_slots();
    uint32_t budget = room > UPLINK_LOG_RESERVE ? room - UPLINK_LOG_RESERVE : 0u;
    if (budget > UPLINK_LINES_PER_PASS)
    {
        budget = UPLINK_LINES_PER_PASS;
    }
    (void)uplink_queue_service(&g_uplink, budget, uplink_out, NULL);
}

/* ---- capture requests (any task) --------------------------------------------------------- */

bool app_request_capture(const capture_meta_t* p_sensors)
{
    if (g_capture_q < 0)
    {
        return false;
    }
    if (rtos_msgq_send_nowait(g_capture_q, p_sensors, sizeof(*p_sensors)))
    {
        rtos_atomic_add_u32(&g_capture_q_waiting, 1);
        return true;
    }
    /* app_request_capture runs on sensors (core 1) and usb_owner (core 0, 'x' test key) */
    rtos_atomic_add_u32(&g_capture_queue_full_count, 1);
    return false;
}

static void count_samples_cb(const char* p_name, void* p_user)
{
    (void)p_user;
    sip_add_file(&g_images, p_name);
}

static void count_cb(const char* p_name, void* p_user)
{
    (void)p_name;
    (*(uint32_t*)p_user)++;
}

static void count_payloads_cb(const char* p_name, void* p_user)
{
    (void)p_user;
    size_t name_len = strlen(p_name);
    if ('.' == p_name[0])
    {
        return; /* F1: macOS "._*" AppleDouble files and other hidden files are not payloads */
    }
    if ((g_n_payloads < TEST_PAYLOADS_MAX) && (name_len < sizeof(g_payload_names[0])))
    {
        memcpy(g_payload_names[g_n_payloads], p_name, name_len + 1u);
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
    unsigned long value = 0;
    return (len > 0) && (1 == sscanf(txt, "%lu", &value)) ? (uint32_t)value : 0;
}

/* write to a temp file, then rename over the real one: a power cut leaves one of the two intact */
static void store_next_id(const alfp_storage_t* p_store, uint32_t next)
{
    char txt[16];
    int len = snprintf(txt, sizeof(txt), "%lu\n", (unsigned long)next);
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
    unsigned long seq;
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
        snprintf(path, sizeof(path), "outbox/q%08lu.job", (unsigned long)s_job_seq[idx]);
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

static void restore_capture_id(const alfp_storage_t* p_store)
{
    p_store->p_mkdir(p_store->p_ctx, "meta");
    uint32_t next = read_u32_file(p_store, ID_FILE);
    uint32_t tmp = read_u32_file(p_store, ID_TMP);
    next = tmp > next ? tmp : next;
    uint32_t seen = highest_id_on_card(p_store);
    g_capture_id = next > seen + 1u ? next - 1u : seen;
    char id_str[CAPTURE_ID_STR_MAX];
    capture_id_format(id_str, sizeof(id_str), NODE_ID, g_capture_id + 1u);
    printf("capture id: next is %s (saved next %lu, highest on card %lu)\n", id_str, (unsigned long)next,
           (unsigned long)seen);
}

static void handle_capture(const capture_meta_t* p_request)
{
    const alfp_storage_t* p_store = &k_timed_storage;
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

    /* meta = the 27-byte record + the image's file name (the optional-name tail, <= 64 bytes in all) */
    uint8_t meta_buf[ALFP_MAX_META];
    uint8_t meta_len = capture_meta_pack(&meta, meta_buf);

    if (APP_SPLIT_TELEMETRY || (image_num < 0))
    {
        /* Telemetry: a tiny transfer tagged with APP_TELEMETRY_MARKER instead of an image name,
         * small enough that the size rule always sends it over LoRa (see main.c). */
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

    /* Image: meta + the image's file name (the optional-name tail), exactly as main.c. */
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

static void print_stats(void)
{
    const alfp_stats_t* p_stats = &g_alfp.stats;
    printf("node %u | alfp in %lu badcrc %lu | tx ok %lu fail %lu | rx ok %lu crcfail %lu expired %lu | outbox %u\n",
           (unsigned)NODE_ID, (unsigned long)p_stats->pkts_in, (unsigned long)p_stats->pkts_bad_crc, (unsigned long)p_stats->tx_ok,
           (unsigned long)p_stats->tx_failed, (unsigned long)p_stats->rx_ok, (unsigned long)p_stats->rx_crc_fail,
           (unsigned long)p_stats->rx_expired, alfp_outbox_count(&g_outbox));
    for (int idx = 0; idx < 2; idx++)
    {
        mesh_stats_t stats;
        g_radios[idx]->p_get_stats(&stats);
        printf("  %s: tx %lu rx %lu delivered %lu relayed %lu dup %lu rxerr %lu\n", g_radios[idx]->p_name,
               (unsigned long)stats.tx_count, (unsigned long)stats.rx_count, (unsigned long)stats.delivered, (unsigned long)stats.relayed,
               (unsigned long)stats.dup_dropped, (unsigned long)stats.rx_error);
    }
    const sensing_env_t* p_env = sensing_env();
    printf("  dht22 good %lu fail %lu | lora %s | wifi %s\n", (unsigned long)p_env->dht_good, (unsigned long)p_env->dht_fail,
           radios_lora_ok() ? "up" : "DOWN", radios_wifi_ok() ? "up" : "DOWN");
    uint32_t now_ms;
    for (int idx = 0; idx < 2; idx++)
    {
        if (g_radios[idx]->p_last_heard(BRIDGE_ID, &now_ms))
        {
            printf("  bridge last heard on %s %lu ms ago\n", g_radios[idx]->p_name, (unsigned long)(p_now(NULL) - now_ms));
        }
        else
        {
            printf("  bridge never heard on %s\n", g_radios[idx]->p_name);
        }
    }

    /* --- Section 7: per-core / per-task view of the dual-core build ---
     * Defect B.1: core and priority are what the kernel reports now (each task's last
     * tk_get_prc(), and tk_ref_tsk() for the priority), not the values the code asked for. */
    struct
    {
        const char* p_name;
        rtos_id_t task_id;
        uint32_t stack;
    } const tasks[TASK_COUNT] = {
        [TASK_COMMS] = { "comms", g_comms_task_id, STACK_COMMS },
        [TASK_USB_OWNER] = { "usb_owner", g_owner_task_id, STACK_USB_OWNER },
        [TASK_SENSORS] = { "sensors", g_sensors_task_id, STACK_SENSORS },
    };
    for (int idx = 0; idx < TASK_COUNT; idx++)
    {
        printf("  task %s: core %u prio %u stack free %lu of %lu B\n", tasks[idx].p_name, (unsigned)g_task_core[idx],
               rtos_task_priority(tasks[idx].task_id), (unsigned long)rtos_task_stack_free(tasks[idx].task_id),
               (unsigned long)tasks[idx].stack);
    }
    printf("  longest comms loop %lu ms | capture q full %lu | log dropped %lu lines | usb dropped %lu lines %lu B"
           " | heap used %lu B\n",
           (unsigned long)g_longest_comms_loop_ms, (unsigned long)g_capture_queue_full_count,
           (unsigned long)log_dropped_count(), (unsigned long)g_usb_lines_dropped, (unsigned long)tm_usb_dropped,
           (unsigned long)smartrelay_heap_used());
    printf("  outbox dropped (full) %lu | msgq errors %lu (last %ld)", (unsigned long)g_outbox.dropped_full,
           (unsigned long)rtos_msgq_error_count(), (long)rtos_msgq_last_error());
    if (NODE_ID == BRIDGE_ID)
    {
        printf(" | uplink sent %lu bad %lu waiting %u (max %u) inline %lu", (unsigned long)g_uplink.sent_ok,
               (unsigned long)g_uplink.sent_bad, uplink_queue_waiting(&g_uplink), (unsigned)g_uplink.max_waiting,
               (unsigned long)g_uplink.queue_full);
    }
    printf("\n");
    {
        char sdst[160];
        storage_fatfs_status(sdst, sizeof(sdst));
        printf("  %s\n", sdst);
    }
    printf("  slowest SD call (ms):");
    for (int idx = 0; idx < SD_OPS; idx++)
    {
        printf(" %s %lu", k_sd_names[idx], (unsigned long)g_sd_max_ms[idx]);
    }
    printf(" | worst %s %lu ms on %s\n", k_sd_names[g_sd_worst_op], (unsigned long)g_sd_worst_ms, g_sd_worst_path);
    printf("  slowest comms step (ms):");
    for (int idx = 0; idx < STEP_COUNT; idx++)
    {
        printf(" %s %lu%s", k_step_names[idx], (unsigned long)g_step_max_ms[idx], idx + 1 < STEP_COUNT ? " |" : "\n");
    }
}

static void comms_task(void* p_arg)
{
    (void)p_arg;
    g_task_core[TASK_COMMS] = (uint8_t)rtos_core_id();
    printf("comms: core %u\n", g_task_core[TASK_COMMS]);

    /* The SD card goes first: it must be switched to SPI mode before any LoRa traffic appears on
     * the shared SPI1 bus (board.h). */
    while (!storage_fatfs_mount())
    {
        printf("sd: mount failed (card in the Maker slot? FAT32?), retrying\n");
        rtos_delay_ms(2000);
    }
    const alfp_storage_t* p_store = &k_timed_storage;
    p_store->p_mkdir(p_store->p_ctx, "samples");
    p_store->p_mkdir(p_store->p_ctx, TEL_DIR); /* also used without APP_SPLIT_TELEMETRY when a capture has no image */
    sip_reset(&g_images);
    p_store->p_list(p_store->p_ctx, "samples", count_samples_cb, NULL);
    unsigned n_img = 0;
    for (uint32_t idx = 0; idx < g_images.count; idx++)
    {
        n_img += g_images.prefixes[idx].count;
    }
    {
        char line[LOG_LINE_MAX];
        int len
            = snprintf(line, sizeof(line), "sd: mounted, %u sample images in %u groups:", n_img, (unsigned)g_images.count);
        for (uint32_t idx = 0; (idx < g_images.count) && (len > 0) && ((size_t)len < sizeof(line)); idx++)
        {
            len += snprintf(line + len, sizeof(line) - (size_t)len, " %s %u", g_images.prefixes[idx].prefix,
                            g_images.prefixes[idx].count);
        }
        printf("%s\n", line);
    }
    p_store->p_list(p_store->p_ctx, TEST_PAYLOADS_DIR, count_payloads_cb, NULL);
    printf("sd: %u test payload file(s) in %s/\n", g_n_payloads, TEST_PAYLOADS_DIR);
    restore_capture_id(p_store);
    if (NODE_ID == BRIDGE_ID)
    {
        /* inbox/ is where firmware older than the rcv/<xx>/ layout kept every file in one folder.
         * Nothing reads or writes it any more; say how much is left there (safe to delete). */
        uint32_t t_list = rtos_now_ms();
        uint32_t n_flat = 0;
        p_store->p_list(p_store->p_ctx, "inbox", count_cb, &n_flat);
        printf("sd: old inbox/ holds %lu files (no longer used, can be deleted), listed in %lu ms\n",
               (unsigned long)n_flat, (unsigned long)rtos_elapsed_ms(t_list));
    }

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
    alfp_init(&g_alfp, &cfg);
    uplink_queue_init(&g_uplink, p_store);
    alfp_outbox_init(&g_outbox, &g_alfp, g_radios, 2, BRIDGE_ID, BRIDGE_FRESH_MS, "outbox");
    alfp_outbox_set_size_threshold(&g_outbox, APP_RADIO_SIZE_THRESHOLD);
    printf("node %u ready (%s), outbox has %u queued, radio size threshold %u B\n", (unsigned)NODE_ID,
           NODE_ID == BRIDGE_ID ? "bridge" : "field node", alfp_outbox_count(&g_outbox),
           (unsigned)APP_RADIO_SIZE_THRESHOLD);

    rtos_flags_set(g_flags, EVT_COMMS_READY);
    bool b_dropping = false, b_topo = true, b_hold = false;
    for (;;)
    {
        uint32_t loop_start = rtos_now_ms();
        uint32_t now_ms = loop_start;
        g_radios[0]->p_poll();
        step_done(STEP_WIFI_POLL, &now_ms);
        g_radios[1]->p_poll();
        step_done(STEP_LORA_POLL, &now_ms);
        alfp_service(&g_alfp, g_radios, 2);
        step_done(STEP_ALFP_SERVICE, &now_ms);
        if (NODE_ID != BRIDGE_ID)
        {
            capture_meta_t req;
            while ((!b_hold) && (rtos_msgq_recv(g_capture_q, &req, sizeof(req), RTOS_NO_WAIT)))
            {
                rtos_atomic_add_u32(&g_capture_q_waiting, (uint32_t)-1);
                handle_capture(&req);
            }
            step_done(STEP_CAPTURES, &now_ms);
            alfp_outbox_pump(&g_outbox); /* radios and the outbox keep running while held */
            step_done(STEP_OUTBOX, &now_ms);
        }
        else
        {
            uplink_pump();
            step_done(STEP_UPLINK, &now_ms);
        }
        g_task_core[TASK_COMMS] = (uint8_t)rtos_core_id();

        uint32_t bits = 0;
        if (rtos_flags_wait_any(g_flags, EVT_COMMS_MASK, &bits, RTOS_NO_WAIT))
        {
            if (bits & EVT_WANT_STATS)
            {
                print_stats();
            }
            if (bits & EVT_TOGGLE_DROP)
            {
                b_dropping = !b_dropping;
                g_radios[0]->p_set_drop_rate(b_dropping ? 0.3f : 0.0f);
                g_radios[1]->p_set_drop_rate(b_dropping ? 0.3f : 0.0f);
                printf("injected loss %s\n", b_dropping ? "30%" : "off");
            }
            if (bits & EVT_TOGGLE_HOLD)
            {
                b_hold = !b_hold;
                printf("capture queue %s (%u waiting)\n",
                       b_hold ? "HELD for test T6: captures stay in the 4-slot queue" : "released, sending what waited",
                       (unsigned)g_capture_q_waiting);
            }
            if (bits & EVT_TEST_PAYLOAD)
            {
                send_next_test_payload();
            }
            if (bits & EVT_TOGGLE_TOPO)
            {
                b_topo = !b_topo;
                radios_set_wifi_topology_filter(b_topo);
                printf("wifi topology filter %s\n",
                       b_topo ? "ON (demo layout, forces multi-hop)" : "OFF (hear everyone)");
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

static void rtos_wait_ms(uint32_t msec)
{
    rtos_delay_ms(msec);
}

/* DHT22.c times the sensor's bits by counting loop iterations and must not be interrupted during
 * the ~5 ms bit-timing part of the read. Post-Tri1 decision: interrupt masking instead of the
 * priority boost. On the dual-core build sensors is alone on core 1, so a priority boost protected
 * nothing (no other task there) while interrupts could still land; masking core 1's interrupts
 * delays nothing else: the radio UART/SPI interrupts and the kernel tick all run on core 0
 * (docs/curveball_findings.md: core 0 is the sole timekeeper). DHT22.c calls these only around the
 * bit-timing loop, never around its 20 ms start pulse, and that loop runs from RAM (sources.mk).
 * SMP=0 (everything on one core): masking 5 ms there would stall the ESP-01 UART, so that build
 * keeps the priority boost, like the FreeRTOS build. */
#if defined(CNF_SMP) && CNF_SMP
static uint32_t s_dht_irq_state;
void DHT_timing_enter(void)
{
    s_dht_irq_state = save_and_disable_interrupts();
}
void DHT_timing_exit(void)
{
    restore_interrupts(s_dht_irq_state);
}
#define DHT_BEGIN_HOOK NULL
#define DHT_END_HOOK NULL
#else
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
#define DHT_BEGIN_HOOK dht_begin
#define DHT_END_HOOK dht_end
#endif

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
        printf("[ENV] DHT22: no reading (%lu fails) | Light: %u | Weather: unknown | PIR %d Sound %d\n",
               (unsigned long)p_env->dht_fail, p_env->light, sensing_pir(), sensing_sound_trigger());
    }
}

static void sensors_task(void* p_arg)
{
    (void)p_arg;
    g_task_core[TASK_SENSORS] = (uint8_t)rtos_core_id();
    printf("sensors: core %u\n", g_task_core[TASK_SENSORS]);
    const sensing_hooks_t hooks = { rtos_wait_ms, DHT_BEGIN_HOOK, DHT_END_HOOK };
    sensing_init(&hooks);
    gpio_init(BOARD_BUTTON_GPIO);
    gpio_set_dir(BOARD_BUTTON_GPIO, GPIO_IN);
    gpio_pull_up(BOARD_BUTTON_GPIO);

    uint32_t bits;
    rtos_flags_wait_any(g_flags, EVT_COMMS_READY, &bits, RTOS_WAIT_FOREVER);
    printf("sensors: PIR GP0, sound GP14, mic GP26, DHT22 GP4, light GP27, button GP20\n");

    bool b_was_down = false;
    uint32_t last_env_print = 0;
    for (;;)
    {
        g_task_core[TASK_SENSORS] = (uint8_t)rtos_core_id();
        uint32_t now = p_now(NULL);
        sensing_env_poll(now);
        bool b_want_env = rtos_flags_wait_any(g_flags, EVT_WANT_ENV, &bits, RTOS_NO_WAIT);
        if (b_want_env || (now - last_env_print >= 10000))
        {
            last_env_print = now;
            print_env();
        }

        bool b_down = !gpio_get(BOARD_BUTTON_GPIO);
        bool b_pressed = b_down && (!b_was_down);
        b_was_down = b_down;
        bool b_forced = b_pressed || (rtos_flags_wait_any(g_flags, EVT_FORCE_CAPTURE, &bits, RTOS_NO_WAIT));
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
            printf("[AUDIO] legacy mic.c: %s energy %.1f zcr %.3f mean %u, %lu samples\n", audio.p_label, audio.energy, audio.zcr,
                   audio.mean_adc, (unsigned long)audio.samples);
            sensing_log_sound();
            print_env();
            if ((NODE_ID != BRIDGE_ID) && (!app_request_capture(&meta)))
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
           "captures fast | z T8: stall the console 5 s | k measure the 1 ms tick (1 s) | w which ESP-01 (WST) | B "
           "reboot to BOOTSEL (reflash) "
           "| h help\n");
}

/* T6 helper (brief Section 8): fires 5 capture requests back to back from the console task, so
 * the 5th meets a full 4-slot queue. Not perfectly deterministic (comms could drain in between);
 * 'H' then 'x' is. */
static void t6_fire_captures(void)
{
    capture_meta_t meta;
    capture_meta_clear(&meta);
    for (int idx = 0; idx < 5; idx++)
    {
        bool b_is_ok = app_request_capture(&meta);
        printf("[T6] capture request %d: %s\n", idx + 1, b_is_ok ? "queued" : "refused");
    }
}

/* 'k': is the kernel tick really 1 ms? 1000 rtos_delay_ms(1) calls, each timed with the RP2040's
 * free-running 1 MHz hardware timer (independent of the kernel), plus the kernel clock against that
 * timer since boot. A 1 ms tick gives intervals of 1-2 ms (a delay of 1 tick ends at the next tick
 * or the one after) and a mean near 1 ms; a 10 ms tick would show ~10 ms. Blocks this task for
 * about 1 s; comms and sensors keep running. */
static void measure_tick(void)
{
    uint32_t min_us = UINT32_MAX, max_us = 0;
    uint64_t sum_us = 0;
    rtos_delay_ms(1); /* start on a tick edge */
    uint32_t start_ms = rtos_now_ms();
    uint64_t prev = time_us_64();
    uint64_t hw0 = prev;
    unsigned within_2ms = 0;
    for (int idx = 0; idx < 1000; idx++)
    {
        rtos_delay_ms(1);
        uint64_t nowus = time_us_64();
        uint32_t delta = (uint32_t)(nowus - prev);
        prev = nowus;
        sum_us += delta;
        within_2ms += delta <= 2100u;
        min_us = delta < min_us ? delta : min_us;
        max_us = delta > max_us ? delta : max_us;
    }
    /* tk_get_tim() is calendar time, not time since boot: compare elapsed against elapsed */
    uint32_t k_ms = rtos_elapsed_ms(start_ms);
    uint32_t hw_ms = (uint32_t)((time_us_64() - hw0) / 1000u);
    printf("tick: 1000 x delay(1 ms): mean %lu us, min %lu us, max %lu us, %u of 1000 <= 2.1 ms | kernel clock "
           "advanced %lu ms while the hw timer advanced %lu ms\n",
           (unsigned long)(sum_us / 1000u), (unsigned long)min_us, (unsigned long)max_us, within_2ms,
           (unsigned long)k_ms, (unsigned long)hw_ms);
}

/* 'w': which ESP-01 is in this board's socket, and whether it matches this image (radios_init()
 * reads its MAC once at boot; read-only afterwards, so the console may read it). */
static void print_wst(void)
{
    uint8_t mac[6] = { 0 };
    int owner = radios_wst_owner(mac);
    if (owner < 0)
    {
        printf("wst: no ESP-01 answered at boot | firmware node %u\n", (unsigned)NODE_ID);
    }
    else if (0 == owner)
    {
        printf("wst: ESP-01 MAC %02X:%02X:%02X:%02X:%02X:%02X is not in wifi_mesh_config.h | firmware node %u\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (unsigned)NODE_ID);
    }
    else
    {
        printf("wst: ESP-01 MAC %02X:%02X:%02X:%02X:%02X:%02X = WST-%d | firmware node %u | %s\n", mac[0], mac[1],
               mac[2], mac[3], mac[4], mac[5], owner, (unsigned)NODE_ID,
               (int)NODE_ID == owner ? "MATCH" : "MISMATCH: flash csc09_smp_node_<WST number>.uf2");
    }
}

/* One log line to the host. Waits (up to 500 ms, like the FreeRTOS build's USB stdio) for room in
 * the kernel's USB transmit ring while a host has the port open; with no host, the line is
 * discarded at once, as USB stdio does, so nothing upstream ever backs up. */
static void usb_write_line(const char* p_str)
{
    size_t len = strlen(p_str);
    uint32_t start_t = rtos_now_ms();
    while (tm_usb_state() >= USB_STATE_OPEN)
    {
        uint32_t pending = tm_usb_pending();
        uint32_t room = USB_TX_RING_BYTES - 1u - (pending < USB_TX_RING_BYTES ? pending : USB_TX_RING_BYTES - 1u);
        if (room >= len + 2u)
        {
            tm_usb_console_put((const uint8_t*)p_str, (int32_t)len);
            tm_usb_console_put((const uint8_t*)"\r\n", 2);
            return;
        }
        if (rtos_elapsed_ms(start_t) > 500u)
        {
            g_usb_lines_dropped++;
            return;
        }
        rtos_delay_ms(1);
    }
}

static void usb_owner_task(void* p_arg)
{
    (void)p_arg;
    log_set_drainer(rtos_self_id());
    /* Hold the start-up lines (they wait in the log ring) until a host opens the port, for up to
     * 10 s (was 3 s: macOS can take longer than that to bring the port up after a plug-in, and the
     * boot log, with the WST line, was lost). The ring holds 64 lines, enough for the boot log. */
    for (int idx = 0; (idx < 1000) && (tm_usb_state() < USB_STATE_OPEN); idx++)
    {
        rtos_delay_ms(10);
    }
    g_task_core[TASK_USB_OWNER] = (uint8_t)rtos_core_id();
    printf("usb_owner: core %u\n", g_task_core[TASK_USB_OWNER]);
    print_help();

    uint32_t stall_until_ms = 0;
    char line[LOG_LINE_MAX];
    for (;;)
    {
        g_task_core[TASK_USB_OWNER] = (uint8_t)rtos_core_id();
        int chan;
        while ((chan = rtos_console_getchar()) >= 0)
        {
            switch (chan)
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
                case 'k':
                    measure_tick();
                    break;
                case 'w':
                    print_wst();
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

        if ((0u != stall_until_ms) && ((int32_t)(rtos_now_ms() - stall_until_ms) < 0))
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
    printf("\nCSC09 trail mesh, node %u, ALFP v%u (uT-Kernel 3 SMP build, %s)\n", (unsigned)NODE_ID, ALFP_VERSION,
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

    g_owner_task_id
        = rtos_task_create("usb_owner", usb_owner_task, NULL, RTOS_PRIO_USB_OWNER, STACK_USB_OWNER, RTOS_CORE_0);
    g_comms_task_id = rtos_task_create("comms", comms_task, NULL, RTOS_PRIO_COMMS, STACK_COMMS, RTOS_CORE_0);
    g_sensors_task_id = rtos_task_create("sensors", sensors_task, NULL, RTOS_PRIO_SENSORS, STACK_SENSORS, RTOS_CORE_1);
    if ((g_owner_task_id <= 0) || (g_comms_task_id <= 0) || (g_sensors_task_id <= 0))
    {
        printf("FATAL: task creation failed (usb_owner %ld comms %ld sensors %ld)\n", (long)g_owner_task_id,
               (long)g_comms_task_id, (long)g_sensors_task_id);
    }
}
