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

static net_t net; /* large: keep off the stack */

#define BRIDGE 1

static bool bridge_has_one(net_t* p_count)
{
    return p_count->node[BRIDGE].n_rx >= 1;
}

static bool sender2_done(net_t* p_count)
{
    return p_count->node[2].n_tx >= 1;
}

static bool sender2_sent_twice(net_t* p_count)
{
    return p_count->node[2].n_tx >= 2;
}

static void setup(int topo, int radios, uint8_t nodes, bool b_outbox, uint32_t seed)
{
    sim_cfg_t cfg;
    sim_default_cfg(&cfg, nodes);
    cfg.seed = seed;
    if (0 == topo)
    {
        sim_topology_full(&cfg, -1);
    }
    else if (1 == topo)
    {
        sim_topology_chain(&cfg, -1);
    }
    else
    {
        sim_topology_trail(&cfg, -1);
    }
    net_init(&net, &cfg, "sim_out/test", radios, b_outbox);
    net_run_ms(&net, 200); /* let the first heartbeats go out */
}

static bool start_send(uint8_t node, const mesh_radio_t* p_radio, const char* p_path, uint32_t size, uint32_t seed)
{
    net_make_file(&net, node, p_path, size, seed);
    sim_set_current(node);
    return ALFP_OK == alfp_send_file(&net.node[node].alfp, p_radio, BRIDGE, p_path, NULL, 0);
}

static bool received_equals(uint8_t node, const char* p_path, unsigned idx)
{
    net_node_t* p_bridge = &net.node[BRIDGE];
    if (idx >= p_bridge->n_rx)
    {
        return false;
    }
    return net_files_equal(&net, node, p_path, BRIDGE, p_bridge->rx_paths[idx]);
}

/* Files ending in suffix under <node>/<dir>, one level of subfolders included (the bridge's inbox
 * is inbox/<xx>/...). */
static unsigned count_in(const char* p_path, const char* p_suffix, int depth)
{
    DIR* p_dir_handle = opendir(p_path);
    unsigned count_c = 0;
    if (!p_dir_handle)
    {
        return 0;
    }
    struct dirent* p_entry;
    while ((p_entry = readdir(p_dir_handle)))
    {
        if ('.' == p_entry->d_name[0])
        {
            continue;
        }
        size_t name_len = strlen(p_entry->d_name), suffix_len = strlen(p_suffix);
        if ((name_len > suffix_len) && (0 == strcmp(p_entry->d_name + name_len - suffix_len, p_suffix)))
        {
            count_c++;
        }
        else if (depth > 0)
        {
            char sub[700];
            snprintf(sub, sizeof(sub), "%s/%s", p_path, p_entry->d_name);
            count_c += count_in(sub, p_suffix, depth - 1);
        }
        else
        {
            /* not a match and no depth left: skip it */
        }
    }
    closedir(p_dir_handle);
    return count_c;
}

static unsigned count_files(uint8_t node, const char* p_dir, const char* p_suffix)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/node%u/%s", net.root, (unsigned)node, p_dir);
    return count_in(path, p_suffix, 1);
}

static void no_leaks(void)
{
    for (uint8_t idx = 1; idx <= net.cfg.n_nodes; idx++)
    {
        CHECK(net.node[idx].posix.open_count == 0 || (idx == BRIDGE && net.node[idx].posix.open_count <= ALFP_RX_SESSIONS));
    }
}

/* ---------------------------------------------------------------------------------------- */

static void test_lora_clean_link_one_round(void)
{
    setup(0, 1, 2, false, 1);
    CHECK(start_send(2, &g_sim_lora, "img/a.jpg", 10000, 1));
    CHECK(net_run_until(&net, sender2_done, 120000));
    alfp_tx_report_t* p_rep = &net.node[2].tx_reports[0];
    CHECK(p_rep->result == ALFP_TX_DONE);
    /* Even with no random loss, the bridge's 3 s heartbeat can collide with a fragment (LoRa has
     * no carrier sense), so allow one repair round. */
    CHECK(p_rep->rounds <= 2);
    CHECK(p_rep->frag_count == 42 && p_rep->frag_size == 241); /* 250 - 9 */
    CHECK(received_equals(2, "img/a.jpg", 0));
    printf("    LoRa 10 kB, 1 hop, no loss: %.1f s, %u packets\n", (p_rep->finished_ms - p_rep->started_ms) / 1000.0,
           p_rep->data_sent + p_rep->manifests_sent + p_rep->polls_sent);
    no_leaks();
    net_close(&net);
}

static void test_wifi_clean_link_one_round(void)
{
    setup(0, 2, 2, false, 2);
    CHECK(start_send(2, &g_sim_wifi, "img/a.jpg", 10000, 2));
    CHECK(net_run_until(&net, sender2_done, 60000));
    alfp_tx_report_t* p_rep = &net.node[2].tx_reports[0];
    CHECK(p_rep->result == ALFP_TX_DONE && p_rep->rounds == 1);
    CHECK(p_rep->frag_size == 236); /* 245 - 9 */
    CHECK(received_equals(2, "img/a.jpg", 0));
    printf("    WiFi 10 kB, 1 hop, no loss: %.2f s\n", (p_rep->finished_ms - p_rep->started_ms) / 1000.0);
    net_close(&net);
}

