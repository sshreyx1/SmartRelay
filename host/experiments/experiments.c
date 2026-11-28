/*
 * experiments.c - measure ALFP on the simulated meshes.
 *
 *   ./experiments one  [key=value ...]   one configuration, many trials, prints a summary
 *   ./experiments suite <outdir>          every experiment in the report, writes CSV files
 *
 * Keys for "one": radio=lora|wifi hops=2 loss=0.1 gap=500 pollto=3000 polls=5 copies=1
 *                 cs=0 (LoRa listen-before-talk) trials=30 size=8000 baseline=0 hb=3000 seed=1
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_net.h"

static net_t net;

typedef struct
{
    int radio; /* SIM_LORA / SIM_WIFI */
    int hops;
    float loss;
    uint32_t gap, gapmax, pollto;
    uint8_t polls, copies, rounds, stalled, taildup;
    int chunk_size;
    int trials;
    uint32_t size;
    int baseline;
    uint32_t heartbeat;
    uint32_t seed;
} params_t;

typedef struct
{
    int trials, ok_flag;
    double t_sum, t_sq, rounds_sum, sent_sum, frag_sum, air_sum;
    double t_ok[512];
    int n_ok;
    int why[4];
    int mismatched; /* DONE reported but the bridge's file differs: must stay 0 */
} result_t;

static params_t defaults(void)
{
    /* defaults = the shipped LoRa profile (alfp_profiles.c) */
    params_t params = {SIM_LORA, 2, 0.1f, 500, 500, 1500, 30, 1, 40, 10, 4, 0, 30, 8000, 0, 3000, 1};
    return params;
}

static void parse_kv(params_t* p_params, const char* p_key_value)
{
    char kdx[32];
    char val_str[32];
    if (sscanf(p_key_value, "%31[^=]=%31s", kdx, val_str) != 2)
    {
        return;
    }
    if (!strcmp(kdx, "radio"))
        p_params->radio = !strcmp(val_str, "wifi") ? SIM_WIFI : SIM_LORA;
    else if (!strcmp(kdx, "hops"))
        p_params->hops = atoi(val_str);
    else if (!strcmp(kdx, "loss"))
        p_params->loss = (float)atof(val_str);
    else if (!strcmp(kdx, "gap"))
        p_params->gap = (uint32_t)atoi(val_str);
    else if (!strcmp(kdx, "gapmax"))
        p_params->gapmax = (uint32_t)atoi(val_str);
    else if (!strcmp(kdx, "pollto"))
        p_params->pollto = (uint32_t)atoi(val_str);
    else if (!strcmp(kdx, "polls"))
        p_params->polls = (uint8_t)atoi(val_str);
    else if (!strcmp(kdx, "copies"))
        p_params->copies = (uint8_t)atoi(val_str);
    else if (!strcmp(kdx, "rounds"))
        p_params->rounds = (uint8_t)atoi(val_str);
    else if (!strcmp(kdx, "taildup"))
        p_params->taildup = (uint8_t)atoi(val_str);
    else if (!strcmp(kdx, "stalled"))
        p_params->stalled = (uint8_t)atoi(val_str);
    else if (!strcmp(kdx, "cs"))
        p_params->chunk_size = atoi(val_str);
    else if (!strcmp(kdx, "trials"))
        p_params->trials = atoi(val_str);
    else if (!strcmp(kdx, "size"))
        p_params->size = (uint32_t)atoi(val_str);
    else if (!strcmp(kdx, "baseline"))
        p_params->baseline = atoi(val_str);
    else if (!strcmp(kdx, "hb"))
        p_params->heartbeat = (uint32_t)atoi(val_str);
    else if (!strcmp(kdx, "seed"))
        p_params->seed = (uint32_t)atoi(val_str);
}

static bool sender_done(net_t* p_net)
{
    return p_net->node[p_net->cfg.n_nodes].n_tx >= 1;
}

static int cmp_d(const void* p_lhs, const void* p_rhs)
{
    double value_x = *(const double*)p_lhs, value_y = *(const double*)p_rhs;
    return (value_x > value_y) - (value_x < value_y);
}

