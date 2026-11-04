/*
 * test_transfer.c - end-to-end transfers over the simulated meshes.
 *
 * Every test sends real files between simulated nodes, each with its own "SD card" directory,
 * and checks the bridge's copy is byte-for-byte identical to the original.
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#include "capture_meta.h"
#include "sim_net.h"
#include "testlib.h"

static net_t N; /* large: keep off the stack */

#define BRIDGE 1

static bool bridge_has_one(net_t* n)
{
    return n->node[BRIDGE].n_rx >= 1;
}

static bool sender2_done(net_t* n)
{
    return n->node[2].n_tx >= 1;
}

static bool sender2_sent_twice(net_t* n)
{
    return n->node[2].n_tx >= 2;
}

static void setup(int topo, int radios, uint8_t nodes, bool outbox, uint32_t seed)
{
    sim_cfg_t cfg;
    sim_default_cfg(&cfg, nodes);
    cfg.seed = seed;
    if (topo == 0)
    {
        sim_topology_full(&cfg, -1);
    }
    else if (topo == 1)
    {
        sim_topology_chain(&cfg, -1);
    }
    else
    {
        sim_topology_trail(&cfg, -1);
    }
    net_init(&N, &cfg, "sim_out/test", radios, outbox);
    net_run_ms(&N, 200); /* let the first heartbeats go out */
}

static bool start_send(uint8_t node, const mesh_radio_t* r, const char* path, uint32_t size, uint32_t seed)
{
    net_make_file(&N, node, path, size, seed);
    sim_set_current(node);
    return alfp_send_file(&N.node[node].alfp, r, BRIDGE, path, NULL, 0) == ALFP_OK;
}

static bool received_equals(uint8_t node, const char* path, unsigned idx)
{
    net_node_t* b = &N.node[BRIDGE];
    if (idx >= b->n_rx)
    {
        return false;
    }
    return net_files_equal(&N, node, path, BRIDGE, b->rx_paths[idx]);
}

static unsigned count_files(uint8_t node, const char* dir, const char* suffix)
{
    char p[600];
    snprintf(p, sizeof(p), "%s/node%u/%s", N.root, (unsigned)node, dir);
    DIR* d = opendir(p);
    unsigned c = 0;
    if (!d)
    {
        return 0;
    }
    struct dirent* e;
    while ((e = readdir(d)))
    {
        size_t l = strlen(e->d_name), s = strlen(suffix);
        if (l > s && strcmp(e->d_name + l - s, suffix) == 0)
        {
            c++;
        }
    }
    closedir(d);
    return c;
}

static void no_leaks(void)
{
    for (uint8_t i = 1; i <= N.cfg.n_nodes; i++)
    {
        CHECK(N.node[i].sp.open_count == 0 || (i == BRIDGE && N.node[i].sp.open_count <= ALFP_RX_SESSIONS));
    }
}

/* ---------------------------------------------------------------------------------------- */

static void test_lora_clean_link_one_round(void)
{
    setup(0, 1, 2, false, 1);
    CHECK(start_send(2, &sim_lora, "img/a.jpg", 10000, 1));
    CHECK(net_run_until(&N, sender2_done, 120000));
    alfp_tx_report_t* r = &N.node[2].tx_reports[0];
    CHECK(r->result == ALFP_TX_DONE);
    /* Even with no random loss, the bridge's 3 s heartbeat can collide with a fragment (LoRa has
     * no carrier sense), so allow one repair round. */
    CHECK(r->rounds <= 2);
    CHECK(r->frag_count == 42 && r->frag_size == 241); /* 250 - 9 */
    CHECK(received_equals(2, "img/a.jpg", 0));
    printf("    LoRa 10 kB, 1 hop, no loss: %.1f s, %u packets\n", (r->finished_ms - r->started_ms) / 1000.0,
           r->data_sent + r->manifests_sent + r->polls_sent);
    no_leaks();
    net_close(&N);
}

