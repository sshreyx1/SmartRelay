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
#define RADIO g_wifi_mesh
#define PROFILE {"wifi", 10, 10, 200, 15000, 40, 10, 40, 1, 4, ALFP_RADIO_WIFI} /* = firmware default */
#else
#include "lora_mesh.h"
void fake_radio_set_node(int node_id);
#define RADIO g_lora_mesh_radio
#define PROFILE {"lora", 150, 150, 1500, 60000, 40, 10, 30, 1, 4, ALFP_RADIO_LORA}
#endif
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "storage_posix.h"


static alfp_ctx_t ctx;
static int done;
static int ok_flag;
static char rx_path[200];
static volatile sig_atomic_t stop;

static void on_stop(int sig) { (void)sig; stop = 1; }

#ifdef USE_WIFI
#define FILTERED() wifi_mesh_filtered_count() /* packets from non-neighbours, ignored */
#else
#define FILTERED() 0u
#endif

static uint32_t p_now(void* p_ctx) { (void)p_ctx; return to_ms_since_boot(get_absolute_time()); }
static uint32_t p_rand(void* p_ctx) { (void)p_ctx; return get_rand_32(); }

static void on_tx(alfp_ctx_t* p_ctx, const alfp_tx_report_t* p_rep)
{
    (void)p_ctx;
    printf("[sender] %s: %s, %lu bytes, crc %08lx, %u frags, %u rounds, %u resent, %.1f s\n", p_rep->p_path,
           alfp_tx_result_str(p_rep->result), (unsigned long)p_rep->file_size, (unsigned long)p_rep->file_crc32, p_rep->frag_count,
           p_rep->rounds, p_rep->data_resent, (p_rep->finished_ms - p_rep->started_ms) / 1000.0);
    done = 1;
    ok_flag = ALFP_TX_DONE == p_rep->result;
}

static void on_rx(alfp_ctx_t* p_ctx, const alfp_rx_report_t* p_rep)
{
    (void)p_ctx;
    printf("[receiver] got %s from node %u: %lu bytes, crc %08lx verified, %u dup\n", p_rep->p_path, p_rep->src,
           (unsigned long)p_rep->file_size, (unsigned long)p_rep->file_crc32, p_rep->duplicates);
    snprintf(rx_path, sizeof(rx_path), "%s", p_rep->p_path);
    done = 1;
    ok_flag = 1;
}

int main(int argc, char** pp_argv)
{
    if (argc < 3) { fprintf(stderr, "usage: integ_node id dir [file]\n"); return 2; }
    int node_num = atoi(pp_argv[1]);
    setvbuf(stdout, NULL, _IOLBF, 0);
#ifdef USE_WIFI
    fake_wst_set_node(node_num);
#else
    fake_radio_set_node(node_num);
#endif
    storage_posix_t posix;
    alfp_storage_t store;
    storage_posix_init(&posix, pp_argv[2], &store);
    alfp_platform_t plat = {NULL, p_now, p_rand, NULL};
    static alfp_profile_t prof[1] = {PROFILE};
    alfp_cfg_t cfg = {.node_id = (uint8_t)node_num, .bridge_id = 1, .p_storage = &store, .p_platform = &plat,
                      .p_on_tx_done = on_tx, .p_on_rx_done = on_rx, .p_profiles = prof, .n_profiles = 1};
#ifdef USE_WIFI
    if (!wifi_mesh_init((uint8_t)node_num)) return 3; /* node 1 heartbeats by itself; topology filter ON */
#else
    if (!lora_mesh_init((uint8_t)node_num)) return 3;
    if (1 == node_num) lora_mesh_set_heartbeat(3000);
#endif
    alfp_init(&ctx, &cfg);
    const mesh_radio_t* radios[1] = {&RADIO};
    uint32_t start_t = p_now(NULL);
    if ((argc > 3) && (0 == strcmp(pp_argv[3], "relay")))
    {
        signal(SIGINT, on_stop);
        signal(SIGTERM, on_stop);
        while ((!stop) && (p_now(NULL) - start_t < 120000)) { RADIO.p_poll(); alfp_service(&ctx, radios, 1); sleep_ms(1); }
        mesh_stats_t stats;
        RADIO.p_get_stats(&stats);
        printf("[relay %d] mesh stats: tx %u rx %u delivered %u relayed %u dup %u filtered %u\n", node_num, stats.tx_count, stats.rx_count,
               stats.delivered, stats.relayed, stats.dup_dropped, (unsigned)FILTERED());
        return 0;
    }
    if (1 != node_num)
    {
        sleep_ms(500);
        if (ALFP_OK != alfp_send_file(&ctx, &RADIO, 1, pp_argv[3], NULL, 0)) { printf("send failed\n"); return 4; }
    }
    while ((!done) && (p_now(NULL) - start_t < 300000))
    {
        RADIO.p_poll();
        alfp_service(&ctx, radios, 1);
        sleep_ms(1);
    }
    /* receiver: keep answering for a few seconds so a lost ACK can be repeated */
    uint32_t mid_t = p_now(NULL);
    while ((1 == node_num) && (p_now(NULL) - mid_t < 6000)) { RADIO.p_poll(); alfp_service(&ctx, radios, 1); sleep_ms(1); }
    mesh_stats_t stats;
    RADIO.p_get_stats(&stats);
    printf("[node %d] mesh stats: tx %u rx %u delivered %u relayed %u dup %u rx_error %u filtered %u\n", node_num, stats.tx_count, stats.rx_count,
           stats.delivered, stats.relayed, stats.dup_dropped, stats.rx_error, (unsigned)FILTERED());
    if ((1 == node_num) && ok_flag) printf("RX_FILE=%s\n", rx_path);
    return ok_flag ? 0 : 1;
}
