/*
 * test_filtering.c - CURVEBALL round 3, Task D.2: filtering tests, i.e. packets that must never
 * reach ALFP or never reach this node's application layer at all. Each test is one input, one
 * expected counter change, one assertion -- mesh_stats_t's counters (mesh/mesh_radio.h) are the
 * source of truth, read straight off the simulated radios (host/sim/sim_mesh.c implements the
 * real flood-mesh rules from mesh/mesh_radio.h's contract, the same ones wifi_mesh.c/lora_mesh.c
 * implement on real hardware).
 *
 * Two filtering checks from the brief are NOT in this file, with reasons:
 *   - hop_dropped (a packet needing relay but hops == 0): sim_mesh.c's HOP_LIMIT is 4, and
 *     SIM_MAX_NODES is 6, so the longest chain this simulator can build is 5 hops -- which uses
 *     exactly 4 relay decisions (hops 4->3->2->1->0) and the final hop always lands ON the
 *     destination, which delivers regardless of its own hops field (sim_mesh.c:on_frame: the
 *     dst==n delivery path does not check hops at all, only the dst!=n relay path does). There is
 *     no way to make a non-destination node receive hops==0 within this node cap; verified by
 *     reading sim_mesh.c:on_frame, not asserted here.
 *   - the real WiFi mesh topology filter (radios_set_wifi_topology_filter /
 *     wifi_mesh_set_topology_filter): sim_mesh.c is a generic flood simulator, not Goutham's real
 *     wifi_mesh.c, so it has no concept of this filter at all. host/integration/integ_node.c
 *     already runs the real wifi_mesh.c with the filter on by default (see its own comment at the
 *     wifi_mesh_init() call) as part of run_wifi_integration.sh's multi-hop E->C->A scenario --
 *     that is where this filter is actually exercised, not here. See docs/TEST_REPORT.md.
 */
#include <string.h>

#include "sim_net.h"
#include "testlib.h"

static net_t N;
#define BRIDGE 1

static bool bridge_has_one(net_t* n)
{
    return n->node[BRIDGE].n_rx >= 1;
}

static bool sender2_done(net_t* n)
{
    return n->node[2].n_tx >= 1; /* the sender's own tx_report, populated only after its ACK arrives */
}

/* sim_lora/sim_wifi are singletons shared by every simulated node (host/sim/sim_mesh.h: "real
 * firmware has one radio singleton per physical node"), so reading a specific node's stats needs
 * sim_get_stats(node_id, ...), not radio->get_stats() (which answers for whatever node
 * sim_set_current() last pointed at, not necessarily this one). */
static mesh_stats_t stats_of(uint8_t node, int kind)
{
    mesh_stats_t s;
    sim_get_stats(node, kind, &s);
    return s;
}

/* Duplicate suppression: a trail topology (chain + skip links, i.e. real redundant paths, same
 * shape test_wifi_heavy_loss already uses) means more than one copy of the same flooded packet
 * reaches at least one node. dup_dropped (keyed on (src, pkt_id) already seen) must rise. */
static void test_duplicate_suppression(void)
{
    sim_cfg_t cfg;
    sim_default_cfg(&cfg, 5);
    sim_topology_trail(&cfg, -1);
    cfg.seed = 200;
    net_init(&N, &cfg, "sim_out/test_filter", 1, false); /* LoRa only: simplest flood to reason about */
    net_make_file(&N, 5, "img/dup.jpg", 3000, 1);
    sim_set_current(5);
    CHECK(alfp_send_file(&N.node[5].alfp, &sim_lora, BRIDGE, "img/dup.jpg", NULL, 0) == ALFP_OK);
    CHECK(net_run_until(&N, bridge_has_one, 120000));

    uint32_t total_dup = 0;
    for (uint8_t i = 1; i <= N.cfg.n_nodes; i++)
    {
        total_dup += stats_of(i, SIM_LORA).dup_dropped;
    }
    CHECK(total_dup > 0);
    printf("    duplicate packets dropped across the mesh: %lu\n", (unsigned long)total_dup);
    net_close(&N);
}

/* Packets not addressed to this node: a unicast frame must never be handed to a node's own
 * application layer (mesh_stats_t.delivered) unless it is the destination or the destination is
 * MESH_BROADCAST -- an intermediate relay only relays (mesh_stats_t.relayed), never delivers. */
