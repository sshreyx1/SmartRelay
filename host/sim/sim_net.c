/*
 * sim_net.c - see sim_net.h.
 */
#include "sim_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static net_t* gp_net; /* callbacks need to find their node */
static const alfp_profile_t* gp_prof;
static uint8_t g_nprof;
static bool gb_baseline;

void net_set_options(const alfp_profile_t* p_profiles, uint8_t n_profiles, bool b_baseline_resend_all)
{
    gp_prof = p_profiles;
    g_nprof = n_profiles;
    gb_baseline = b_baseline_resend_all;
}

static uint32_t p_now(void* p_ctx)
{
    (void)p_ctx;
    return sim_now();
}

static uint32_t p_rand(void* p_ctx)
{
    (void)p_ctx;
    return sim_rand();
}

static unsigned p_rand_u(void)
{
    return sim_rand();
}

static net_node_t* node_of(alfp_ctx_t* p_ctx)
{
    return (net_node_t*)p_ctx->cfg.p_user;
}

static void on_tx(alfp_ctx_t* p_ctx, const alfp_tx_report_t* p_rep)
{
    net_node_t* p_node_end = node_of(p_ctx);
    if (p_node_end->n_tx < NET_MAX_REPORTS)
    {
        p_node_end->tx_reports[p_node_end->n_tx++] = *p_rep;
    }
    if (p_node_end->b_use_outbox)
    {
        alfp_outbox_on_tx_done(&p_node_end->outbox, p_rep);
    }
}

static void on_rx(alfp_ctx_t* p_ctx, const alfp_rx_report_t* p_rep)
{
    net_node_t* p_node_end = node_of(p_ctx);
    if (p_node_end->n_rx < NET_MAX_REPORTS)
    {
        p_node_end->rx_reports[p_node_end->n_rx] = *p_rep;
        snprintf(p_node_end->rx_paths[p_node_end->n_rx], sizeof(p_node_end->rx_paths[0]), "%s", p_rep->p_path);
        p_node_end->rx_reports[p_node_end->n_rx].p_path = p_node_end->rx_paths[p_node_end->n_rx];
        p_node_end->rx_reports[p_node_end->n_rx].p_meta = NULL; /* points into the session; not kept */
        p_node_end->n_rx++;
    }
}

void net_init(net_t* p_net, const sim_cfg_t* p_cfg, const char* p_root, int radios_mask, bool b_use_outbox)
{
    memset(p_net, 0, sizeof(*p_net));
    gp_net = p_net;
    p_net->cfg = *p_cfg;
    snprintf(p_net->root, sizeof(p_net->root), "%s", p_root);
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", p_root);
    if (0 != system(cmd))
    {
        fprintf(stderr, "could not clear %s\n", p_root);
    }
    sim_init(p_cfg);
    for (uint8_t idx = 1; idx <= p_cfg->n_nodes; idx++)
    {
        net_node_t* p_node_end = &p_net->node[idx];
        char dir[300];
        snprintf(dir, sizeof(dir), "%s/node%u", p_root, (unsigned)idx);
        storage_posix_init(&p_node_end->posix, dir, &p_node_end->store);
        p_node_end->posix.p_rand_fn = p_rand_u;
        p_node_end->plat = (alfp_platform_t){NULL, p_now, p_rand, NULL};
        p_node_end->n_radios = 0;
        if (radios_mask & 2)
        {
            p_node_end->radios[p_node_end->n_radios++] = &g_sim_wifi;
        }
        if (radios_mask & 1)
        {
            p_node_end->radios[p_node_end->n_radios++] = &g_sim_lora;
        }
        alfp_cfg_t cfg_a = {
            .node_id = idx,
            .bridge_id = p_cfg->bridge_id,
            .p_storage = &p_node_end->store,
            .p_platform = &p_node_end->plat,
            .p_inbox_dir = "inbox",
            .p_tmp_dir = "rx",
            .p_on_tx_done = on_tx,
            .p_on_rx_done = on_rx,
            .p_user = p_node_end,
            .p_profiles = gp_prof,
            .n_profiles = g_nprof,
            .b_baseline_resend_all = gb_baseline,
        };
        sim_set_current(idx);
        alfp_init(&p_node_end->alfp, &cfg_a);
        p_node_end->b_use_outbox = b_use_outbox;
        if (b_use_outbox)
        {
            alfp_outbox_init(&p_node_end->outbox, &p_node_end->alfp, p_node_end->radios, p_node_end->n_radios, p_cfg->bridge_id, 10000, "outbox");
        }
    }
}