static void test_wifi_clean_link_one_round(void)
{
    setup(0, 2, 2, false, 2);
    CHECK(start_send(2, &sim_wifi, "img/a.jpg", 10000, 2));
    CHECK(net_run_until(&N, sender2_done, 60000));
    alfp_tx_report_t* r = &N.node[2].tx_reports[0];
    CHECK(r->result == ALFP_TX_DONE && r->rounds == 1);
    CHECK(r->frag_size == 236); /* 245 - 9 */
    CHECK(received_equals(2, "img/a.jpg", 0));
    printf("    WiFi 10 kB, 1 hop, no loss: %.2f s\n", (r->finished_ms - r->started_ms) / 1000.0);
    net_close(&N);
}

static void test_lora_loss_sweep_multihop(void)
{
    const float losses[] = {0.1f, 0.2f, 0.3f};
    for (unsigned i = 0; i < 3; i++)
    {
        setup(1, 1, 3, false, 10 + i); /* chain: 3 -> 2 -> 1, two hops */
        sim_set_link_loss(SIM_LORA, losses[i]);
        CHECK(start_send(3, &sim_lora, "img/b.jpg", 8000, 3));
        bool fin = net_run_until(&N, bridge_has_one, 600000);
        net_run_ms(&N, 10000);
        CHECK(fin);
        CHECK(N.node[3].n_tx == 1 && N.node[3].tx_reports[0].result == ALFP_TX_DONE);
        CHECK(received_equals(3, "img/b.jpg", 0));
        alfp_tx_report_t* r = &N.node[3].tx_reports[0];
        printf("    LoRa 2 hops, %2.0f%% loss/hop: %u rounds, %u resent of %u, %.1f s\n", losses[i] * 100, r->rounds,
               r->data_resent, r->frag_count, (r->finished_ms - r->started_ms) / 1000.0);
        CHECK(r->rounds >= 2); /* loss really happened and was repaired */
        net_close(&N);
    }
}

static void test_wifi_heavy_loss(void)
{
    setup(2, 2, 5, false, 20); /* trail with redundant links */
    sim_set_link_loss(SIM_WIFI, 0.35f);
    CHECK(start_send(5, &sim_wifi, "img/c.jpg", 20000, 4));
    CHECK(net_run_until(&N, bridge_has_one, 300000));
    net_run_ms(&N, 3000);
    CHECK(N.node[5].n_tx == 1 && N.node[5].tx_reports[0].result == ALFP_TX_DONE);
    CHECK(received_equals(5, "img/c.jpg", 0));
    net_close(&N);
}

/* Bridge deaf for the first 150 ms: the manifest (and maybe early data) is lost.
 * The bridge must open the session from DATA, then ask for the manifest in its NACK. */
static void test_manifest_lost(void)
{
    setup(0, 1, 2, false, 30);
    sim_set_drop_rate(BRIDGE, SIM_LORA, 1.0f);
    CHECK(start_send(2, &sim_lora, "img/d.jpg", 5000, 5));
    net_run_ms(&N, 150);
    sim_set_drop_rate(BRIDGE, SIM_LORA, 0.0f);
    CHECK(net_run_until(&N, sender2_done, 120000));
    alfp_tx_report_t* r = &N.node[2].tx_reports[0];
    CHECK(r->result == ALFP_TX_DONE);
    CHECK(r->manifests_sent >= 2);
    CHECK(N.node[BRIDGE].alfp.stats.data_orphan >= 1);
    CHECK(received_equals(2, "img/d.jpg", 0));
    net_close(&N);
}

/* The bridge's ACK is lost. The sender polls; the bridge remembers the finished session and
 * ACKs again instead of starting over. Exactly one file must appear on the bridge. */
