/*
 * main_smp.c - CURVEBALL port of firmware/app/main.c onto uT-Kernel 3 SMP.
 * Same node firmware, same behaviour, ported per docs/curveball_findings.md
 * and the Tribunal decisions (brief section 3). The original firmware/app/
 * main.c and its CMake/FreeRTOS build are untouched (brief hard rule 4) -
 * this is the additive new build tree; see firmware_smp/build/README.md.
 *
 * Core layout (3.1): comms + this merged console/USB-owner task on core 0,
 * sensors alone on core 1. SMP=0 collapses both to core 0 unchanged (the
 * kernel's own single-core fallback, see findings Step 2 item 9).
 */
/* tk/syslib.h typedefs size_t itself (as SZ, a long int), conflicting with the real size_t
 * (<string.h>/<stdio.h>'s stddef.h, unsigned int) the moment both are in one translation unit --
 * confirmed on a real toolchain run, round 3, once usb_console_compat.h's own <tk/tkernel.h>
 * include joined this file's existing <stdio.h>/<string.h>. Same fix and same guard as
 * rtos_port_mtk3.c and the kernel's own lib/liblwip/sysdepend/pico_rp2040/sys_arch.c. */
#define PROHIBIT_DEF_SIZE_T

#include <stdio.h>
#include <string.h>

#include "alfp.h"
#include "alfp_outbox.h"
#include "board.h"
#include "capture_api.h"
#include "hardware/gpio.h"
#include "log.h"
#include "radios.h"
#include "rtos_port.h"
#include "sensing.h"
#include "storage_fatfs.h"
#include "uplink.h"

#include <tm/tmonitor.h>
/* tm_usb_state() -- vendored in third_party/mtk3smp_rp2040/app_program/ alongside this file once
 * overlaid there (not something build_smp.sh copies in: it was already part of the port's own
 * app_program/, used by its original app_main.c too). Confirmed missing on a real toolchain run,
 * round 3: "implicit declaration of function 'tm_usb_state'". */
#include "usb_console_compat.h"

#ifndef NODE_ID
#define NODE_ID 2
#endif
#define BRIDGE_ID 1
#define BRIDGE_FRESH_MS 10000u

/* Task A.2 (CURVEBALL round 3), same rationale as firmware/app/main.c: send capture metadata as
 * its own tiny ALFP transfer so the size rule (APP_RADIO_SIZE_THRESHOLD, alfp_config.h) routes it
 * over LoRa while the image goes over WiFi. 0 restores the single combined transfer. */
#ifndef APP_SPLIT_TELEMETRY
#define APP_SPLIT_TELEMETRY 1
#endif
#define TEL_DIR "tel"
/* See firmware/app/main.c for the full rationale: marks the telemetry job's filename-tail so the
 * backend/tools can tell "no image in this transfer" apart from a transfer with a real image and
 * simply no name tail (which this file's image jobs never did before this round, but keeping the
 * same explicit marker avoids relying on that distinction at all). Kept in sync by hand with
 * tools/bridge_receiver.py's TELEMETRY_MARKER and backend/app.py's own copy. */
#define APP_TELEMETRY_MARKER "TELEMETRY"

/* ---- console -> comms/sensors signalling (3.3: event flags, not volatile bools) ---- */
#define EVT_WANT_STATS (1u << 0)
#define EVT_TOGGLE_DROP (1u << 1)
#define EVT_TOGGLE_TOPO (1u << 2)
#define EVT_FORCE_CAPTURE (1u << 3)
#define EVT_WANT_ENV (1u << 4)
#define EVT_COMMS_READY (1u << 5) /* comms -> sensors, set once */

static alfp_ctx_t g_alfp;
static alfp_outbox_t g_outbox;
static rtos_id_t g_capture_q = -1;
#define CAPTURE_Q_DEPTH 4
static uint8_t g_capture_q_buf[RTOS_MSGQ_BYTES(sizeof(capture_meta_t), CAPTURE_Q_DEPTH)];
static const mesh_radio_t* g_radios[2];
static uint32_t g_capture_id;
static uint8_t g_n_samples, g_next_sample;
static rtos_id_t g_flags = -1;

