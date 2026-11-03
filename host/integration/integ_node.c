/*
 * integ_node.c - one Pico's worth of code running as a laptop process:
 *   Mridula's real lora_mesh.c + fake LR1121 (UDP)          (default build)
 *   Goutham's real wifi_mesh.c + fake ESP-01 link (UDP)     (built with -DUSE_WIFI)
 *   + ALFP + a folder standing in for the SD card.
 *
 *   ./integ_node 1 <dir>                 receiver: waits for one file, exits 0 when it is verified
 *   ./integ_node N <dir> <file>          sender: sends <file> (relative to <dir>) to node 1
 *   ./integ_node N <dir> relay           relay only: runs the mesh for 120 s, or until SIGINT/SIGTERM,
 *                                        then prints its mesh stats
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alfp.h"
#ifdef USE_WIFI
#include "wifi_mesh.h"
void fake_wst_set_node(int node_id);
#define RADIO wifi_mesh
#define PROFILE {"wifi", 10, 10, 200, 15000, 40, 10, 40, 1, 4, ALFP_RADIO_WIFI} /* = firmware default */
#else
#include "lora_mesh.h"
void fake_radio_set_node(int node_id);
#define RADIO lora_mesh_radio
#define PROFILE {"lora", 150, 150, 1500, 60000, 40, 10, 30, 1, 4, ALFP_RADIO_LORA}
#endif
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "storage_posix.h"


static alfp_ctx_t ctx;
static int done, ok;
static char rx_path[200];
static volatile sig_atomic_t stop;

static void on_stop(int sig) { (void)sig; stop = 1; }

#ifdef USE_WIFI
#define FILTERED() wifi_mesh_filtered_count() /* packets from non-neighbours, ignored */
#else
#define FILTERED() 0u
#endif

static uint32_t p_now(void* c) { (void)c; return to_ms_since_boot(get_absolute_time()); }
static uint32_t p_rand(void* c) { (void)c; return get_rand_32(); }

static void on_tx(alfp_ctx_t* c, const alfp_tx_report_t* r)
{
    (void)c;
    printf("[sender] %s: %s, %lu bytes, crc %08lx, %u frags, %u rounds, %u resent, %.1f s\n", r->path,
           alfp_tx_result_str(r->result), (unsigned long)r->file_size, (unsigned long)r->file_crc32, r->frag_count,
           r->rounds, r->data_resent, (r->finished_ms - r->started_ms) / 1000.0);
    done = 1;
    ok = r->result == ALFP_TX_DONE;
}

static void on_rx(alfp_ctx_t* c, const alfp_rx_report_t* r)
{
    (void)c;
    printf("[receiver] got %s from node %u: %lu bytes, crc %08lx verified, %u dup\n", r->path, r->src,
           (unsigned long)r->file_size, (unsigned long)r->file_crc32, r->duplicates);
    snprintf(rx_path, sizeof(rx_path), "%s", r->path);
    done = 1;
    ok = 1;
}

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: integ_node id dir [file]\n"); return 2; }
    int id = atoi(argv[1]);
    setvbuf(stdout, NULL, _IOLBF, 0);
#ifdef USE_WIFI
    fake_wst_set_node(id);
#else
    fake_radio_set_node(id);
#endif
    storage_posix_t sp;
    alfp_storage_t st;
    storage_posix_init(&sp, argv[2], &st);
    alfp_platform_t plat = {NULL, p_now, p_rand, NULL};
    static alfp_profile_t prof[1] = {PROFILE};
    alfp_cfg_t cfg = {.node_id = (uint8_t)id, .bridge_id = 1, .storage = &st, .platform = &plat,
                      .on_tx_done = on_tx, .on_rx_done = on_rx, .profiles = prof, .n_profiles = 1};
#ifdef USE_WIFI
    if (!wifi_mesh_init((uint8_t)id)) return 3; /* node 1 heartbeats by itself; topology filter ON */
#else
    if (!lora_mesh_init((uint8_t)id)) return 3;
    if (id == 1) lora_mesh_set_heartbeat(3000);
#endif
    alfp_init(&ctx, &cfg);
    const mesh_radio_t* radios[1] = {&RADIO};
    uint32_t t0 = p_now(NULL);
    if (argc > 3 && strcmp(argv[3], "relay") == 0)
    {
        signal(SIGINT, on_stop);
        signal(SIGTERM, on_stop);
        while (!stop && p_now(NULL) - t0 < 120000) { RADIO.poll(); alfp_service(&ctx, radios, 1); sleep_ms(1); }
        mesh_stats_t m;
        RADIO.get_stats(&m);
        printf("[relay %d] mesh stats: tx %u rx %u delivered %u relayed %u dup %u filtered %u\n", id, m.tx, m.rx,
               m.delivered, m.relayed, m.dup_dropped, (unsigned)FILTERED());
        return 0;
    }
    if (id != 1)
    {
        sleep_ms(500);
        if (alfp_send_file(&ctx, &RADIO, 1, argv[3], NULL, 0) != ALFP_OK) { printf("send failed\n"); return 4; }
    }
    while (!done && p_now(NULL) - t0 < 300000)
    {
        RADIO.poll();
        alfp_service(&ctx, radios, 1);
        sleep_ms(1);
    }
    /* receiver: keep answering for a few seconds so a lost ACK can be repeated */
    uint32_t t1 = p_now(NULL);
    while (id == 1 && p_now(NULL) - t1 < 6000) { RADIO.poll(); alfp_service(&ctx, radios, 1); sleep_ms(1); }
    mesh_stats_t m;
    RADIO.get_stats(&m);
    printf("[node %d] mesh stats: tx %u rx %u delivered %u relayed %u dup %u rx_error %u filtered %u\n", id, m.tx, m.rx,
           m.delivered, m.relayed, m.dup_dropped, m.rx_error, (unsigned)FILTERED());
    if (id == 1 && ok) printf("RX_FILE=%s\n", rx_path);
    return ok ? 0 : 1;
}