static void test_lora_loss_sweep_multihop(void)
{
    const float losses[] = {0.1f, 0.2f, 0.3f};
    for (unsigned idx = 0; idx < 3; idx++)
    {
        setup(1, 1, 3, false, 10 + idx); /* chain: 3 -> 2 -> 1, two hops */
        sim_set_link_loss(SIM_LORA, losses[idx]);
        CHECK(start_send(3, &g_sim_lora, "img/b.jpg", 8000, 3));
        bool b_fin = net_run_until(&net, bridge_has_one, 600000);
        net_run_ms(&net, 10000);
        CHECK(b_fin);
        CHECK(net.node[3].n_tx == 1 && net.node[3].tx_reports[0].result == ALFP_TX_DONE);
        CHECK(received_equals(3, "img/b.jpg", 0));
        alfp_tx_report_t* p_rep = &net.node[3].tx_reports[0];
        printf("    LoRa 2 hops, %2.0f%% loss/hop: %u rounds, %u resent of %u, %.1f s\n", losses[idx] * 100, p_rep->rounds,
               p_rep->data_resent, p_rep->frag_count, (p_rep->finished_ms - p_rep->started_ms) / 1000.0);
        CHECK(p_rep->rounds >= 2); /* loss really happened and was repaired */
        net_close(&net);
    }
}

static void test_wifi_heavy_loss(void)
{
    setup(2, 2, 5, false, 20); /* trail with redundant links */
    sim_set_link_loss(SIM_WIFI, 0.35f);
    CHECK(start_send(5, &g_sim_wifi, "img/c.jpg", 20000, 4));
    CHECK(net_run_until(&net, bridge_has_one, 300000));
    net_run_ms(&net, 3000);
    CHECK(net.node[5].n_tx == 1 && net.node[5].tx_reports[0].result == ALFP_TX_DONE);
    CHECK(received_equals(5, "img/c.jpg", 0));
    net_close(&net);
}

/* Bridge deaf for the first 150 ms: the manifest (and maybe early data) is lost.
 * The bridge must open the session from DATA, then ask for the manifest in its NACK. */
static void test_manifest_lost(void)
{
    setup(0, 1, 2, false, 30);
    sim_set_drop_rate(BRIDGE, SIM_LORA, 1.0f);
    CHECK(start_send(2, &g_sim_lora, "img/d.jpg", 5000, 5));
    net_run_ms(&net, 150);
    sim_set_drop_rate(BRIDGE, SIM_LORA, 0.0f);
    CHECK(net_run_until(&net, sender2_done, 120000));
    alfp_tx_report_t* p_rep = &net.node[2].tx_reports[0];
    CHECK(p_rep->result == ALFP_TX_DONE);
    CHECK(p_rep->manifests_sent >= 2);
    CHECK(net.node[BRIDGE].alfp.stats.data_orphan >= 1);
    CHECK(received_equals(2, "img/d.jpg", 0));
    net_close(&net);
}

/* The bridge's ACK is lost. The sender polls; the bridge remembers the finished session and
 * ACKs again instead of starting over. Exactly one file must appear on the bridge. */
static void test_ack_lost(void)
{
    setup(0, 1, 2, false, 40);
    CHECK(start_send(2, &g_sim_lora, "img/e.jpg", 4000, 6));
    CHECK(net_run_until(&net, bridge_has_one, 120000));
    sim_set_drop_rate(2, SIM_LORA, 1.0f); /* sender goes deaf right as the ACK is sent */
    net_run_ms(&net, 4000);
    sim_set_drop_rate(2, SIM_LORA, 0.0f);
    CHECK(net_run_until(&net, sender2_done, 60000));
    CHECK(net.node[2].tx_reports[0].result == ALFP_TX_DONE);
    CHECK(net.node[2].tx_reports[0].polls_sent >= 2);
    net_run_ms(&net, 5000);
    CHECK(net.node[BRIDGE].n_rx == 1);
    CHECK(count_files(BRIDGE, "inbox", ".bin") == 1);
    net_close(&net);
}

/* Bits flipped AFTER the radio's own CRC (UART, SPI or a relay's buffer). CRC-16 must catch them. */
static void test_corruption_caught_by_crc16(void)
{
    setup(0, 1, 2, false, 50);
    sim_set_corrupt_rate(SIM_LORA, 0.1f);
    CHECK(start_send(2, &g_sim_lora, "img/f.jpg", 10000, 7));
    CHECK(net_run_until(&net, sender2_done, 300000));
    CHECK(net.node[2].tx_reports[0].result == ALFP_TX_DONE);
    CHECK(net.node[BRIDGE].alfp.stats.pkts_bad_crc > 0);
    CHECK(received_equals(2, "img/f.jpg", 0));
    printf("    %u corrupted packets rejected by CRC-16, file still exact\n",
           (unsigned)net.node[BRIDGE].alfp.stats.pkts_bad_crc);
    net_close(&net);
}

/* The bridge's SD card corrupts a write. Each fragment passed CRC-16, but the file CRC-32
 * catches it; the sender resends and the final file is still exact. */
