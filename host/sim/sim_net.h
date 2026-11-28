/*
 * sim_net.h - runs several complete nodes (ALFP + outbox + storage) on the
 * simulated meshes.
 * Each node gets its own directory standing in for its SD card: <root>/node<N>/
 */
#ifndef SIM_NET_H
#define SIM_NET_H

#include "alfp.h"
#include "alfp_outbox.h"
#include "sim_mesh.h"
#include "storage_posix.h"

#define NET_MAX_REPORTS 64

typedef struct
{
    alfp_ctx_t          alfp;
    alfp_outbox_t       outbox;
    storage_posix_t     posix;
    alfp_storage_t      store;
    alfp_platform_t     plat;
    const mesh_radio_t* radios[2]; /* preference: wifi, lora (or just one) */
    uint8_t             n_radios;
    bool                b_use_outbox;
    alfp_tx_report_t    tx_reports[NET_MAX_REPORTS];
    unsigned            n_tx;
    alfp_rx_report_t    rx_reports[NET_MAX_REPORTS];
    char                rx_paths[NET_MAX_REPORTS][ALFP_PATH_MAX + 8];
    unsigned            n_rx;
} net_node_t;

typedef struct
{
    sim_cfg_t  cfg;
    net_node_t node[SIM_MAX_NODES + 1];
    char       root[256];
} net_t;

/* Optional, call before net_init: timing overrides and the naive baseline
 * switch for every node. */
void net_set_options(const alfp_profile_t* p_profiles, uint8_t n_profiles,
                     bool b_baseline_resend_all);

/* radios_mask: 1 = LoRa only, 2 = WiFi only, 3 = both (WiFi preferred). */
void net_init(net_t* p_net, const sim_cfg_t* p_cfg, const char* p_root,
              int radios_mask, bool b_use_outbox);
void net_run_ms(net_t* p_net, uint32_t msec);
/* Runs until pred(n) is true or max_ms passes. Returns true if pred became
 * true. */
bool net_run_until(net_t* p_net, bool (*p_pred)(net_t*), uint32_t max_ms);
void net_close(net_t* p_net);

/* Writes a deterministic pseudo-random test file of size bytes into node's
 * storage. */
void net_make_file(net_t* p_net, uint8_t node, const char* p_path,
                   uint32_t size, uint32_t seed);
/* Compares a file on node a with a file on node b. */
bool net_files_equal(net_t* p_net, uint8_t node_a, const char* p_path_a,
                     uint8_t node_b, const char* p_path_b);

#endif

/*** end of file ***/