static rtos_id_t g_comms_task_id = -1;
static rtos_id_t g_sensors_task_id = -1;
static rtos_id_t g_owner_task_id = -1;

/* Section 7 stats additions. */
static volatile uint32_t g_capture_queue_full_count;
static uint32_t g_longest_comms_loop_ms;

/* ---- platform ---------------------------------------------------------------------------- */

static uint32_t p_now(void* c)
{
    (void)c;
    return rtos_now_ms();
}

static uint32_t p_rand(void* c)
{
    (void)c;
    /* no verified hardware RNG call in this port's cited API (findings doc);
     * a counter mixed with the free-running clock is good enough for ALFP's
     * use (jitter/backoff), which does not need cryptographic randomness. */
    static uint32_t s_ctr;
    s_ctr += 2654435761u;
    return s_ctr ^ rtos_now_ms();
}

static void p_log(void* c, const char* m)
{
    (void)c;
    log_write_line(m);
}

static const alfp_platform_t k_plat = {NULL, p_now, p_rand, p_log};

/* ---- callbacks ---------------------------------------------------------------------------- */

static void on_tx_done(alfp_ctx_t* ctx, const alfp_tx_report_t* r)
{
    (void)ctx;
    char line[LOG_LINE_MAX];
    uint32_t secs_x10 = (r->finished_ms - r->started_ms) / 100u;
    tm_sprintf((UB*)line, (UB*)"tx %s: %s via %s, %u frags, %u rounds, %u resent, %u.%us", r->path,
               alfp_tx_result_str(r->result), r->radio, r->frag_count, r->rounds, r->data_resent,
               secs_x10 / 10u, secs_x10 % 10u);
    log_write_line(line);
    if (r->result == ALFP_TX_DONE && strncmp(r->path, TEL_DIR "/", sizeof(TEL_DIR "/") - 1u) == 0)
    {
        /* Telemetry's "file" is a temp copy of its own meta made just for this transfer (unlike
         * the image, a reused sample on the SD card): remove it once sent. */
        storage_fatfs()->remove(storage_fatfs()->ctx, r->path);
    }
    alfp_outbox_on_tx_done(&g_outbox, r);
}

static void uplink_out(const char* s, void* user)
{
    (void)user;
    log_write_line(s);
}

static void on_rx_done(alfp_ctx_t* ctx, const alfp_rx_report_t* r)
{
    char line[LOG_LINE_MAX];
    tm_sprintf((UB*)line, (UB*)"rx from node %u: %s, %lu bytes via %s, %u dup", r->src, r->path,
               (unsigned long)r->file_size, r->radio, r->duplicates);
    log_write_line(line);
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
        return true;
    }
    rtos_atomic_add_u32(&g_capture_queue_full_count, 1); /* app_request_capture runs on sensors (core 1) and comms/usb_owner (core 0) */
    log_write_line("capture queue full");
    return false;
}

static void count_samples_cb(const char* name, void* user)
{
    (void)user;
    if (strncmp(name, "img_", 4) == 0)
    {
        g_n_samples++;
    }
}