static void test_sd_fault_caught_by_crc32(void)
{
    setup(0, 2, 2, false, 60);
    net.node[BRIDGE].posix.fault_write_number = net.node[BRIDGE].posix.writes + 10; /* the 10th fragment written */
    CHECK(start_send(2, &g_sim_wifi, "img/g.jpg", 10000, 8));
    CHECK(net_run_until(&net, sender2_done, 120000));
    CHECK(net.node[2].tx_reports[0].result == ALFP_TX_DONE);
    CHECK(net.node[BRIDGE].alfp.stats.rx_crc_fail >= 1);
    CHECK(received_equals(2, "img/g.jpg", 0));
    printf("    %u file CRC-32 mismatches caught and repaired\n", (unsigned)net.node[BRIDGE].alfp.stats.rx_crc_fail);
    net_close(&net);
}

/* WiFi preferred when the bridge is heard on it; LoRa when WiFi cannot reach. */
static void test_radio_selection(void)
{
    const mesh_radio_t* pref[2] = {&g_sim_wifi, &g_sim_lora};
    setup(0, 3, 2, false, 70);
    net_run_ms(&net, 3500);
    sim_set_current(2);
    CHECK(alfp_select_radio(pref, 2, BRIDGE, sim_now(), 10000) == &g_sim_wifi);
    sim_set_link(SIM_WIFI, 1, 2, false);
    net_run_ms(&net, 12000); /* WiFi heartbeat now older than 10 s */
    sim_set_current(2);
    CHECK(alfp_select_radio(pref, 2, BRIDGE, sim_now(), 10000) == &g_sim_lora);
    sim_set_link(SIM_LORA, 1, 2, false);
    net_run_ms(&net, 12000);
    sim_set_current(2);
    CHECK(alfp_select_radio(pref, 2, BRIDGE, sim_now(), 10000) == NULL);
    net_close(&net);
}

/* alfp_select_radio_for_size(): pure function, exercised directly against two fake radios so
 * every case (sizes, freshness combinations, now_ms wraparound) is fast and exact, with no
 * network simulation needed. alfp_select_radio() itself is untouched (see test_radio_selection
 * above, still passing unmodified). */
static uint32_t g_fsz_lora_heard;
static uint32_t g_fsz_wifi_heard;
static bool gb_fsz_have_lora;
static bool gb_fsz_have_wifi;

static bool fsz_last_heard_lora(uint8_t node_id, uint32_t* p_msec)
{
    (void)node_id;
    if (!gb_fsz_have_lora)
    {
        return false;
    }
    *p_msec = g_fsz_lora_heard;
    return true;
}

static bool fsz_last_heard_wifi(uint8_t node_id, uint32_t* p_msec)
{
    (void)node_id;
    if (!gb_fsz_have_wifi)
    {
        return false;
    }
    *p_msec = g_fsz_wifi_heard;
    return true;
}

static mesh_radio_t fsz_lora = {.p_name = "lora", .p_last_heard = fsz_last_heard_lora};
static mesh_radio_t fsz_wifi = {.p_name = "wifi", .p_last_heard = fsz_last_heard_wifi};

static void test_radio_selection_for_size(void)
{
    const mesh_radio_t* pref[2] = {&fsz_lora, &fsz_wifi};
    const uint32_t fresh = 10000u;
    const uint32_t thr = 1024u;
    const uint32_t now_ms = 50000u;

    /* Both fresh: below threshold picks LoRa, at/above picks WiFi. Boundary and max-size cases. */
    gb_fsz_have_lora = true;
    g_fsz_lora_heard = now_ms - 1000u;
    gb_fsz_have_wifi = true;
    g_fsz_wifi_heard = now_ms - 1000u;
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, now_ms, fresh, 0u, thr) == &fsz_lora);
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, now_ms, fresh, 1u, thr) == &fsz_lora);
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, now_ms, fresh, thr - 1u, thr) == &fsz_lora);
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, now_ms, fresh, thr, thr) == &fsz_wifi);
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, now_ms, fresh, thr + 1u, thr) == &fsz_wifi);
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, now_ms, fresh, 236u * ALFP_MAX_FRAGS, thr) == &fsz_wifi);
    CHECK(alfp_radio_was_size_preferred(pref, 2, 0u, thr, &fsz_lora));
    CHECK(!alfp_radio_was_size_preferred(pref, 2, 0u, thr, &fsz_wifi));
    CHECK(alfp_radio_was_size_preferred(pref, 2, thr, thr, &fsz_wifi));

    /* Preferred radio stale: falls back to the other one. */
    g_fsz_wifi_heard = now_ms - fresh - 1u; /* WiFi just over fresh_ms old */
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, now_ms, fresh, thr, thr) == &fsz_lora);
    CHECK(!alfp_radio_was_size_preferred(pref, 2, thr, thr, &fsz_lora)); /* that pick was a fallback, not size */
    g_fsz_wifi_heard = now_ms - 1000u; /* wifi fresh again */
    g_fsz_lora_heard = now_ms - fresh - 1u;
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, now_ms, fresh, 0u, thr) == &fsz_wifi);

    /* Both stale, or never heard at all: NULL regardless of size. */
    g_fsz_wifi_heard = now_ms - fresh - 1u;
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, now_ms, fresh, 0u, thr) == NULL);
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, now_ms, fresh, thr, thr) == NULL);
    gb_fsz_have_lora = false;
    gb_fsz_have_wifi = false;
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, now_ms, fresh, 0u, thr) == NULL);

    /* now_ms wraparound: heard 16 ms before the 32-bit wrap, "now" 16 ms after it (32 ms elapsed).
     * Must use the same unsigned-subtraction math alfp_select_radio() already relies on. */
    gb_fsz_have_lora = true;
    g_fsz_lora_heard = 0xFFFFFFF0u;
    const uint32_t wrapped_now = 0x00000010u;
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, wrapped_now, fresh, 0u, thr) == &fsz_lora);
    CHECK(alfp_select_radio_for_size(pref, 2, BRIDGE, wrapped_now, 20u, 0u, thr) == NULL); /* 32 ms > 20 ms fresh */
}

