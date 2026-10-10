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

static net_t N;

typedef struct
{
    int radio; /* SIM_LORA / SIM_WIFI */
    int hops;
    float loss;
    uint32_t gap, gapmax, pollto;
    uint8_t polls, copies, rounds, stalled, taildup;
    int cs;
    int trials;
    uint32_t size;
    int baseline;
    uint32_t hb;
    uint32_t seed;
} params_t;

typedef struct
{
    int trials, ok;
    double t_sum, t_sq, rounds_sum, sent_sum, frag_sum, air_sum;
    double t_ok[512];
    int n_ok;
    int why[4];
    int mismatched; /* DONE reported but the bridge's file differs: must stay 0 */
} result_t;

static params_t defaults(void)
{
    /* defaults = the shipped LoRa profile (alfp_profiles.c) */
    params_t p = {SIM_LORA, 2, 0.1f, 500, 500, 1500, 30, 1, 40, 10, 4, 0, 30, 8000, 0, 3000, 1};
    return p;
}

static void parse_kv(params_t* p, const char* kv)
{
    char k[32];
    char v[32];
    if (sscanf(kv, "%31[^=]=%31s", k, v) != 2)
    {
        return;
    }
    if (!strcmp(k, "radio"))
        p->radio = !strcmp(v, "wifi") ? SIM_WIFI : SIM_LORA;
    else if (!strcmp(k, "hops"))
        p->hops = atoi(v);
    else if (!strcmp(k, "loss"))
        p->loss = (float)atof(v);
    else if (!strcmp(k, "gap"))
        p->gap = (uint32_t)atoi(v);
    else if (!strcmp(k, "gapmax"))
        p->gapmax = (uint32_t)atoi(v);
    else if (!strcmp(k, "pollto"))
        p->pollto = (uint32_t)atoi(v);
    else if (!strcmp(k, "polls"))
        p->polls = (uint8_t)atoi(v);
    else if (!strcmp(k, "copies"))
        p->copies = (uint8_t)atoi(v);
    else if (!strcmp(k, "rounds"))
        p->rounds = (uint8_t)atoi(v);
    else if (!strcmp(k, "taildup"))
        p->taildup = (uint8_t)atoi(v);
    else if (!strcmp(k, "stalled"))
        p->stalled = (uint8_t)atoi(v);
    else if (!strcmp(k, "cs"))
        p->cs = atoi(v);
    else if (!strcmp(k, "trials"))
        p->trials = atoi(v);
    else if (!strcmp(k, "size"))
        p->size = (uint32_t)atoi(v);
    else if (!strcmp(k, "baseline"))
        p->baseline = atoi(v);
    else if (!strcmp(k, "hb"))
        p->hb = (uint32_t)atoi(v);
    else if (!strcmp(k, "seed"))
        p->seed = (uint32_t)atoi(v);
}

static bool sender_done(net_t* n)
{
    return n->node[n->cfg.n_nodes].n_tx >= 1;
}