static void test_ack_lost(void)
{
    setup(0, 1, 2, false, 40);
    CHECK(start_send(2, &sim_lora, "img/e.jpg", 4000, 6));
    CHECK(net_run_until(&N, bridge_has_one, 120000));
    sim_set_drop_rate(2, SIM_LORA, 1.0f); /* sender goes deaf right as the ACK is sent */
    net_run_ms(&N, 4000);
    sim_set_drop_rate(2, SIM_LORA, 0.0f);
    CHECK(net_run_until(&N, sender2_done, 60000));
    CHECK(N.node[2].tx_reports[0].result == ALFP_TX_DONE);
    CHECK(N.node[2].tx_reports[0].polls_sent >= 2);
    net_run_ms(&N, 5000);
    CHECK(N.node[BRIDGE].n_rx == 1);
    CHECK(count_files(BRIDGE, "inbox", ".bin") == 1);
    net_close(&N);
}

/* Bits flipped AFTER the radio's own CRC (UART, SPI or a relay's buffer). CRC-16 must catch them. */
static void test_corruption_caught_by_crc16(void)
{
    setup(0, 1, 2, false, 50);
    sim_set_corrupt_rate(SIM_LORA, 0.1f);
    CHECK(start_send(2, &sim_lora, "img/f.jpg", 10000, 7));
    CHECK(net_run_until(&N, sender2_done, 300000));
    CHECK(N.node[2].tx_reports[0].result == ALFP_TX_DONE);
    CHECK(N.node[BRIDGE].alfp.stats.pkts_bad_crc > 0);
    CHECK(received_equals(2, "img/f.jpg", 0));
    printf("    %u corrupted packets rejected by CRC-16, file still exact\n",
           (unsigned)N.node[BRIDGE].alfp.stats.pkts_bad_crc);
    net_close(&N);
}

/* The bridge's SD card corrupts a write. Each fragment passed CRC-16, but the file CRC-32
 * catches it; the sender resends and the final file is still exact. */
static void test_sd_fault_caught_by_crc32(void)
{
    setup(0, 2, 2, false, 60);
    N.node[BRIDGE].sp.fault_write_number = N.node[BRIDGE].sp.writes + 10; /* the 10th fragment written */
    CHECK(start_send(2, &sim_wifi, "img/g.jpg", 10000, 8));
    CHECK(net_run_until(&N, sender2_done, 120000));
    CHECK(N.node[2].tx_reports[0].result == ALFP_TX_DONE);
    CHECK(N.node[BRIDGE].alfp.stats.rx_crc_fail >= 1);
    CHECK(received_equals(2, "img/g.jpg", 0));
    printf("    %u file CRC-32 mismatches caught and repaired\n", (unsigned)N.node[BRIDGE].alfp.stats.rx_crc_fail);
    net_close(&N);
}

/* WiFi preferred when the bridge is heard on it; LoRa when WiFi cannot reach. */
static void test_radio_selection(void)
{
    const mesh_radio_t* pref[2] = {&sim_wifi, &sim_lora};
    setup(0, 3, 2, false, 70);
    net_run_ms(&N, 3500);
    sim_set_current(2);
    CHECK(alfp_select_radio(pref, 2, BRIDGE, sim_now(), 10000) == &sim_wifi);
    sim_set_link(SIM_WIFI, 1, 2, false);
    net_run_ms(&N, 12000); /* WiFi heartbeat now older than 10 s */
    sim_set_current(2);
    CHECK(alfp_select_radio(pref, 2, BRIDGE, sim_now(), 10000) == &sim_lora);
    sim_set_link(SIM_LORA, 1, 2, false);
    net_run_ms(&N, 12000);
    sim_set_current(2);
    CHECK(alfp_select_radio(pref, 2, BRIDGE, sim_now(), 10000) == NULL);
    net_close(&N);
}

/* alfp_select_radio_for_size(): pure function, exercised directly against two fake radios so
 * every case (sizes, freshness combinations, now_ms wraparound) is fast and exact, with no
 * network simulation needed. alfp_select_radio() itself is untouched (see test_radio_selection
 * above, still passing unmodified). */
static uint32_t g_fsz_lora_heard;
static uint32_t g_fsz_wifi_heard;
static bool g_fsz_have_lora;
static bool g_fsz_have_wifi;

static bool fsz_last_heard_lora(uint8_t node_id, uint32_t* ms)
{
    (void)node_id;
    if (!g_fsz_have_lora)
    {
        return false;
    }
    *ms = g_fsz_lora_heard;
    return true;
}