static result_t run(const params_t* p_params)
{
    result_t res;
    memset(&res, 0, sizeof(res));
    alfp_profile_t prof[2] = {
        {"lora", 500, 1500, 1500, 60000, 40, 10, 30, 1, 4, ALFP_RADIO_LORA},
        {"wifi", 10, 80, 200, 15000, 40, 10, 40, 1, 4, ALFP_RADIO_WIFI},
    };
    alfp_profile_t* p_profile = &prof[p_params->radio == SIM_WIFI ? 1 : 0];
    p_profile->gap_ms = p_params->gap;
    p_profile->gap_max_ms = p_params->gapmax;
    p_profile->poll_timeout_ms = p_params->pollto;
    p_profile->max_polls = p_params->polls;
    p_profile->poll_copies = p_params->copies;
    p_profile->max_rounds = p_params->rounds;
    p_profile->max_stalled = p_params->stalled;
    p_profile->tail_dup = p_params->taildup;
    net_set_options(prof, 2, p_params->baseline != 0);

    for (int trial = 0; trial < p_params->trials; trial++)
    {
        sim_cfg_t cfg;
        uint8_t nodes = (uint8_t)(p_params->hops + 1);
        sim_default_cfg(&cfg, nodes);
        cfg.seed = p_params->seed * 1000u + (uint32_t)trial;
        cfg.heartbeat_ms = p_params->heartbeat;
        sim_topology_chain(&cfg, -1); /* sender = last node, bridge = node 1 */
        cfg.kind[p_params->radio].link_loss = p_params->loss;
        if (p_params->radio == SIM_LORA)
        {
            cfg.kind[SIM_LORA].b_carrier_sense = p_params->chunk_size != 0;
        }
        net_init(&net, &cfg, "sim_out/exp", p_params->radio == SIM_WIFI ? 2 : 1, false);
        net_run_ms(&net, 300 + (sim_rand() % 3000)); /* random phase against the heartbeat */
        net_make_file(&net, nodes, "img.jpg", p_params->size, cfg.seed);
        sim_set_current(nodes);
        const mesh_radio_t* p_radio = p_params->radio == SIM_WIFI ? &g_sim_wifi : &g_sim_lora;
        if (alfp_send_file(&net.node[nodes].alfp, p_radio, 1, "img.jpg", NULL, 0) != ALFP_OK)
        {
            fprintf(stderr, "send_file failed\n");
            /* DEVIATION (BARR-C 8.5.b, exit()): laptop-only experiment tool, never linked into
             * firmware; a failed setup must stop the run rather than write bogus CSV rows.
             * Approved by: ____________________ (project manager), date: __________ */
            exit(1);
        }
        uint32_t air0 = sim_airtime_used(p_params->radio);
        net_run_until(&net, sender_done, 3600000);
        res.trials++;
        alfp_tx_report_t* p_rep = &net.node[nodes].tx_reports[0];
        if (net.node[nodes].n_tx && p_rep->result == ALFP_TX_GAVE_UP && p_rep->why < 4)
        {
            res.why[p_rep->why]++;
        }
        if (net.node[nodes].n_tx && p_rep->result == ALFP_TX_DONE)
        {
            if (net.node[1].n_rx < 1 || !net_files_equal(&net, nodes, "img.jpg", 1, net.node[1].rx_paths[0]))
            {
                res.mismatched++;
            }
            double secs = (p_rep->finished_ms - p_rep->started_ms) / 1000.0;
            res.ok_flag++;
            res.t_sum += secs;
            res.t_sq += secs * secs;
            res.rounds_sum += p_rep->rounds;
            res.sent_sum += p_rep->data_sent;
            res.frag_sum += p_rep->frag_count;
            res.air_sum += (sim_airtime_used(p_params->radio) - air0) / 1000.0;
            if (res.n_ok < 512)
            {
                res.t_ok[res.n_ok++] = secs;
            }
        }
        net_close(&net);
    }
    if (res.mismatched)
    {
        fprintf(stderr, "ERROR: %d transfers reported done with a wrong file\n", res.mismatched);
    }
    return res;
}

static double mean(double total, int count)
{
    return count ? total / count : NAN;
}

static double median(result_t* p_res)
{
    if (!p_res->n_ok)
    {
        return NAN;
    }
    qsort(p_res->t_ok, (size_t)p_res->n_ok, sizeof(double), cmp_d);
    return p_res->n_ok % 2 ? p_res->t_ok[p_res->n_ok / 2] : (p_res->t_ok[p_res->n_ok / 2 - 1] + p_res->t_ok[p_res->n_ok / 2]) / 2;
}

static double pct(result_t* p_res, double queue)
{
    if (!p_res->n_ok)
    {
        return NAN;
    }
    median(p_res); /* sorts */
    int idx = (int)floor(queue * (p_res->n_ok - 1) + 0.5);
    return p_res->t_ok[idx];
}