static int cmp_d(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static result_t run(const params_t* p)
{
    result_t r;
    memset(&r, 0, sizeof(r));
    alfp_profile_t prof[2] = {
        {"lora", 500, 1500, 1500, 60000, 40, 10, 30, 1, 4, ALFP_RADIO_LORA},
        {"wifi", 10, 80, 200, 15000, 40, 10, 40, 1, 4, ALFP_RADIO_WIFI},
    };
    alfp_profile_t* pr = &prof[p->radio == SIM_WIFI ? 1 : 0];
    pr->gap_ms = p->gap;
    pr->gap_max_ms = p->gapmax;
    pr->poll_timeout_ms = p->pollto;
    pr->max_polls = p->polls;
    pr->poll_copies = p->copies;
    pr->max_rounds = p->rounds;
    pr->max_stalled = p->stalled;
    pr->tail_dup = p->taildup;
    net_set_options(prof, 2, p->baseline != 0);

    for (int t = 0; t < p->trials; t++)
    {
        sim_cfg_t cfg;
        uint8_t nodes = (uint8_t)(p->hops + 1);
        sim_default_cfg(&cfg, nodes);
        cfg.seed = p->seed * 1000u + (uint32_t)t;
        cfg.heartbeat_ms = p->hb;
        sim_topology_chain(&cfg, -1); /* sender = last node, bridge = node 1 */
        cfg.kind[p->radio].link_loss = p->loss;
        if (p->radio == SIM_LORA)
        {
            cfg.kind[SIM_LORA].carrier_sense = p->cs != 0;
        }
        net_init(&N, &cfg, "sim_out/exp", p->radio == SIM_WIFI ? 2 : 1, false);
        net_run_ms(&N, 300 + (sim_rand() % 3000)); /* random phase against the heartbeat */
        net_make_file(&N, nodes, "img.jpg", p->size, cfg.seed);
        sim_set_current(nodes);
        const mesh_radio_t* radio = p->radio == SIM_WIFI ? &sim_wifi : &sim_lora;
        if (alfp_send_file(&N.node[nodes].alfp, radio, 1, "img.jpg", NULL, 0) != ALFP_OK)
        {
            fprintf(stderr, "send_file failed\n");
            exit(1);
        }
        uint32_t air0 = sim_airtime_used(p->radio);
        net_run_until(&N, sender_done, 3600000);
        r.trials++;
        alfp_tx_report_t* rep = &N.node[nodes].tx_reports[0];
        if (N.node[nodes].n_tx && rep->result == ALFP_TX_GAVE_UP && rep->why < 4)
        {
            r.why[rep->why]++;
        }
        if (N.node[nodes].n_tx && rep->result == ALFP_TX_DONE)
        {
            if (N.node[1].n_rx < 1 || !net_files_equal(&N, nodes, "img.jpg", 1, N.node[1].rx_paths[0]))
            {
                r.mismatched++;
            }
            double secs = (rep->finished_ms - rep->started_ms) / 1000.0;
            r.ok++;
            r.t_sum += secs;
            r.t_sq += secs * secs;
            r.rounds_sum += rep->rounds;
            r.sent_sum += rep->data_sent;
            r.frag_sum += rep->frag_count;
            r.air_sum += (sim_airtime_used(p->radio) - air0) / 1000.0;
            if (r.n_ok < 512)
            {
                r.t_ok[r.n_ok++] = secs;
            }
        }
        net_close(&N);
    }
    if (r.mismatched)
    {
        fprintf(stderr, "ERROR: %d transfers reported done with a wrong file\n", r.mismatched);
    }
    return r;
}

static double mean(double s, int n)
{
    return n ? s / n : NAN;
}

static double median(result_t* r)
{
    if (!r->n_ok)
    {
        return NAN;
    }
    qsort(r->t_ok, (size_t)r->n_ok, sizeof(double), cmp_d);
    return r->n_ok % 2 ? r->t_ok[r->n_ok / 2] : (r->t_ok[r->n_ok / 2 - 1] + r->t_ok[r->n_ok / 2]) / 2;
}

static double pct(result_t* r, double q)
{
    if (!r->n_ok)
    {
        return NAN;
    }
    median(r); /* sorts */
    int i = (int)floor(q * (r->n_ok - 1) + 0.5);
    return r->t_ok[i];
}

static void print_one(const params_t* p, result_t* r)
{
    printf("%s hops=%d loss=%.2f gap=%u..%u pollto=%u polls=%u copies=%u taildup=%u cs=%d base=%d size=%u | ok %d/%d (%.0f%%) "
           "t_med=%.1fs rounds=%.2f sent/frag=%.2f air=%.1fs fail[polls=%d stall=%d cap=%d] bad_files=%d\n",
           p->radio == SIM_WIFI ? "wifi" : "lora", p->hops, p->loss, p->gap, p->gapmax, p->pollto, p->polls, p->copies, p->taildup, p->cs,
           p->baseline, p->size, r->ok, r->trials, 100.0 * r->ok / r->trials, median(r),
           mean(r->rounds_sum, r->ok), r->frag_sum ? r->sent_sum / r->frag_sum : NAN, mean(r->air_sum, r->ok),
           r->why[1], r->why[2], r->why[3], r->mismatched);
}

/* ---- suite: every experiment used in the report ------------------------------------------- */

static FILE* csv(const char* dir, const char* name, const char* header)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE* f = fopen(p, "w");
    if (!f)
    {
        perror(p);
        exit(1);
    }
    fprintf(f, "%s\n", header);
    return f;
}

static void row(FILE* f, const char* label, double x, const params_t* p, result_t* r)
{
    fprintf(f, "%s,%g,%d,%d,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", label, x, r->ok, r->trials,
            (double)r->ok / r->trials, median(r), pct(r, 0.1), pct(r, 0.9), mean(r->rounds_sum, r->ok),
            r->frag_sum ? r->sent_sum / r->frag_sum : NAN, mean(r->air_sum, r->ok));
    fflush(f);
    print_one(p, r);
}

#define HDR "series,x,ok,trials,success,t_median_s,t_p10_s,t_p90_s,rounds_mean,sent_per_fragment,airtime_s"

static params_t tuned(int radio)
{
    params_t p = defaults();
    p.radio = radio;
    if (radio == SIM_WIFI)
    {
        p.gap = 10;
        p.gapmax = 10;
        p.pollto = 200;
        p.polls = 40;
    }
    return p;
}