static bool fsz_last_heard_wifi(uint8_t node_id, uint32_t* ms)
{
    (void)node_id;
    if (!g_fsz_have_wifi)
    {
        return false;
    }
    *ms = g_fsz_wifi_heard;
    return true;
}

static mesh_radio_t fsz_lora = {.name = "lora", .last_heard = fsz_last_heard_lora};
static mesh_radio_t fsz_wifi = {.name = "wifi", .last_heard = fsz_last_heard_wifi};

static void test_radio_selection_for_size(void)
{
    const mesh_radio_t* pref[2] = {&fsz_lora, &fsz_wifi};
    const uint32_t FRESH = 10000u;
    const uint32_t THR = 1024u;
    const uint32_t t = 50000u;

    /* Both fresh: below threshold picks LoRa, at/above picks WiFi. Boundary and max-size cases. */
    g_fsz_have_lora = true;
    g_fsz_lora_heard = t - 1000u;
    g_fsz_have_wifi = true;
    g_fsz_wifi_heard = t - 1000u;
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, t, FRESH, 0u, THR) == &fsz_lora);
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, t, FRESH, 1u, THR) == &fsz_lora);
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, t, FRESH, THR - 1u, THR) == &fsz_lora);
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, t, FRESH, THR, THR) == &fsz_wifi);
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, t, FRESH, THR + 1u, THR) == &fsz_wifi);
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, t, FRESH, 236u * ALFP_MAX_FRAGS, THR) == &fsz_wifi);
    CHECK(alfp_radio_was_size_preferred(pref, 2, 0u, THR, &fsz_lora));
    CHECK(!alfp_radio_was_size_preferred(pref, 2, 0u, THR, &fsz_wifi));
    CHECK(alfp_radio_was_size_preferred(pref, 2, THR, THR, &fsz_wifi));

    /* Preferred radio stale: falls back to the other one. */
    g_fsz_wifi_heard = t - FRESH - 1u; /* WiFi just over fresh_ms old */
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, t, FRESH, THR, THR) == &fsz_lora);
    CHECK(!alfp_radio_was_size_preferred(pref, 2, THR, THR, &fsz_lora)); /* that pick was a fallback, not size */
    g_fsz_wifi_heard = t - 1000u; /* wifi fresh again */
    g_fsz_lora_heard = t - FRESH - 1u;
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, t, FRESH, 0u, THR) == &fsz_wifi);

    /* Both stale, or never heard at all: NULL regardless of size. */
    g_fsz_wifi_heard = t - FRESH - 1u;
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, t, FRESH, 0u, THR) == NULL);
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, t, FRESH, THR, THR) == NULL);
    g_fsz_have_lora = false;
    g_fsz_have_wifi = false;
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, t, FRESH, 0u, THR) == NULL);

    /* now_ms wraparound: heard 16 ms before the 32-bit wrap, "now" 16 ms after it (32 ms elapsed).
     * Must use the same unsigned-subtraction math alfp_select_radio() already relies on. */
    g_fsz_have_lora = true;
    g_fsz_lora_heard = 0xFFFFFFF0u;
    const uint32_t wrapped_now = 0x00000010u;
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, wrapped_now, FRESH, 0u, THR) == &fsz_lora);
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, wrapped_now, 20u, 0u, THR) == NULL); /* 32 ms > 20 ms fresh */
}

/* Outbox-level proof that the size rule actually drives radio choice end to end, through the
 * real network simulator: a small job goes over LoRa, a large one over WiFi, with the threshold
 * turned on via alfp_outbox_set_size_threshold() (default after alfp_outbox_init() is 0 = off,
 * proven byte-identical-to-before by every other test in this file still passing unmodified). */