static void print_one(const params_t* p_params, result_t* p_res)
{
    printf("%s hops=%d loss=%.2f gap=%u..%u pollto=%u polls=%u copies=%u taildup=%u cs=%d base=%d size=%u | ok %d/%d (%.0f%%) "
           "t_med=%.1fs rounds=%.2f sent/frag=%.2f air=%.1fs fail[polls=%d stall=%d cap=%d] bad_files=%d\n",
           p_params->radio == SIM_WIFI ? "wifi" : "lora", p_params->hops, p_params->loss, p_params->gap, p_params->gapmax, p_params->pollto, p_params->polls, p_params->copies, p_params->taildup, p_params->chunk_size,
           p_params->baseline, p_params->size, p_res->ok_flag, p_res->trials, 100.0 * p_res->ok_flag / p_res->trials, median(p_res),
           mean(p_res->rounds_sum, p_res->ok_flag), p_res->frag_sum ? p_res->sent_sum / p_res->frag_sum : NAN, mean(p_res->air_sum, p_res->ok_flag),
           p_res->why[1], p_res->why[2], p_res->why[3], p_res->mismatched);
}

/* ---- suite: every experiment used in the report ------------------------------------------- */

static FILE* csv(const char* p_dir, const char* p_name, const char* p_header)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", p_dir, p_name);
    FILE* p_file = fopen(path, "w");
    if (!p_file)
    {
        perror(path);
        /* DEVIATION (BARR-C 8.5.b, exit()): laptop-only tool, see send_file failure above.
         * Approved by: ____________________ (project manager), date: __________ */
        exit(1);
    }
    fprintf(p_file, "%s\n", p_header);
    return p_file;
}

static void row(FILE* p_file, const char* p_label, double value_x, const params_t* p_params, result_t* p_res)
{
    fprintf(p_file, "%s,%g,%d,%d,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", p_label, value_x, p_res->ok_flag, p_res->trials,
            (double)p_res->ok_flag / p_res->trials, median(p_res), pct(p_res, 0.1), pct(p_res, 0.9), mean(p_res->rounds_sum, p_res->ok_flag),
            p_res->frag_sum ? p_res->sent_sum / p_res->frag_sum : NAN, mean(p_res->air_sum, p_res->ok_flag));
    fflush(p_file);
    print_one(p_params, p_res);
}

#define HDR "series,x,ok,trials,success,t_median_s,t_p10_s,t_p90_s,rounds_mean,sent_per_fragment,airtime_s"

static params_t tuned(int radio)
{
    params_t params = defaults();
    params.radio = radio;
    if (radio == SIM_WIFI)
    {
        params.gap = 10;
        params.gapmax = 10;
        params.pollto = 200;
        params.polls = 40;
    }
    return params;
}