static void one_ms(net_t* p_net)
{
    sim_tick();
    for (uint8_t idx = 1; idx <= p_net->cfg.n_nodes; idx++)
    {
        net_node_t* p_node_end = &p_net->node[idx];
        sim_set_current(idx);
        g_sim_lora.p_poll();
        g_sim_wifi.p_poll();
        alfp_service(&p_node_end->alfp, p_node_end->radios, p_node_end->n_radios);
        if (p_node_end->b_use_outbox)
        {
            alfp_outbox_pump(&p_node_end->outbox);
        }
    }
}

void net_run_ms(net_t* p_net, uint32_t msec)
{
    for (uint32_t tick = 0; tick < msec; tick++)
    {
        one_ms(p_net);
    }
}

bool net_run_until(net_t* p_net, bool (*p_pred)(net_t*), uint32_t max_ms)
{
    for (uint32_t tick = 0; tick < max_ms; tick++)
    {
        one_ms(p_net);
        if (p_pred(p_net))
        {
            return true;
        }
    }
    return false;
}

void net_close(net_t* p_net)
{
    for (uint8_t idx = 1; idx <= p_net->cfg.n_nodes; idx++)
    {
        storage_posix_close_all(&p_net->node[idx].posix);
    }
}

void net_make_file(net_t* p_net, uint8_t node, const char* p_path, uint32_t size, uint32_t seed)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/node%u/%s", p_net->root, (unsigned)node, p_path);
    /* make parent dir */
    char dir[600];
    snprintf(dir, sizeof(dir), "%s", path);
    char* p_slash = strrchr(dir, '/');
    if (p_slash)
    {
        *p_slash = 0;
        char cmd[700];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
        if (0 != system(cmd))
        {
            fprintf(stderr, "mkdir failed\n");
        }
    }
    FILE* p_file = fopen(path, "wb");
    uint32_t value_x = seed * 2654435761u + 12345u;
    for (uint32_t idx = 0; idx < size; idx++)
    {
        value_x ^= value_x << 13;
        value_x ^= value_x >> 17;
        value_x ^= value_x << 5;
        fputc((int)(value_x & 0xFF), p_file);
    }
    fclose(p_file);
}

bool net_files_equal(net_t* p_net, uint8_t node_a, const char* p_path_a, uint8_t node_b, const char* p_path_b)
{
    char path_one[600], path_two[600];
    snprintf(path_one, sizeof(path_one), "%s/node%u/%s", p_net->root, (unsigned)node_a, p_path_a);
    snprintf(path_two, sizeof(path_two), "%s/node%u/%s", p_net->root, (unsigned)node_b, p_path_b);
    FILE* p_file_a = fopen(path_one, "rb");
    FILE* p_file_b = fopen(path_two, "rb");
    bool b_equal = p_file_a && p_file_b;
    while (b_equal)
    {
        int chr_a = fgetc(p_file_a), chr_b = fgetc(p_file_b);
        if (chr_a != chr_b)
        {
            b_equal = false;
        }
        if ((EOF == chr_a) || (EOF == chr_b))
        {
            break;
        }
    }
    if (p_file_a)
    {
        fclose(p_file_a);
    }
    if (p_file_b)
    {
        fclose(p_file_b);
    }
    return b_equal;
}