static void handle_capture(const capture_meta_t* in)
{
    capture_meta_t m = *in;
    m.node_id = NODE_ID;
    m.capture_id = ++g_capture_id;
    if (m.uptime_ms == 0)
    {
        m.uptime_ms = p_now(NULL);
    }
    if (!g_n_samples)
    {
        log_write_line("capture: no samples/img_XX.jpg on the SD card");
        return;
    }
    m.image_index = g_next_sample;
    char path[32];
    tm_sprintf((UB*)path, (UB*)"samples/img_%02u.jpg", (unsigned)g_next_sample);
    g_next_sample = (uint8_t)((g_next_sample + 1u) % g_n_samples);
    uint8_t mb[ALFP_MAX_META]; /* 27 meta bytes now, plus the image's file name appended below */
    capture_meta_pack(&m, mb);
    char line[LOG_LINE_MAX];

    if (APP_SPLIT_TELEMETRY)
    {
        const alfp_storage_t* st = storage_fatfs();
        char tel_path[32];
        tm_sprintf((UB*)tel_path, (UB*)TEL_DIR "/t_%lu.bin", (unsigned long)m.capture_id);
        int th = st->open_write(st->ctx, tel_path);
        bool tel_ok = th >= 0 && st->write_at(st->ctx, th, 0, mb, CAPTURE_META_LEN) == CAPTURE_META_LEN;
        if (th >= 0)
        {
            st->close(st->ctx, th);
        }
        uint8_t tel_meta[ALFP_MAX_META];
        memcpy(tel_meta, mb, CAPTURE_META_LEN);
        size_t marker_len = strlen(APP_TELEMETRY_MARKER);
        memcpy(tel_meta + CAPTURE_META_LEN, APP_TELEMETRY_MARKER, marker_len);
        uint8_t tel_meta_len = (uint8_t)(CAPTURE_META_LEN + marker_len);
        if (tel_ok && alfp_outbox_add(&g_outbox, tel_path, tel_meta, tel_meta_len))
        {
            tm_sprintf((UB*)line, (UB*)"capture %lu telemetry queued (%s)", (unsigned long)m.capture_id, tel_path);
        }
        else
        {
            tm_sprintf((UB*)line, (UB*)"capture %lu: telemetry SD write failed, image still sent",
                       (unsigned long)m.capture_id);
            st->remove(st->ctx, tel_path);
        }
        log_write_line(line);
    }

    /* Image job's meta carries the image's own file name as a tail after the 27 meta bytes (same
     * convention as firmware/app/main.c): that is what tells the backend this transfer, unlike
     * the telemetry one above, has a real image attached. */
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t nl = strlen(base);
    memcpy(mb + CAPTURE_META_LEN, base, nl);
    uint8_t ml = (uint8_t)(CAPTURE_META_LEN + nl);
    if (alfp_outbox_add(&g_outbox, path, mb, ml))
    {
        tm_sprintf((UB*)line, (UB*)"capture %lu queued (%s), outbox %u", (unsigned long)m.capture_id, path,
                   alfp_outbox_count(&g_outbox));
    }
    else
    {
        tm_sprintf((UB*)line, (UB*)"capture: SD write failed");
    }
    log_write_line(line);
}

/* ---- tasks -------------------------------------------------------------------------------- */