/* Outbox-level proof that the size rule actually drives radio choice end to end, through the
 * real network simulator: a small job goes over LoRa, a large one over WiFi, with the threshold
 * turned on via alfp_outbox_set_size_threshold() (default after alfp_outbox_init() is 0 = off,
 * proven byte-identical-to-before by every other test in this file still passing unmodified). */
static void test_outbox_size_based_routing(void)
{
    setup(0, 3, 2, true, 200);
    net_run_ms(&net, 3500); /* both links fresh */
    alfp_outbox_set_size_threshold(&net.node[2].outbox, 1024);
    net_make_file(&net, 2, "img/small.bin", 200, 201);
    net_make_file(&net, 2, "img/large.bin", 20000, 202);

    sim_set_current(2);
    CHECK(alfp_outbox_add(&net.node[2].outbox, "img/small.bin", NULL, 0));
    net_run_ms(&net, 50); /* outbox has started the small transfer */
    CHECK(alfp_tx_active(&net.node[2].alfp));
    CHECK(net.node[2].alfp.tx_session.p_radio != NULL && strcmp(net.node[2].alfp.tx_session.p_radio->p_name, "lora") == 0);
    CHECK(net_run_until(&net, sender2_done, 120000));
    CHECK(strcmp(net.node[2].tx_reports[0].p_radio, "lora") == 0);
    CHECK(net.node[2].tx_reports[0].result == ALFP_TX_DONE);

    sim_set_current(2);
    CHECK(alfp_outbox_add(&net.node[2].outbox, "img/large.bin", NULL, 0));
    net_run_ms(&net, 50);
    CHECK(alfp_tx_active(&net.node[2].alfp));
    CHECK(net.node[2].alfp.tx_session.p_radio != NULL && strcmp(net.node[2].alfp.tx_session.p_radio->p_name, "wifi") == 0);
    CHECK(net_run_until(&net, sender2_sent_twice, 120000));
    CHECK(strcmp(net.node[2].tx_reports[1].p_radio, "wifi") == 0);
    CHECK(net.node[2].tx_reports[1].result == ALFP_TX_DONE);
    net_close(&net);
}

/* WiFi breaks in the middle of a transfer. The transfer must NOT switch to LoRa: it finishes
 * or fails on WiFi. The outbox's next attempt is a new transfer, which may pick LoRa. */
static void test_never_switch_mid_transfer(void)
{
    setup(0, 3, 2, true, 80);
    net_run_ms(&net, 3500);
    net_make_file(&net, 2, "img/h.jpg", 20000, 9);
    sim_set_current(2);
    CHECK(alfp_outbox_add(&net.node[2].outbox, "img/h.jpg", NULL, 0));
    net_run_ms(&net, 150); /* transfer has started on WiFi */
    CHECK(alfp_tx_active(&net.node[2].alfp) && net.node[2].alfp.tx_session.p_radio == &g_sim_wifi);
    sim_set_link(SIM_WIFI, 1, 2, false); /* WiFi dies mid-transfer */
    CHECK(net_run_until(&net, bridge_has_one, 400000));
    net_run_ms(&net, 5000);
    net_node_t* p_src_node = &net.node[2];
    CHECK(p_src_node->n_tx >= 2);
    CHECK(strcmp(p_src_node->tx_reports[0].p_radio, "wifi") == 0 && p_src_node->tx_reports[0].result == ALFP_TX_GAVE_UP);
    CHECK(strcmp(p_src_node->tx_reports[p_src_node->n_tx - 1].p_radio, "lora") == 0 &&
          p_src_node->tx_reports[p_src_node->n_tx - 1].result == ALFP_TX_DONE);
    CHECK(received_equals(2, "img/h.jpg", 0));
    CHECK(alfp_outbox_count(&p_src_node->outbox) == 0);
    net_close(&net);
}