static void test_not_addressed_to_node_is_not_delivered(void)
{
    sim_cfg_t cfg;
    sim_default_cfg(&cfg, 3);
    sim_topology_chain(&cfg, -1); /* 1 - 2 - 3: node 2 must relay node 3's unicast to node 1, never deliver it */
    cfg.seed = 201;
    net_init(&N, &cfg, "sim_out/test_filter", 1, false);
    net_make_file(&N, 3, "img/relay.jpg", 2000, 2);
    sim_set_current(3);
    CHECK(alfp_send_file(&N.node[3].alfp, &sim_lora, BRIDGE, "img/relay.jpg", NULL, 0) == ALFP_OK);
    CHECK(net_run_until(&N, bridge_has_one, 120000));

    mesh_stats_t relay_stats = stats_of(2, SIM_LORA);
    CHECK(relay_stats.delivered == 0); /* node 2 is not the destination: never delivered to its app layer */
    CHECK(relay_stats.relayed > 0);    /* but it did relay the frames on toward the bridge */
    printf("    relay node: delivered=%lu (must be 0) relayed=%lu\n", (unsigned long)relay_stats.delivered,
           (unsigned long)relay_stats.relayed);
    net_close(&N);
}

/* Injected packet loss (the 'd' console key on real hardware, sim_set_drop_rate here): a transfer
 * must still complete, with the resent counter climbing to show the repair actually happened. */
static void test_loss_injection_30_percent_still_completes(void)
{
    sim_cfg_t cfg;
    sim_default_cfg(&cfg, 2);
    sim_topology_full(&cfg, -1);
    cfg.seed = 202;
    net_init(&N, &cfg, "sim_out/test_filter", 1, false);
    sim_set_link_loss(SIM_LORA, 0.3f); /* per-link loss, same mechanism test_lora_loss_sweep_multihop uses */
    net_make_file(&N, 2, "img/loss.jpg", 6000, 3);
    sim_set_current(2);
    CHECK(alfp_send_file(&N.node[2].alfp, &sim_lora, BRIDGE, "img/loss.jpg", NULL, 0) == ALFP_OK);
    CHECK(net_run_until(&N, sender2_done, 300000));
    sim_set_link_loss(SIM_LORA, 0.0f);

    alfp_tx_report_t* r = &N.node[2].tx_reports[0];
    CHECK(r->result == ALFP_TX_DONE);
    CHECK(r->data_resent > 0); /* the repair (not the first round) is what proves loss injection did something */
    printf("    30%% loss: %u rounds, %u resent, transfer still completed\n", r->rounds, r->data_resent);
    net_close(&N);
}

/* A packet addressed to MESH_BROADCAST is delivered everywhere it is heard, unlike a unicast
 * addressed elsewhere (test_not_addressed_to_node_is_not_delivered above) -- the other half of
 * the same filtering rule, verified directly against sim_mesh.c's heartbeat frames (dst 0xFE is
 * sim_mesh.c's own heartbeat marker, delivered to nobody's app layer by design, but "heard" by
 * everyone -- used here only to confirm every node in range does hear the sender at all, which
 * the broadcast delivery rule depends on). */
static void test_heartbeat_heard_by_every_linked_node(void)
{
    sim_cfg_t cfg;
    sim_default_cfg(&cfg, 3);
    sim_topology_full(&cfg, -1);
    cfg.seed = 203;
    net_init(&N, &cfg, "sim_out/test_filter", 3, false);
    net_run_ms(&N, 3500); /* let heartbeats go out */
    for (uint8_t i = 1; i <= 3; i++)
    {
        sim_set_current(i); /* radio methods answer for whichever node sim_set_current() last named */
        for (uint8_t k = 0; k < N.node[i].n_radios; k++)
        {
            uint32_t t;
            CHECK(N.node[i].radios[k]->last_heard(BRIDGE, &t) || i == BRIDGE);
        }
    }
    net_close(&N);
}

int main(void)
{
    RUN(test_duplicate_suppression);
    RUN(test_not_addressed_to_node_is_not_delivered);
    RUN(test_loss_injection_30_percent_still_completes);
    RUN(test_heartbeat_heard_by_every_linked_node);
    return SUMMARY();
}