static void print_stats(void)
{
    char line[LOG_LINE_MAX];
    const alfp_stats_t* s = &g_alfp.stats;
    tm_sprintf((UB*)line, (UB*)"node %u | alfp in %lu badcrc %lu | tx ok %lu fail %lu | rx ok %lu crcfail %lu "
                                "expired %lu | outbox %u",
               NODE_ID, (unsigned long)s->pkts_in, (unsigned long)s->pkts_bad_crc, (unsigned long)s->tx_ok,
               (unsigned long)s->tx_failed, (unsigned long)s->rx_ok, (unsigned long)s->rx_crc_fail,
               (unsigned long)s->rx_expired, alfp_outbox_count(&g_outbox));
    log_write_line(line);
    for (int i = 0; i < 2; i++)
    {
        mesh_stats_t m;
        g_radios[i]->get_stats(&m);
        tm_sprintf((UB*)line, (UB*)"  %s: tx %lu rx %lu delivered %lu relayed %lu dup %lu rxerr %lu",
                   g_radios[i]->name, (unsigned long)m.tx, (unsigned long)m.rx, (unsigned long)m.delivered,
                   (unsigned long)m.relayed, (unsigned long)m.dup_dropped, (unsigned long)m.rx_error);
        log_write_line(line);
    }
    const sensing_env_t* e = sensing_env();
    tm_sprintf((UB*)line, (UB*)"  dht22 good %lu fail %lu | lora %s | wifi %s", (unsigned long)e->dht_good,
               (unsigned long)e->dht_fail, radios_lora_ok() ? "up" : "DOWN", radios_wifi_ok() ? "up" : "DOWN");
    log_write_line(line);
    uint32_t t;
    for (int i = 0; i < 2; i++)
    {
        if (g_radios[i]->last_heard(BRIDGE_ID, &t))
        {
            tm_sprintf((UB*)line, (UB*)"  bridge last heard on %s %lu ms ago", g_radios[i]->name,
                       (unsigned long)(p_now(NULL) - t));
        }
        else
        {
            tm_sprintf((UB*)line, (UB*)"  bridge never heard on %s", g_radios[i]->name);
        }
        log_write_line(line);
    }

    /* --- Section 7: new fields --- */
    struct
    {
        const char* name;
        rtos_id_t id;
        unsigned core;
        unsigned prio;
    } tasks[3] = {
        {"comms", g_comms_task_id, RTOS_CORE_0, RTOS_PRIO_COMMS},
        {"usb_owner", g_owner_task_id, RTOS_CORE_0, RTOS_PRIO_USB_OWNER},
        {"sensors", g_sensors_task_id, RTOS_CORE_1, RTOS_PRIO_SENSORS},
    };
    for (int i = 0; i < 3; i++)
    {
        tm_sprintf((UB*)line, (UB*)"  task %s: core %u prio %u stack free %lu B", tasks[i].name, tasks[i].core,
                   tasks[i].prio, (unsigned long)rtos_task_stack_free(tasks[i].id));
        log_write_line(line);
    }
    tm_sprintf((UB*)line, (UB*)"  longest comms loop %lu ms | capture q full %lu | log dropped %lu",
               (unsigned long)g_longest_comms_loop_ms, (unsigned long)g_capture_queue_full_count,
               (unsigned long)log_dropped_count());
    log_write_line(line);
}

static void comms_task(void* arg)
{
    (void)arg;
    char line[LOG_LINE_MAX];
    tm_sprintf((UB*)line, (UB*)"comms: core %u", rtos_core_id());
    log_write_line(line);

    /* The SD card goes first: it must be switched to SPI mode before any LoRa traffic appears on
     * the shared SPI1 bus (board.h). comms is SPI1's only caller - see board.h/6.10. */
    while (!storage_fatfs_mount())
    {
        log_write_line("sd: mount failed (card in the Maker slot? FAT32?), retrying");
        rtos_delay_ms(2000);
    }
    const alfp_storage_t* st = storage_fatfs();
    st->mkdir(st->ctx, "samples");
    if (APP_SPLIT_TELEMETRY)
    {
        st->mkdir(st->ctx, TEL_DIR);
    }
    st->list(st->ctx, "samples", count_samples_cb, NULL);
    tm_sprintf((UB*)line, (UB*)"sd: mounted, %u sample images", g_n_samples);
    log_write_line(line);

    if (!radios_init(NODE_ID))
    {
        log_write_line("radios: NEITHER radio started. Captures will wait on the SD card.");
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
    tm_sprintf((UB*)line, (UB*)"node %u ready (%s), outbox has %u queued, radio size threshold %u B", NODE_ID,
               NODE_ID == BRIDGE_ID ? "bridge" : "field node", alfp_outbox_count(&g_outbox),
               (unsigned)APP_RADIO_SIZE_THRESHOLD);
    log_write_line(line);

    rtos_flags_set(g_flags, EVT_COMMS_READY);
    bool dropping = false, topo = true;
    for (;;)
    {
        uint32_t loop_start = rtos_now_ms();
        g_radios[0]->poll();
        g_radios[1]->poll();
        alfp_service(&g_alfp, g_radios, 2);
        if (NODE_ID != BRIDGE_ID)
        {
            capture_meta_t req;
            while (rtos_msgq_recv(g_capture_q, &req, sizeof(req), RTOS_NO_WAIT))
            {
                handle_capture(&req);
            }
            alfp_outbox_pump(&g_outbox);
        }

        uint32_t bits = 0;
        if (rtos_flags_wait_any(g_flags, EVT_WANT_STATS | EVT_TOGGLE_DROP | EVT_TOGGLE_TOPO, &bits, RTOS_NO_WAIT))
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
                log_write_line(dropping ? "injected loss 30%" : "injected loss off");
            }
            if (bits & EVT_TOGGLE_TOPO)
            {
                topo = !topo;
                radios_set_wifi_topology_filter(topo);
                log_write_line(topo ? "wifi topology filter ON (demo layout, forces multi-hop)"
                                     : "wifi topology filter OFF (hear everyone)");
            }
        }

        uint32_t loop_ms = rtos_elapsed_ms(loop_start);
        if (loop_ms > g_longest_comms_loop_ms)
        {
            g_longest_comms_loop_ms = loop_ms;
        }
        /* Same reasoning as the FreeRTOS version (main.c): a fixed delay, not
         * a delay-until, so a slow iteration (SD busy, outbox pump) does not
         * try to make up lost time by starving sensors on the other core. */
        rtos_delay_ms(2);
    }
}

