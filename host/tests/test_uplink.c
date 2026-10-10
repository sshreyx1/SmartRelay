/*
 * test_uplink.c - end to end: sender -> mesh -> bridge -> USB uplink text -> (Python receiver).
 *
 * Runs one WiFi transfer in the simulator with capture metadata, then writes the bridge's uplink
 * stream (with some ordinary log lines mixed in) to uplink_stream.txt. The ctest step
 * "uplink_python" then feeds that file to tools/bridge_receiver.py and checks the saved image.
 */
#include <string.h>

#include "capture_meta.h"
#include "sim_net.h"
#include "testlib.h"
#include "uplink.h"

static net_t N;
static FILE* g_out;

static bool done(net_t* n)
{
    return n->node[2].n_tx >= 1;
}

static void w(const char* s, void* u)
{
    (void)u;
    fputs(s, g_out);
}

static void test_uplink_stream(void)
{
    sim_cfg_t cfg;
    sim_default_cfg(&cfg, 2);
    sim_topology_full(&cfg, -1);
    net_init(&N, &cfg, "sim_out/uplink", 2, false);
    net_run_ms(&N, 200);
    net_make_file(&N, 2, "samples/img_00.jpg", 7777, 99);
    capture_meta_t m;
    capture_meta_clear(&m);
    m.node_id = 2;
    m.capture_id = 5;
    m.temp_c_x10 = 297;
    m.humidity_x10 = 842;
    m.trigger_flags = CM_TRIG_PIR | CM_TRIG_SOUND;
    uint8_t mb[CAPTURE_META_LEN];
    capture_meta_pack(&m, mb);
    sim_set_current(2);
    CHECK(alfp_send_file(&N.node[2].alfp, &sim_wifi, 1, "samples/img_00.jpg", mb, sizeof(mb)) == ALFP_OK);
    CHECK(net_run_until(&N, done, 60000));
    CHECK(N.node[1].n_rx == 1);

    /* rebuild the report the bridge's callback received (metadata comes from the .meta sidecar) */
    alfp_rx_report_t r = N.node[1].rx_reports[0];
    uint8_t meta[64];
    char mp[600];
    snprintf(mp, sizeof(mp), "%s/node1/%s", N.root, r.path);
    strcpy(strrchr(mp, '.'), ".meta");
    FILE* f = fopen(mp, "rb");
    CHECK(f != NULL);
    r.meta_len = f ? (uint8_t)fread(meta, 1, sizeof(meta), f) : 0;
    if (f)
    {
        fclose(f);
    }
    r.meta = meta;

    g_out = fopen("uplink_stream.txt", "w");
    fputs("node 1 ready (bridge)\n", g_out);
    CHECK(uplink_send_file(&N.node[1].st, &r, w, NULL) == 0);
    fputs("some other log line\n", g_out);
    fclose(g_out);
    net_close(&N);
}

int main(void)
{
    RUN(test_uplink_stream);
    return SUMMARY();
}