/* No radio can reach the bridge: the capture waits on SD, then goes as soon as a link appears. */
static void test_store_and_forward(void)
{
    sim_cfg_t cfg;
    sim_default_cfg(&cfg, 3);
    cfg.seed = 90; /* no links at all */
    net_init(&net, &cfg, "sim_out/test", 3, true);
    capture_meta_t meta;
    capture_meta_clear(&meta);
    meta.node_id = 3;
    meta.capture_id = 77;
    meta.temp_c_x10 = 291;
    uint8_t meta_buf[CAPTURE_META_LEN];
    capture_meta_pack(&meta, meta_buf);
    for (int idx = 0; idx < 3; idx++)
    {
        char path[32];
        snprintf(path, sizeof(path), "img/s%d.jpg", idx);
        net_make_file(&net, 3, path, 3000 + 500 * (uint32_t)idx, 20 + (uint32_t)idx);
        sim_set_current(3);
        CHECK(alfp_outbox_add(&net.node[3].outbox, path, meta_buf, sizeof(meta_buf)));
    }
    net_run_ms(&net, 20000);
    CHECK(alfp_outbox_count(&net.node[3].outbox) == 3);
    CHECK(net.node[3].n_tx == 0 && net.node[3].outbox.no_radio_waits > 0);
    sim_set_link(SIM_LORA, 3, 2, true); /* node 3 comes into range of the relay */
    sim_set_link(SIM_LORA, 2, 1, true);
    for (int kdx = 0; (kdx < 400) && (net.node[BRIDGE].n_rx < 3); kdx++)
    {
        net_run_ms(&net, 1000);
    }
    net_run_ms(&net, 10000); /* let the last ACK reach the sender */
    for (unsigned idx = 0; idx < net.node[3].n_tx; idx++)
    {
        alfp_tx_report_t* p_rep = &net.node[3].tx_reports[idx];
        printf("    attempt %u: %s on %s, %u rounds, %.1f s\n", idx + 1, alfp_tx_result_str(p_rep->result), p_rep->p_radio,
               p_rep->rounds, (p_rep->finished_ms - p_rep->started_ms) / 1000.0);
    }
    CHECK(net.node[BRIDGE].n_rx == 3);
    CHECK(alfp_outbox_count(&net.node[3].outbox) == 0);
    for (unsigned idx = 0; (idx < net.node[BRIDGE].n_rx) && (idx < 3); idx++)
    {
        char path[32];
        snprintf(path, sizeof(path), "img/s%u.jpg", idx);
        CHECK(received_equals(3, path, idx)); /* oldest first */
    }
    CHECK(count_files(BRIDGE, "inbox", ".met") == 3);
    net_close(&net);
}

/* Two field nodes send at the same time; the bridge reassembles both. */
static void test_two_senders_at_once(void)
{
    setup(0, 2, 3, false, 100);
    CHECK(start_send(2, &g_sim_wifi, "img/x.jpg", 12000, 30));
    CHECK(start_send(3, &g_sim_wifi, "img/y.jpg", 9000, 31));
    for (int kdx = 0; (kdx < 120) && (net.node[BRIDGE].n_rx < 2); kdx++)
    {
        net_run_ms(&net, 500);
    }
    CHECK(net.node[BRIDGE].n_rx == 2);
    bool b_is_ok = true;
    for (unsigned idx = 0; idx < net.node[BRIDGE].n_rx; idx++)
    {
        uint8_t src = net.node[BRIDGE].rx_reports[idx].src;
        b_is_ok &= received_equals(src, 2 == src ? "img/x.jpg" : "img/y.jpg", idx);
    }
    CHECK(b_is_ok);
    net_close(&net);
}

/* The sender vanishes mid-transfer: the bridge must free the session and delete the .part file. */
static void test_abandoned_session_expires(void)
{
    setup(0, 2, 2, false, 110);
    CHECK(start_send(2, &g_sim_wifi, "img/z.jpg", 20000, 32));
    net_run_ms(&net, 300);
    net.node[2].n_radios = 0; /* node 2 loses power */
    sim_set_link(SIM_WIFI, 1, 2, false);
    CHECK(count_files(BRIDGE, "rx", ".part") == 1);
    net_run_ms(&net, 17000); /* WiFi rx_expiry is 15 s */
    CHECK(net.node[BRIDGE].alfp.stats.rx_expired == 1);
    CHECK(count_files(BRIDGE, "rx", ".part") == 0);
    CHECK(net.node[BRIDGE].posix.open_count == 0);
    net_close(&net);
}

static void test_edge_sizes(void)
{
    const uint32_t sizes[] = {1, 236, 237, 236 * 3, 236u * ALFP_MAX_FRAGS};
    for (unsigned idx = 0; idx < 5; idx++)
    {
        setup(0, 2, 2, false, 120 + idx);
        CHECK(start_send(2, &g_sim_wifi, "img/e.bin", sizes[idx], 40 + idx));
        CHECK(net_run_until(&net, sender2_done, 600000));
        CHECK(net.node[2].tx_reports[0].result == ALFP_TX_DONE);
        CHECK(received_equals(2, "img/e.bin", 0));
        net_close(&net);
    }
    setup(0, 2, 2, false, 130);
    net_make_file(&net, 2, "img/big.bin", 236u * ALFP_MAX_FRAGS + 1u, 50);
    sim_set_current(2);
    CHECK(alfp_send_file(&net.node[2].alfp, &g_sim_wifi, BRIDGE, "img/big.bin", NULL, 0) == ALFP_E_TOO_BIG);
    net_make_file(&net, 2, "img/empty.bin", 0, 51);
    CHECK(alfp_send_file(&net.node[2].alfp, &g_sim_wifi, BRIDGE, "img/empty.bin", NULL, 0) == ALFP_E_EMPTY);
    CHECK(alfp_send_file(&net.node[2].alfp, &g_sim_wifi, BRIDGE, "img/missing.bin", NULL, 0) == ALFP_E_OPEN);
    CHECK(net.node[2].posix.open_count == 0);
    net_close(&net);
}