/* ---- sensors (Maryam + Rhaylene) ---------------------------------------------------------- */

static void rtos_wait_ms(uint32_t ms)
{
    rtos_delay_ms(ms);
}

/* DHT22.c times the sensor's bits by counting loop iterations and must not be interrupted for the
 * ~5 ms of the read. Brief 3.8 Gap 14: kept as the priority boost (not interrupt masking), just
 * translated to tk_chg_pri. */
static unsigned s_saved_prio;
static void dht_begin(void)
{
    s_saved_prio = rtos_priority_self_get();
    rtos_priority_self_set(1); /* MIN_TSKPRI: briefly the highest-priority task on core 1 */
}
static void dht_end(void)
{
    rtos_priority_self_set(s_saved_prio);
}

static void print_env(void)
{
    const sensing_env_t* e = sensing_env();
    char line[LOG_LINE_MAX];
    if (e->dht_ok)
    {
        int32_t t10 = (int32_t)(e->temp_c * 10.0f);
        int32_t h10 = (int32_t)(e->hum_pct * 10.0f);
        tm_sprintf((UB*)line, (UB*)"[ENV] Temp: %d.%uC | Hum: %u.%u%% | Light: %u | Weather: %s | PIR %d Sound %d",
                   (int)(t10 / 10), (unsigned)((t10 < 0 ? -t10 : t10) % 10), (unsigned)(h10 / 10),
                   (unsigned)(h10 % 10), e->light, sensing_weather_name(e->weather), sensing_pir(),
                   sensing_sound_trigger());
    }
    else
    {
        tm_sprintf((UB*)line, (UB*)"[ENV] DHT22: no reading (%lu fails) | Light: %u | Weather: unknown | PIR %d Sound %d",
                   (unsigned long)e->dht_fail, e->light, sensing_pir(), sensing_sound_trigger());
    }
    log_write_line(line);
}