static void suite(const char* dir, int trials)
{
    const float loss_axis[] = {0.0f, 0.05f, 0.1f, 0.15f, 0.2f, 0.25f, 0.3f};
    const unsigned nl = sizeof(loss_axis) / sizeof(loss_axis[0]);

    /* E1 LoRa pacing gap vs hops (half-duplex relays and hidden terminals), 10% loss per hop */
    FILE* f = csv(dir, "e1_lora_gap.csv", HDR);
    const uint32_t gaps[] = {0, 250, 500, 700, 1000, 1300};
    for (int hops = 1; hops <= 4; hops++)
    {
        for (unsigned i = 0; i < sizeof(gaps) / sizeof(gaps[0]); i++)
        {
            params_t p = tuned(SIM_LORA);
            p.trials = trials;
            p.hops = hops;
            p.gap = p.gapmax = gaps[i];
            char label[32];
            snprintf(label, sizeof(label), "lora_%dhop", hops);
            result_t r = run(&p);
            row(f, label, gaps[i], &p, &r);
        }
    }
    fclose(f);

    /* E2 first design vs final design (control path), LoRa 2 hops */
    f = csv(dir, "e2_design_iterations.csv", HDR);
    for (int v = 0; v < 3; v++)
    {
        for (unsigned i = 0; i < nl; i++)
        {
            params_t p = tuned(SIM_LORA);
            p.trials = trials;
            p.loss = loss_axis[i];
            const char* label = "final";
            if (v == 0) /* first design: 3 s poll timeout, give up after 5 silent polls, 10 rounds, no tail dup */
            {
                p.pollto = 3000;
                p.polls = 5;
                p.rounds = 10;
                p.taildup = 0;
                label = "v1_first_design";
            }
            else if (v == 1) /* faster polls + silence budget, still no tail duplication */
            {
                p.taildup = 0;
                label = "v2_fast_polls";
            }
            result_t r = run(&p);
            row(f, label, loss_axis[i], &p, &r);
        }
    }
    fclose(f);

    /* E3 LoRa listen-before-talk (CAD) off/on: recommendation for the LoRa mesh */
    f = csv(dir, "e3_lora_cad.csv", HDR);
    for (int cs = 0; cs <= 1; cs++)
    {
        for (unsigned i = 0; i < nl; i++)
        {
            params_t p = tuned(SIM_LORA);
            p.trials = trials;
            p.loss = loss_axis[i];
            p.cs = cs;
            result_t r = run(&p);
            row(f, cs ? "cad_on" : "cad_off", loss_axis[i], &p, &r);
        }
    }
    fclose(f);

    /* E4 selective repeat vs resend-everything, both radios, 2 hops */
    f = csv(dir, "e4_selective_vs_full.csv", HDR);
    for (int radio = 0; radio < 2; radio++)
    {
        for (int base = 0; base <= 1; base++)
        {
            for (unsigned i = 0; i < nl; i++)
            {
                params_t p = tuned(radio ? SIM_WIFI : SIM_LORA);
                p.trials = trials;
                p.loss = loss_axis[i];
                p.baseline = base;
                char label[32];
                snprintf(label, sizeof(label), "%s_%s", radio ? "wifi" : "lora", base ? "resend_all" : "selective");
                result_t r = run(&p);
                row(f, label, loss_axis[i], &p, &r);
            }
        }
    }
    fclose(f);

    /* E5 final protocol, loss sweep, both radios, 1-3 hops */
    f = csv(dir, "e5_loss_sweep.csv", HDR);
    const float l5[] = {0.0f, 0.05f, 0.1f, 0.15f, 0.2f, 0.25f, 0.3f, 0.35f, 0.4f};
    for (int radio = 0; radio < 2; radio++)
    {
        for (int hops = 1; hops <= 3; hops++)
        {
            for (unsigned i = 0; i < sizeof(l5) / sizeof(l5[0]); i++)
            {
                params_t p = tuned(radio ? SIM_WIFI : SIM_LORA);
                p.trials = trials;
                p.loss = l5[i];
                p.hops = hops;
                char label[32];
                snprintf(label, sizeof(label), "%s_%dhop", radio ? "wifi" : "lora", hops);
                result_t r = run(&p);
                row(f, label, l5[i], &p, &r);
            }
        }
    }
    fclose(f);

    /* E6 image size vs time, both radios, 2 hops, 10% loss */
    f = csv(dir, "e6_size.csv", HDR);
    const uint32_t sizes[] = {2000, 4000, 6000, 8000, 12000, 16000, 20000};
    for (int radio = 0; radio < 2; radio++)
    {
        for (unsigned i = 0; i < 7; i++)
        {
            params_t p = tuned(radio ? SIM_WIFI : SIM_LORA);
            p.trials = trials;
            p.size = sizes[i];
            result_t r = run(&p);
            row(f, radio ? "wifi" : "lora", sizes[i], &p, &r);
        }
    }
    fclose(f);
}

int main(int argc, char** argv)
{
    if (argc >= 2 && !strcmp(argv[1], "one"))
    {
        params_t p = defaults();
        for (int i = 2; i < argc; i++)
        {
            parse_kv(&p, argv[i]);
        }
        result_t r = run(&p);
        print_one(&p, &r);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "suite"))
    {
        suite(argv[2], argc >= 4 ? atoi(argv[3]) : 40);
        return 0;
    }
    fprintf(stderr, "usage: %s one [k=v ...] | suite <outdir> [trials]\n", argv[0]);
    return 2;
}