/* Queue survives a reboot: job files are re-read from SD. */
static void test_outbox_survives_reboot(void)
{
    setup(0, 1, 2, true, 140);
    sim_set_link(SIM_LORA, 1, 2, false);
    for (int idx = 0; idx < 4; idx++)
    {
        net_make_file(&net, 2, "img/r.jpg", 1000, 60);
        sim_set_current(2);
        CHECK(alfp_outbox_add(&net.node[2].outbox, "img/r.jpg", NULL, 0));
    }
    alfp_outbox_t again;
    sim_set_current(2);
    alfp_outbox_init(&again, &net.node[2].alfp, net.node[2].radios, net.node[2].n_radios, BRIDGE, 10000, "outbox");
    CHECK(alfp_outbox_count(&again) == 4);
    CHECK(again.next_seq == net.node[2].outbox.next_seq);
    net_close(&net);
}

/* F9 + F3: a bridge reboot deletes the .part files transfers cut off by the reboot left behind,
 * and a session id that repeats after the reboot never overwrites a file already in the inbox. */
static void test_bridge_reboot_parts_and_names(void)
{
    setup(0, 2, 2, false, 150);
    CHECK(start_send(2, &g_sim_wifi, "img/f3a.jpg", 5000, 70));
    CHECK(net_run_until(&net, sender2_done, 60000));
    CHECK(net.node[BRIDGE].n_rx == 1);
    uint16_t sid = net.node[2].tx_reports[0].sid;

    /* a transfer the "reboot" will cut short */
    CHECK(start_send(2, &g_sim_wifi, "img/cut.jpg", 20000, 71));
    net_run_ms(&net, 300);
    CHECK(count_files(BRIDGE, "rx", ".part") == 1);
    net.node[2].n_radios = 0;
    sim_set_link(SIM_WIFI, 1, 2, false);

    /* reboot the bridge: a fresh alfp_init with the same settings (its done-list is forgotten) */
    for (unsigned idx = 0; idx < ALFP_RX_SESSIONS; idx++)
    {
        if (net.node[BRIDGE].alfp.rx_sessions[idx].file_handle >= 0)
        {
            net.node[BRIDGE].store.p_close(net.node[BRIDGE].store.p_ctx, net.node[BRIDGE].alfp.rx_sessions[idx].file_handle);
        }
    }
    alfp_cfg_t cfg = net.node[BRIDGE].alfp.cfg;
    sim_set_current(BRIDGE);
    alfp_init(&net.node[BRIDGE].alfp, &cfg);
    CHECK(count_files(BRIDGE, "rx", ".part") == 0);

    /* node 2 "reboots" too and draws the same session id again */
    if (net.node[2].alfp.tx_session.file_handle >= 0)
    {
        net.node[2].store.p_close(net.node[2].store.p_ctx, net.node[2].alfp.tx_session.file_handle);
    }
    alfp_cfg_t cfg2 = net.node[2].alfp.cfg;
    sim_set_current(2);
    alfp_init(&net.node[2].alfp, &cfg2);
    net.node[2].n_radios = 2;
    sim_set_link(SIM_WIFI, 1, 2, true);
    net.node[2].alfp.next_sid = sid;
    CHECK(start_send(2, &g_sim_wifi, "img/f3b.jpg", 6000, 72));
    CHECK(net_run_until(&net, sender2_sent_twice, 60000));
    CHECK(net.node[2].n_tx == 2 && net.node[2].tx_reports[1].result == ALFP_TX_DONE);
    CHECK(net.node[2].tx_reports[1].sid == sid);
    CHECK(net.node[BRIDGE].n_rx == 2);
    CHECK(strcmp(net.node[BRIDGE].rx_paths[0], net.node[BRIDGE].rx_paths[1]) != 0);
    CHECK(received_equals(2, "img/f3a.jpg", 0)); /* the first file is still intact */
    CHECK(received_equals(2, "img/f3b.jpg", 1));
    CHECK(count_files(BRIDGE, "inbox", ".bin") == 2);
    char want[32];
    snprintf(want, sizeof(want), "inbox/%02x/n2_%05u", (unsigned)(sid >> 8), (unsigned)sid);
    CHECK(strncmp(net.node[BRIDGE].rx_paths[0], want, strlen(want)) == 0); /* sharded by sid >> 8 */
    net_close(&net);
}

/* Metadata goes through intact and lands next to the image on the bridge. */
static void test_metadata_delivered(void)
{
    setup(0, 2, 2, false, 150);
    capture_meta_t meta, back;
    capture_meta_clear(&meta);
    meta.node_id = 2;
    meta.capture_id = 4242;
    meta.humidity_x10 = 903;
    meta.weather_label = 3;
    meta.sound_class = 5;
    uint8_t meta_buf[CAPTURE_META_LEN];
    capture_meta_pack(&meta, meta_buf);
    net_make_file(&net, 2, "img/m.jpg", 3000, 70);
    sim_set_current(2);
    CHECK(alfp_send_file(&net.node[2].alfp, &g_sim_wifi, BRIDGE, "img/m.jpg", meta_buf, sizeof(meta_buf)) == ALFP_OK);
    CHECK(net_run_until(&net, sender2_done, 60000));
    char path[600];
    snprintf(path, sizeof(path), "%s/node1/%s", net.root, net.node[BRIDGE].rx_paths[0]);
    char* p_dot = strrchr(path, '.');
    strcpy(p_dot, ".met");
    FILE* p_file = fopen(path, "rb");
    CHECK(p_file != NULL);
    if (p_file)
    {
        uint8_t input[64];
        size_t len = fread(input, 1, sizeof(input), p_file);
        fclose(p_file);
        CHECK(capture_meta_unpack(input, (uint8_t)len, &back) == 0);
        CHECK(back.capture_id == 4242 && back.humidity_x10 == 903 && back.weather_label == 3 && back.sound_class == 5);
    }
    net_close(&net);
}