static void sensors_task(void* arg)
{
    (void)arg;
    char line[LOG_LINE_MAX];
    tm_sprintf((UB*)line, (UB*)"sensors: core %u", rtos_core_id());
    log_write_line(line);

    const sensing_hooks_t hooks = {rtos_wait_ms, dht_begin, dht_end};
    sensing_init(&hooks);
    gpio_init(BOARD_BUTTON_GPIO);
    gpio_set_dir(BOARD_BUTTON_GPIO, GPIO_IN);
    gpio_pull_up(BOARD_BUTTON_GPIO);

    uint32_t bits;
    rtos_flags_wait_any(g_flags, EVT_COMMS_READY, &bits, RTOS_WAIT_FOREVER);
    log_write_line("sensors: PIR GP0, sound GP14, mic GP26, DHT22 GP4, light GP27, button GP20");

    bool was_down = false;
    uint32_t last_env_print = 0;
    for (;;)
    {
        uint32_t now = p_now(NULL);
        sensing_env_poll(now);
        bool want_env = false;
        if (rtos_flags_wait_any(g_flags, EVT_WANT_ENV, &bits, RTOS_NO_WAIT))
        {
            want_env = true;
        }
        if (want_env || now - last_env_print >= 10000)
        {
            last_env_print = now;
            print_env();
        }

        bool down = !gpio_get(BOARD_BUTTON_GPIO);
        bool pressed = down && !was_down;
        was_down = down;
        bool forced = pressed;
        if (rtos_flags_wait_any(g_flags, EVT_FORCE_CAPTURE, &bits, RTOS_NO_WAIT))
        {
            forced = true;
        }
        bool dual = sensing_dual_trigger(now);

        if (dual || forced)
        {
            uint8_t flags = (uint8_t)((sensing_pir() ? CM_TRIG_PIR : 0) | (sensing_sound_trigger() ? CM_TRIG_SOUND : 0));
            if (NODE_ID == BRIDGE_ID)
            {
                log_write_line("[ALERT] trigger on the bridge: the bridge only receives, no capture is sent");
            }
            tm_sprintf((UB*)line, (UB*)"[ALERT] %s (PIR %d, sound %d). Recording %us of audio...",
                       dual ? "Dual trigger fired" : "Manual capture", (flags & CM_TRIG_PIR) != 0,
                       (flags & CM_TRIG_SOUND) != 0, (unsigned)AUDIO_SECONDS);
            log_write_line(line);
            capture_meta_t m;
            audio_result_t a;
            sensing_capture(now, flags, &m, &a);
            /* rms/mean_adc are already integer ADC counts (audio_features.h); only energy/zcr are
             * float and get the x1000 fixed-point treatment %.1f/%.3f relied on in main.c. */
            tm_sprintf((UB*)line, (UB*)"[AUDIO] %s (class %d) energy %u zcr_x1000 %u rms %u mean %u, %lu samples",
                       a.label, a.class_id, (unsigned)a.energy, (unsigned)(a.zcr * 1000.0f), a.rms, a.mean_adc,
                       (unsigned long)a.samples);
            log_write_line(line);
            print_env();
            if (NODE_ID != BRIDGE_ID && !app_request_capture(&m))
            {
                log_write_line("[ALERT] capture queue full, dropped");
            }
        }
        rtos_delay_ms(10); /* their main.c polls every 10 ms; also debounces the button */
    }
}

/* ---- console + USB owner (merged, brief 3.6) ---------------------------------------------- */

static void print_help(void)
{
    log_write_line("keys: c capture now | e sensor readings | s stats | d toggle 30% loss | t toggle "
                    "WiFi topology filter | x T6: fire 5 captures fast | z T8: simulate a stalled "
                    "terminal for 5s | h help");
}

/* T6 helper (brief Section 8): app_request_capture() is a plain exported
 * function, safe to call from any task, so this fires 5 requests back to
 * back from the console task itself rather than waiting on sensors_task's
 * slower, gated trigger path - the 5th should return false. Not perfectly
 * deterministic (comms_task could drain between calls if it is scheduled
 * in between), but close enough to demonstrate the full-queue path on
 * demand per the brief's "trigger 5+ captures quickly". */
static void t6_fire_captures(void)
{
    capture_meta_t m;
    capture_meta_clear(&m);
    for (int i = 0; i < 5; i++)
    {
        bool ok = app_request_capture(&m);
        char line[LOG_LINE_MAX];
        tm_sprintf((UB*)line, (UB*)"[T6] capture request %d: %s", i + 1, ok ? "queued" : "refused");
        log_write_line(line);
    }
}