static void test_outbox_size_based_routing(void)
{
    setup(0, 3, 2, true, 200);
    net_run_ms(&N, 3500); /* both links fresh */
    alfp_outbox_set_size_threshold(&N.node[2].outbox, 1024);
    net_make_file(&N, 2, "img/small.bin", 200, 201);
    net_make_file(&N, 2, "img/large.bin", 20000, 202);

    sim_set_current(2);
    CHECK(alfp_outbox_add(&N.node[2].outbox, "img/small.bin", NULL, 0));
    net_run_ms(&N, 50); /* outbox has started the small transfer */
    CHECK(alfp_tx_active(&N.node[2].alfp));
    CHECK(N.node[2].alfp.tx.radio != NULL && strcmp(N.node[2].alfp.tx.radio->name, "lora") == 0);
    CHECK(net_run_until(&N, sender2_done, 120000));
    CHECK(strcmp(N.node[2].tx_reports[0].radio, "lora") == 0);
    CHECK(N.node[2].tx_reports[0].result == ALFP_TX_DONE);

    sim_set_current(2);
    CHECK(alfp_outbox_add(&N.node[2].outbox, "img/large.bin", NULL, 0));
    net_run_ms(&N, 50);
    CHECK(alfp_tx_active(&N.node[2].alfp));
    CHECK(N.node[2].alfp.tx.radio != NULL && strcmp(N.node[2].alfp.tx.radio->name, "wifi") == 0);
    CHECK(net_run_until(&N, sender2_sent_twice, 120000));
    CHECK(strcmp(N.node[2].tx_reports[1].radio, "wifi") == 0);
    CHECK(N.node[2].tx_reports[1].result == ALFP_TX_DONE);
    net_close(&N);
}

/* WiFi breaks in the middle of a transfer. The transfer must NOT switch to LoRa: it finishes
 * or fails on WiFi. The outbox's next attempt is a new transfer, which may pick LoRa. */
static void test_never_switch_mid_transfer(void)
{
    setup(0, 3, 2, true, 80);
    net_run_ms(&N, 3500);
    net_make_file(&N, 2, "img/h.jpg", 20000, 9);
    sim_set_current(2);
    CHECK(alfp_outbox_add(&N.node[2].outbox, "img/h.jpg", NULL, 0));
    net_run_ms(&N, 150); /* transfer has started on WiFi */
    CHECK(alfp_tx_active(&N.node[2].alfp) && N.node[2].alfp.tx.radio == &sim_wifi);
    sim_set_link(SIM_WIFI, 1, 2, false); /* WiFi dies mid-transfer */
    CHECK(net_run_until(&N, bridge_has_one, 400000));
    net_run_ms(&N, 5000);
    net_node_t* s = &N.node[2];
    CHECK(s->n_tx >= 2);
    CHECK(strcmp(s->tx_reports[0].radio, "wifi") == 0 && s->tx_reports[0].result == ALFP_TX_GAVE_UP);
    CHECK(strcmp(s->tx_reports[s->n_tx - 1].radio, "lora") == 0 &&
          s->tx_reports[s->n_tx - 1].result == ALFP_TX_DONE);
    CHECK(received_equals(2, "img/h.jpg", 0));
    CHECK(alfp_outbox_count(&s->outbox) == 0);
    net_close(&N);
}

