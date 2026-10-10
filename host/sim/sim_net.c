/*
 * sim_net.c - see sim_net.h.
 */
#include "sim_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static net_t* g_net; /* callbacks need to find their node */
static const alfp_profile_t* g_prof;
static uint8_t g_nprof;
static bool g_baseline;

void net_set_options(const alfp_profile_t* profiles, uint8_t n_profiles, bool baseline_resend_all)
{
    g_prof = profiles;
    g_nprof = n_profiles;
    g_baseline = baseline_resend_all;
}

static uint32_t p_now(void* c)
{
    (void)c;
    return sim_now();
}

static uint32_t p_rand(void* c)
{
    (void)c;
    return sim_rand();
}

static unsigned p_rand_u(void)
{
    return sim_rand();
}

static net_node_t* node_of(alfp_ctx_t* ctx)
{
    return (net_node_t*)ctx->cfg.user;
}

static void on_tx(alfp_ctx_t* ctx, const alfp_tx_report_t* r)
{
    net_node_t* nd = node_of(ctx);
    if (nd->n_tx < NET_MAX_REPORTS)
    {
        nd->tx_reports[nd->n_tx++] = *r;
    }
    if (nd->use_outbox)
    {
        alfp_outbox_on_tx_done(&nd->outbox, r);
    }
}

static void on_rx(alfp_ctx_t* ctx, const alfp_rx_report_t* r)
{
    net_node_t* nd = node_of(ctx);
    if (nd->n_rx < NET_MAX_REPORTS)
    {
        nd->rx_reports[nd->n_rx] = *r;
        snprintf(nd->rx_paths[nd->n_rx], sizeof(nd->rx_paths[0]), "%s", r->path);
        nd->rx_reports[nd->n_rx].path = nd->rx_paths[nd->n_rx];
        nd->rx_reports[nd->n_rx].meta = NULL; /* points into the session; not kept */
        nd->n_rx++;
    }
}

void net_init(net_t* n, const sim_cfg_t* cfg, const char* root, int radios_mask, bool use_outbox)
{
    memset(n, 0, sizeof(*n));
    g_net = n;
    n->cfg = *cfg;
    snprintf(n->root, sizeof(n->root), "%s", root);
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0)
    {
        fprintf(stderr, "could not clear %s\n", root);
    }
    sim_init(cfg);
    for (uint8_t i = 1; i <= cfg->n_nodes; i++)
    {
        net_node_t* nd = &n->node[i];
        char dir[300];
        snprintf(dir, sizeof(dir), "%s/node%u", root, (unsigned)i);
        storage_posix_init(&nd->sp, dir, &nd->st);
        nd->sp.rand_fn = p_rand_u;
        nd->plat = (alfp_platform_t){NULL, p_now, p_rand, NULL};
        nd->n_radios = 0;
        if (radios_mask & 2)
        {
            nd->radios[nd->n_radios++] = &sim_wifi;
        }
        if (radios_mask & 1)
        {
            nd->radios[nd->n_radios++] = &sim_lora;
        }
        alfp_cfg_t ac = {
            .node_id = i,
            .bridge_id = cfg->bridge_id,
            .storage = &nd->st,
            .platform = &nd->plat,
            .inbox_dir = "inbox",
            .tmp_dir = "rx",
            .on_tx_done = on_tx,
            .on_rx_done = on_rx,
            .user = nd,
            .profiles = g_prof,
            .n_profiles = g_nprof,
            .baseline_resend_all = g_baseline,
        };
        sim_set_current(i);
        alfp_init(&nd->alfp, &ac);
        nd->use_outbox = use_outbox;
        if (use_outbox)
        {
            alfp_outbox_init(&nd->outbox, &nd->alfp, nd->radios, nd->n_radios, cfg->bridge_id, 10000, "outbox");
        }
    }
}

static void one_ms(net_t* n)
{
    sim_tick();
    for (uint8_t i = 1; i <= n->cfg.n_nodes; i++)
    {
        net_node_t* nd = &n->node[i];
        sim_set_current(i);
        sim_lora.poll();
        sim_wifi.poll();
        alfp_service(&nd->alfp, nd->radios, nd->n_radios);
        if (nd->use_outbox)
        {
            alfp_outbox_pump(&nd->outbox);
        }
    }
}

void net_run_ms(net_t* n, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t++)
    {
        one_ms(n);
    }
}

bool net_run_until(net_t* n, bool (*pred)(net_t*), uint32_t max_ms)
{
    for (uint32_t t = 0; t < max_ms; t++)
    {
        one_ms(n);
        if (pred(n))
        {
            return true;
        }
    }
    return false;
}

void net_close(net_t* n)
{
    for (uint8_t i = 1; i <= n->cfg.n_nodes; i++)
    {
        storage_posix_close_all(&n->node[i].sp);
    }
}

void net_make_file(net_t* n, uint8_t node, const char* path, uint32_t size, uint32_t seed)
{
    char p[600];
    snprintf(p, sizeof(p), "%s/node%u/%s", n->root, (unsigned)node, path);
    /* make parent dir */
    char dir[600];
    snprintf(dir, sizeof(dir), "%s", p);
    char* slash = strrchr(dir, '/');
    if (slash)
    {
        *slash = 0;
        char cmd[700];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
        if (system(cmd) != 0)
        {
            fprintf(stderr, "mkdir failed\n");
        }
    }
    FILE* f = fopen(p, "wb");
    uint32_t x = seed * 2654435761u + 12345u;
    for (uint32_t i = 0; i < size; i++)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        fputc((int)(x & 0xFF), f);
    }
    fclose(f);
}

bool net_files_equal(net_t* n, uint8_t a, const char* pa, uint8_t b, const char* pb)
{
    char p1[600], p2[600];
    snprintf(p1, sizeof(p1), "%s/node%u/%s", n->root, (unsigned)a, pa);
    snprintf(p2, sizeof(p2), "%s/node%u/%s", n->root, (unsigned)b, pb);
    FILE* f1 = fopen(p1, "rb");
    FILE* f2 = fopen(p2, "rb");
    bool eq = f1 && f2;
    while (eq)
    {
        int c1 = fgetc(f1), c2 = fgetc(f2);
        if (c1 != c2)
        {
            eq = false;
        }
        if (c1 == EOF || c2 == EOF)
        {
            break;
        }
    }
    if (f1)
    {
        fclose(f1);
    }
    if (f2)
    {
        fclose(f2);
    }
    return eq;
}