/* The outbox's prepare_meta hook runs just before alfp_send_file(): what it writes (here queued_ms
 * of a v2 record) is what the bridge receives, while the job on the SD card keeps the original. */
static unsigned g_stamped;

static void stamp_meta(uint8_t* p_meta, uint8_t len, void* p_user)
{
    (void)p_user;
    if ((len >= CAPTURE_META_LEN_V2) && (2u == p_meta[0]))
    {
        cm_put32(p_meta + CAPTURE_META_QUEUED_OFF, 2600u);
        g_stamped++;
    }
}

static void test_outbox_prepare_meta(void)
{
    setup(0, 2, 2, true, 170);
    capture_meta_t meta, back;
    capture_meta_clear(&meta);
    meta.node_id = 2;
    meta.capture_id = 77;
    uint8_t meta_buf[CAPTURE_META_LEN + 6];
    uint8_t meta_len = capture_meta_pack(&meta, meta_buf);
    memcpy(meta_buf + meta_len, "x.jpg", 5);
    meta_len = (uint8_t)(meta_len + 5u);
    g_stamped = 0;
    net.node[2].outbox.p_prepare_meta = stamp_meta;
    net_make_file(&net, 2, "img/q.jpg", 2000, 71);
    sim_set_current(2);
    CHECK(alfp_outbox_add(&net.node[2].outbox, "img/q.jpg", meta_buf, meta_len));
    CHECK(net_run_until(&net, sender2_done, 60000));
    CHECK(g_stamped == 1);
    char path[600];
    snprintf(path, sizeof(path), "%s/node1/%s", net.root, net.node[BRIDGE].rx_paths[0]);
    strcpy(strrchr(path, '.'), ".met");
    FILE* p_file = fopen(path, "rb");
    CHECK(p_file != NULL);
    if (p_file)
    {
        uint8_t input[64];
        size_t len = fread(input, 1, sizeof(input), p_file);
        fclose(p_file);
        CHECK(len == meta_len && capture_meta_unpack(input, (uint8_t)len, &back) == 0);
        CHECK(back.capture_id == 77 && back.queued_ms == 2600u);
        CHECK(capture_meta_record_len(input, (uint8_t)len) == CAPTURE_META_LEN_V2 && memcmp(input + 31, "x.jpg", 5) == 0);
    }
    net_close(&net);
}

/* ---- progress callbacks (TRANSFER_VISIBILITY.md 3.2) ------------------------------------- */

typedef struct
{
    char kind; /* S start, M manifest, F frag, N nack, X fail, T tx start, K tx nack */
    uint16_t sid, seq, count;
    uint8_t round;
    bool b_dup;
} ev_t;

static ev_t g_ev[512];
static unsigned g_nev;

static void ev_push(char kind, uint16_t sid, uint16_t seq, uint16_t count, uint8_t round, bool b_dup)
{
    if (g_nev < sizeof(g_ev) / sizeof(g_ev[0]))
    {
        g_ev[g_nev++] = (ev_t){kind, sid, seq, count, round, b_dup};
    }
}

static void cb_rx_start(alfp_ctx_t* p_ctx, uint8_t src, uint16_t sid, const char* p_radio)
{
    (void)p_ctx;
    CHECK(src == 2 && strcmp(p_radio, "wifi") == 0);
    ev_push('S', sid, 0, 0, 0, false);
}

static void cb_rx_manifest(alfp_ctx_t* p_ctx, uint8_t src, uint16_t sid, uint16_t frag_count, uint16_t frag_size,
                           uint32_t file_size, uint32_t file_crc32, const uint8_t* p_meta, uint8_t meta_len)
{
    (void)p_ctx;
    (void)src;
    (void)frag_size;
    (void)file_crc32;
    (void)p_meta;
    CHECK(file_size == 6000 && meta_len == 0);
    ev_push('M', sid, 0, frag_count, 0, false);
}

static void cb_rx_frag(alfp_ctx_t* p_ctx, uint8_t src, uint16_t sid, uint16_t seq, uint8_t round, bool b_dup)
{
    (void)p_ctx;
    (void)src;
    ev_push('F', sid, seq, 0, round, b_dup);
}

static void cb_rx_nack(alfp_ctx_t* p_ctx, uint8_t src, uint16_t sid, uint8_t round, uint16_t missing)
{
    (void)p_ctx;
    (void)src;
    ev_push('N', sid, 0, missing, round, false);
}

static void cb_rx_fail(alfp_ctx_t* p_ctx, uint8_t src, uint16_t sid, const char* p_why)
{
    (void)p_ctx;
    (void)src;
    (void)p_why;
    ev_push('X', sid, 0, 0, 0, false);
}

static void cb_tx_start(alfp_ctx_t* p_ctx, const alfp_tx_t* p_tx_session)
{
    (void)p_ctx;
    ev_push('T', p_tx_session->sid, 0, p_tx_session->frag_count, 0, false);
}