/* No radio can reach the bridge: the capture waits on SD, then goes as soon as a link appears. */
static void test_store_and_forward(void)
{
    sim_cfg_t cfg;
    sim_default_cfg(&cfg, 3);
    cfg.seed = 90; /* no links at all */
    net_init(&N, &cfg, "sim_out/test", 3, true);
    capture_meta_t m;
    capture_meta_clear(&m);
    m.node_id = 3;
    m.capture_id = 77;
    m.temp_c_x10 = 291;
    uint8_t mb[CAPTURE_META_LEN];
    capture_meta_pack(&m, mb);
    for (int i = 0; i < 3; i++)
    {
        char p[32];
        snprintf(p, sizeof(p), "img/s%d.jpg", i);
        net_make_file(&N, 3, p, 3000 + 500 * (uint32_t)i, 20 + (uint32_t)i);
        sim_set_current(3);
        CHECK(alfp_outbox_add(&N.node[3].outbox, p, mb, sizeof(mb)));
    }
    net_run_ms(&N, 20000);
    CHECK(alfp_outbox_count(&N.node[3].outbox) == 3);
    CHECK(N.node[3].n_tx == 0 && N.node[3].outbox.no_radio_waits > 0);
    sim_set_link(SIM_LORA, 3, 2, true); /* node 3 comes into range of the relay */
    sim_set_link(SIM_LORA, 2, 1, true);
    for (int k = 0; k < 400 && N.node[BRIDGE].n_rx < 3; k++)
    {
        net_run_ms(&N, 1000);
    }
    net_run_ms(&N, 10000); /* let the last ACK reach the sender */
    for (unsigned i = 0; i < N.node[3].n_tx; i++)
    {
        alfp_tx_report_t* r = &N.node[3].tx_reports[i];
        printf("    attempt %u: %s on %s, %u rounds, %.1f s\n", i + 1, alfp_tx_result_str(r->result), r->radio,
               r->rounds, (r->finished_ms - r->started_ms) / 1000.0);
    }
    CHECK(N.node[BRIDGE].n_rx == 3);
    CHECK(alfp_outbox_count(&N.node[3].outbox) == 0);
    for (unsigned i = 0; i < N.node[BRIDGE].n_rx && i < 3; i++)
    {
        char p[32];
        snprintf(p, sizeof(p), "img/s%u.jpg", i);
        CHECK(received_equals(3, p, i)); /* oldest first */
    }
    CHECK(count_files(BRIDGE, "inbox", ".meta") == 3);
    net_close(&N);
}

/* Two field nodes send at the same time; the bridge reassembles both. */
static void test_two_senders_at_once(void)
{
    setup(0, 2, 3, false, 100);
    CHECK(start_send(2, &sim_wifi, "img/x.jpg", 12000, 30));
    CHECK(start_send(3, &sim_wifi, "img/y.jpg", 9000, 31));
    for (int k = 0; k < 120 && N.node[BRIDGE].n_rx < 2; k++)
    {
        net_run_ms(&N, 500);
    }
    CHECK(N.node[BRIDGE].n_rx == 2);
    bool ok = true;
    for (unsigned i = 0; i < N.node[BRIDGE].n_rx; i++)
    {
        uint8_t src = N.node[BRIDGE].rx_reports[i].src;
        ok &= received_equals(src, src == 2 ? "img/x.jpg" : "img/y.jpg", i);
    }
    CHECK(ok);
    net_close(&N);
}

/* The sender vanishes mid-transfer: the bridge must free the session and delete the .part file. */
static void test_abandoned_session_expires(void)
{
    setup(0, 2, 2, false, 110);
    CHECK(start_send(2, &sim_wifi, "img/z.jpg", 20000, 32));
    net_run_ms(&N, 300);
    N.node[2].n_radios = 0; /* node 2 loses power */
    sim_set_link(SIM_WIFI, 1, 2, false);
    CHECK(count_files(BRIDGE, "rx", ".part") == 1);
    net_run_ms(&N, 17000); /* WiFi rx_expiry is 15 s */
    CHECK(N.node[BRIDGE].alfp.stats.rx_expired == 1);
    CHECK(count_files(BRIDGE, "rx", ".part") == 0);
    CHECK(N.node[BRIDGE].sp.open_count == 0);
    net_close(&N);
}