/* T8 helper: stop draining the log ring for 5 s, as if a terminal/dashboard
 * had stopped reading. comms/sensors keep running on their own loops the
 * whole time (they only ever do a non-blocking send); log_dropped_count()
 * should climb instead of anything stalling. */
static uint32_t s_stall_until_ms;

static void usb_owner_task(void* arg)
{
    (void)arg;
    char line[LOG_LINE_MAX];
    tm_sprintf((UB*)line, (UB*)"usb_owner: core %u", rtos_core_id());
    log_write_line(line);
    print_help();

    for (int i = 0; i < 200 && tm_usb_state() < 2; i++)
    {
        rtos_delay_ms(100);
    }

    for (;;)
    {
        bool stalled = s_stall_until_ms != 0 && (int32_t)(rtos_now_ms() - s_stall_until_ms) < 0;
        if (!stalled)
        {
            s_stall_until_ms = 0;
            char drained[LOG_LINE_MAX];
            while (log_drain_one(drained, RTOS_NO_WAIT))
            {
                tm_printf((UB*)"%s\n", drained);
            }
        }

        /* Round 2 review fix A2: tm_getchar(0) is dead on CONSOLE=usb_cdc
         * (see firmware_smp/buildglue/usb_tinyusb_glue_overlay.c); this
         * reads the same CDC RX bytes via the build-time overlay instead. */
        int ch = rtos_console_getchar();
        if (ch == 'c')
        {
            rtos_flags_set(g_flags, EVT_FORCE_CAPTURE);
        }
        else if (ch == 'e')
        {
            rtos_flags_set(g_flags, EVT_WANT_ENV);
        }
        else if (ch == 't')
        {
            rtos_flags_set(g_flags, EVT_TOGGLE_TOPO);
        }
        else if (ch == 'h' || ch == '?')
        {
            print_help();
        }
        else if (ch == 's')
        {
            rtos_flags_set(g_flags, EVT_WANT_STATS);
        }
        else if (ch == 'd')
        {
            rtos_flags_set(g_flags, EVT_TOGGLE_DROP);
        }
        else if (ch == 'x')
        {
            t6_fire_captures();
        }
        else if (ch == 'z')
        {
            s_stall_until_ms = rtos_now_ms() + 5000u;
        }
        /* Drain the log ring at least this often even with no console input
         * (brief 3.6: a stalled terminal must never stall comms/sensors - the
         * ring just fills and log_dropped_count() climbs instead). */
        rtos_delay_ms(20);
    }
}

void app_main_smp(void)
{
    rtos_atomic_init();
    log_init();

    /* Round 2 review fix A4: confirm both queues really hold exactly the
     * depth they are supposed to, before any task starts using either one.
     * The log ring's own selftest must run (and finish, leaving it empty)
     * before the first real log_write_line. */
    char selftest_line[LOG_LINE_MAX];
    log_selftest(selftest_line, sizeof(selftest_line));
    log_write_line(selftest_line);

    g_flags = rtos_flags_create();
    g_capture_q = rtos_msgq_create(sizeof(capture_meta_t), CAPTURE_Q_DEPTH, g_capture_q_buf);
    rtos_msgq_selftest(g_capture_q, sizeof(capture_meta_t), CAPTURE_Q_DEPTH, selftest_line, sizeof(selftest_line));
    log_write_line(selftest_line);

    g_comms_task_id =
        rtos_task_create("comms", comms_task, NULL, RTOS_PRIO_COMMS, 8192, RTOS_CORE_0);
    g_owner_task_id =
        rtos_task_create("usb_owner", usb_owner_task, NULL, RTOS_PRIO_USB_OWNER, 4096, RTOS_CORE_0);
    g_sensors_task_id =
        rtos_task_create("sensors", sensors_task, NULL, RTOS_PRIO_SENSORS, 4096, RTOS_CORE_1);
}