static void cb_tx_nack(alfp_ctx_t* p_ctx, uint16_t sid, uint8_t round, uint16_t missing)
{
    (void)p_ctx;
    ev_push('K', sid, 0, missing, round, false);
}

static bool bridge_heard_10_frags(net_t* p_count)
{
    (void)p_count;
    unsigned frags = 0;
    for (unsigned idx = 0; idx < g_nev; idx++)
    {
        frags += 'F' == g_ev[idx].kind;
    }
    return frags >= 10;
}

/* The bridge goes deaf for exactly two DATA packets in the middle of the transfer. The callbacks
 * must report: start -> manifest -> first-try fragments (round 0) -> NACK -> the two resent
 * fragments (round 1; tail_dup sends each twice, so the second copy is a duplicate) -> done. */
static void test_progress_callbacks(void)
{
    setup(0, 2, 2, false, 160);
    g_nev = 0;
    alfp_cfg_t* p_cfg_b = &net.node[BRIDGE].alfp.cfg;
    p_cfg_b->p_on_rx_start = cb_rx_start;
    p_cfg_b->p_on_rx_manifest = cb_rx_manifest;
    p_cfg_b->p_on_rx_frag = cb_rx_frag;
    p_cfg_b->p_on_rx_nack = cb_rx_nack;
    p_cfg_b->p_on_rx_fail = cb_rx_fail;
    net.node[2].alfp.cfg.p_on_tx_start = cb_tx_start;
    net.node[2].alfp.cfg.p_on_tx_nack = cb_tx_nack;

    CHECK(start_send(2, &g_sim_wifi, "img/ev.jpg", 6000, 90));
    CHECK(net_run_until(&net, bridge_heard_10_frags, 60000));
    sim_set_drop_rate(BRIDGE, SIM_WIFI, 1.0f);
    uint16_t sent0 = net.node[2].alfp.tx_session.rep.data_sent;
    for (int idx = 0; (idx < 5000) && (net.node[2].alfp.tx_session.rep.data_sent < sent0 + 2u); idx++)
    {
        net_run_ms(&net, 1);
    }
    /* until the second one has really left the sender's radio (a third may be lost too) */
    for (int idx = 0; idx < 5000; idx++)
    {
        sim_set_current(2);
        if ((net.node[2].alfp.tx_session.rep.data_sent >= sent0 + 3u) || (!g_sim_wifi.p_tx_busy()))
        {
            break;
        }
        net_run_ms(&net, 1);
    }
    net_run_ms(&net, 2);
    sim_set_drop_rate(BRIDGE, SIM_WIFI, 0.0f);
    CHECK(net_run_until(&net, sender2_done, 60000));
    CHECK(net.node[2].tx_reports[0].result == ALFP_TX_DONE);
    CHECK(received_equals(2, "img/ev.jpg", 0));

    /* order and content */
    CHECK(g_nev > 0 && g_ev[0].kind == 'T');
    int first_s = -1, first_m = -1, first_n = -1, n_fail = 0;
    unsigned frags_r0 = 0, frags_r1 = 0, dups_r1 = 0, tx_nacks = 0, n_nacks = 0;
    for (unsigned idx = 0; idx < g_nev; idx++)
    {
        const ev_t* p_entry = &g_ev[idx];
        if (('S' == p_entry->kind) && (first_s < 0))
        {
            first_s = (int)idx;
        }
        if (('M' == p_entry->kind) && (first_m < 0))
        {
            first_m = (int)idx;
        }
        if ('N' == p_entry->kind)
        {
            n_nacks++;
            if (first_n < 0)
            {
                first_n = (int)idx;
                CHECK(p_entry->round == 1 && p_entry->count >= 2 && p_entry->count <= 4);
                if (!((p_entry->count >= 2) && (p_entry->count <= 4)))
                {
                    printf("    first NACK asked for %u\n", p_entry->count);
                }
            }
        }
        if ('F' == p_entry->kind)
        {
            CHECK(first_s >= 0); /* never a fragment before the session start */
            if (0 == p_entry->round)
            {
                CHECK(first_n < 0); /* round 0 only before the first NACK */
                CHECK(!p_entry->b_dup);
                frags_r0++;
            }
            else
            {
                CHECK(first_n >= 0 && p_entry->round >= 1);
                frags_r1++;
                dups_r1 += p_entry->b_dup;
            }
        }
        tx_nacks += 'K' == p_entry->kind;
        n_fail += 'X' == p_entry->kind;
    }
    CHECK(first_s >= 0 && first_m > first_s);
    CHECK(first_n > first_m);
    CHECK(n_nacks >= 1 && tx_nacks >= 1);
    CHECK(frags_r1 >= 2);
    CHECK(dups_r1 >= 1);
    CHECK(n_fail == 0);
    CHECK(frags_r0 + frags_r1 - dups_r1 == net.node[2].tx_reports[0].frag_count);
    printf("    events: %u, first-try frags %u, resent %u (%u dup), NACKs %u\n", g_nev, frags_r0, frags_r1, dups_r1,
           n_nacks);
    net_close(&net);
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
    RUN(test_bridge_reboot_parts_and_names);
    RUN(test_progress_callbacks);
    RUN(test_outbox_prepare_meta);
    return SUMMARY();
}