static void suite(const char* p_dir, int trials)
{
    const float loss_axis[] = {0.0f, 0.05f, 0.1f, 0.15f, 0.2f, 0.25f, 0.3f};
    const unsigned name_len = sizeof(loss_axis) / sizeof(loss_axis[0]);

    /* E1 LoRa pacing gap vs hops (half-duplex relays and hidden terminals), 10% loss per hop */
    FILE* p_file = csv(p_dir, "e1_lora_gap.csv", HDR);
    const uint32_t gaps[] = {0, 250, 500, 700, 1000, 1300};
    for (int hops = 1; hops <= 4; hops++)
    {
        for (unsigned idx = 0; idx < sizeof(gaps) / sizeof(gaps[0]); idx++)
        {
            params_t params = tuned(SIM_LORA);
            params.trials = trials;
            params.hops = hops;
            params.gap = params.gapmax = gaps[idx];
            char label[32];
            snprintf(label, sizeof(label), "lora_%dhop", hops);
            result_t res = run(&params);
            row(p_file, label, gaps[idx], &params, &res);
        }
    }
    fclose(p_file);

    /* E2 first design vs final design (control path), LoRa 2 hops */
    p_file = csv(p_dir, "e2_design_iterations.csv", HDR);
    for (int value = 0; value < 3; value++)
    {
        for (unsigned idx = 0; idx < name_len; idx++)
        {
            params_t params = tuned(SIM_LORA);
            params.trials = trials;
            params.loss = loss_axis[idx];
            const char* p_label = "final";
            if (value == 0) /* first design: 3 s poll timeout, give up after 5 silent polls, 10 rounds, no tail dup */
            {
                params.pollto = 3000;
                params.polls = 5;
                params.rounds = 10;
                params.taildup = 0;
                p_label = "v1_first_design";
            }
            else if (value == 1) /* faster polls + silence budget, still no tail duplication */
            {
                params.taildup = 0;
                p_label = "v2_fast_polls";
            }
            else
            {
                /* the final design: keep the defaults set above */
            }
            result_t res = run(&params);
            row(p_file, p_label, loss_axis[idx], &params, &res);
        }
    }
    fclose(p_file);

    /* E3 LoRa listen-before-talk (CAD) off/on: recommendation for the LoRa mesh */
    p_file = csv(p_dir, "e3_lora_cad.csv", HDR);
    for (int chunk_size = 0; chunk_size <= 1; chunk_size++)
    {
        for (unsigned idx = 0; idx < name_len; idx++)
        {
            params_t params = tuned(SIM_LORA);
            params.trials = trials;
            params.loss = loss_axis[idx];
            params.chunk_size = chunk_size;
            result_t res = run(&params);
            row(p_file, chunk_size ? "cad_on" : "cad_off", loss_axis[idx], &params, &res);
        }
    }
    fclose(p_file);

    /* E4 selective repeat vs resend-everything, both radios, 2 hops */
    p_file = csv(p_dir, "e4_selective_vs_full.csv", HDR);
    for (int radio = 0; radio < 2; radio++)
    {
        for (int base = 0; base <= 1; base++)
        {
            for (unsigned idx = 0; idx < name_len; idx++)
            {
                params_t params = tuned(radio ? SIM_WIFI : SIM_LORA);
                params.trials = trials;
                params.loss = loss_axis[idx];
                params.baseline = base;
                char label[32];
                snprintf(label, sizeof(label), "%s_%s", radio ? "wifi" : "lora", base ? "resend_all" : "selective");
                result_t res = run(&params);
                row(p_file, label, loss_axis[idx], &params, &res);
            }
        }
    }
    fclose(p_file);

    /* E5 final protocol, loss sweep, both radios, 1-3 hops */
    p_file = csv(p_dir, "e5_loss_sweep.csv", HDR);
    const float loss_axis5[] = {0.0f, 0.05f, 0.1f, 0.15f, 0.2f, 0.25f, 0.3f, 0.35f, 0.4f};
    for (int radio = 0; radio < 2; radio++)
    {
        for (int hops = 1; hops <= 3; hops++)
        {
            for (unsigned idx = 0; idx < sizeof(loss_axis5) / sizeof(loss_axis5[0]); idx++)
            {
                params_t params = tuned(radio ? SIM_WIFI : SIM_LORA);
                params.trials = trials;
                params.loss = loss_axis5[idx];
                params.hops = hops;
                char label[32];
                snprintf(label, sizeof(label), "%s_%dhop", radio ? "wifi" : "lora", hops);
                result_t res = run(&params);
                row(p_file, label, loss_axis5[idx], &params, &res);
            }
        }
    }
    fclose(p_file);

    /* E6 image size vs time, both radios, 2 hops, 10% loss */
    p_file = csv(p_dir, "e6_size.csv", HDR);
    const uint32_t sizes[] = {2000, 4000, 6000, 8000, 12000, 16000, 20000};
    for (int radio = 0; radio < 2; radio++)
    {
        for (unsigned idx = 0; idx < 7; idx++)
        {
            params_t params = tuned(radio ? SIM_WIFI : SIM_LORA);
            params.trials = trials;
            params.size = sizes[idx];
            result_t res = run(&params);
            row(p_file, radio ? "wifi" : "lora", sizes[idx], &params, &res);
        }
    }
    fclose(p_file);
}

int main(int argc, char** pp_argv)
{
    if (argc >= 2 && !strcmp(pp_argv[1], "one"))
    {
        params_t params = defaults();
        for (int idx = 2; idx < argc; idx++)
        {
            parse_kv(&params, pp_argv[idx]);
        }
        result_t res = run(&params);
        print_one(&params, &res);
        return 0;
    }
    if (argc >= 3 && !strcmp(pp_argv[1], "suite"))
    {
        suite(pp_argv[2], argc >= 4 ? atoi(pp_argv[3]) : 40);
        return 0;
    }
    fprintf(stderr, "usage: %s one [k=v ...] | suite <outdir> [trials]\n", pp_argv[0]);
    return 2;
}