static void test_edge_sizes(void)
{
    const uint32_t sizes[] = {1, 236, 237, 236 * 3, 236u * ALFP_MAX_FRAGS};
    for (unsigned i = 0; i < 5; i++)
    {
        setup(0, 2, 2, false, 120 + i);
        CHECK(start_send(2, &sim_wifi, "img/e.bin", sizes[i], 40 + i));
        CHECK(net_run_until(&N, sender2_done, 600000));
        CHECK(N.node[2].tx_reports[0].result == ALFP_TX_DONE);
        CHECK(received_equals(2, "img/e.bin", 0));
        net_close(&N);
    }
    setup(0, 2, 2, false, 130);
    net_make_file(&N, 2, "img/big.bin", 236u * ALFP_MAX_FRAGS + 1u, 50);
    sim_set_current(2);
    CHECK(alfp_send_file(&N.node[2].alfp, &sim_wifi, BRIDGE, "img/big.bin", NULL, 0) == ALFP_E_TOO_BIG);
    net_make_file(&N, 2, "img/empty.bin", 0, 51);
    CHECK(alfp_send_file(&N.node[2].alfp, &sim_wifi, BRIDGE, "img/empty.bin", NULL, 0) == ALFP_E_EMPTY);
    CHECK(alfp_send_file(&N.node[2].alfp, &sim_wifi, BRIDGE, "img/missing.bin", NULL, 0) == ALFP_E_OPEN);
    CHECK(N.node[2].sp.open_count == 0);
    net_close(&N);
}

/* Queue survives a reboot: job files are re-read from SD. */
static void test_outbox_survives_reboot(void)
{
    setup(0, 1, 2, true, 140);
    sim_set_link(SIM_LORA, 1, 2, false);
    for (int i = 0; i < 4; i++)
    {
        net_make_file(&N, 2, "img/r.jpg", 1000, 60);
        sim_set_current(2);
        CHECK(alfp_outbox_add(&N.node[2].outbox, "img/r.jpg", NULL, 0));
    }
    alfp_outbox_t again;
    sim_set_current(2);
    alfp_outbox_init(&again, &N.node[2].alfp, N.node[2].radios, N.node[2].n_radios, BRIDGE, 10000, "outbox");
    CHECK(alfp_outbox_count(&again) == 4);
    CHECK(again.next_seq == N.node[2].outbox.next_seq);
    net_close(&N);
}

/* Metadata goes through intact and lands next to the image on the bridge. */
static void test_metadata_delivered(void)
{
    setup(0, 2, 2, false, 150);
    capture_meta_t m, back;
    capture_meta_clear(&m);
    m.node_id = 2;
    m.capture_id = 4242;
    m.humidity_x10 = 903;
    m.weather_label = 3;
    m.sound_class = 5;
    uint8_t mb[CAPTURE_META_LEN];
    capture_meta_pack(&m, mb);
    net_make_file(&N, 2, "img/m.jpg", 3000, 70);
    sim_set_current(2);
    CHECK(alfp_send_file(&N.node[2].alfp, &sim_wifi, BRIDGE, "img/m.jpg", mb, sizeof(mb)) == ALFP_OK);
    CHECK(net_run_until(&N, sender2_done, 60000));
    char p[600];
    snprintf(p, sizeof(p), "%s/node1/%s", N.root, N.node[BRIDGE].rx_paths[0]);
    char* dot = strrchr(p, '.');
    strcpy(dot, ".meta");
    FILE* f = fopen(p, "rb");
    CHECK(f != NULL);
    if (f)
    {
        uint8_t in[64];
        size_t n = fread(in, 1, sizeof(in), f);
        fclose(f);
        CHECK(capture_meta_unpack(in, (uint8_t)n, &back) == 0);
        CHECK(back.capture_id == 4242 && back.humidity_x10 == 903 && back.weather_label == 3 && back.sound_class == 5);
    }
    net_close(&N);
}

int main(void)
{
    RUN(test_lora_clean_link_one_round);
    RUN(test_wifi_clean_link_one_round);
    RUN(test_lora_loss_sweep_multihop);
    RUN(test_wifi_heavy_loss);
    RUN(test_manifest_lost);
    RUN(test_ack_lost);
    RUN(test_corruption_caught_by_crc16);
    RUN(test_sd_fault_caught_by_crc32);
    RUN(test_radio_selection);
    RUN(test_radio_selection_for_size);
    RUN(test_outbox_size_based_routing);
    RUN(test_never_switch_mid_transfer);
    RUN(test_store_and_forward);
    RUN(test_two_senders_at_once);
    RUN(test_abandoned_session_expires);
    RUN(test_edge_sizes);
    RUN(test_outbox_survives_reboot);
    RUN(test_metadata_delivered);
    return SUMMARY();
}
