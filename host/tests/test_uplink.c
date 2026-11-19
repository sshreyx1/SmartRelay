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

static char* g_buf;
static size_t g_buf_len;

static void wb(const char* s, void* u)
{
    (void)u;
    size_t n = strlen(s);
    memcpy(g_buf + g_buf_len, s, n);
    g_buf_len += n;
    g_buf[g_buf_len] = 0;
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

    /* rebuild the report the bridge's callback received (metadata comes from the .met sidecar) */
    alfp_rx_report_t r = N.node[1].rx_reports[0];
    uint8_t meta[64];
    char mp[600];
    snprintf(mp, sizeof(mp), "%s/node1/%s", N.root, r.path);
    strcpy(strrchr(mp, '.'), ".met");
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

    /* The queued path the firmware uses must produce exactly the same text, whatever the per-pass
     * line budget, and must keep several finished files in order. */
    static char one_shot[64 * 1024], queued[3 * 64 * 1024];
    g_buf = one_shot;
    g_buf_len = 0;
    CHECK(uplink_send_file(&N.node[1].st, &r, wb, NULL) == 0);
    size_t one_len = g_buf_len;
    CHECK(one_len > 7777u);

    static uplink_queue_t q;
    uplink_queue_init(&q, &N.node[1].st);
    for (int i = 0; i < 3; i++)
    {
        CHECK(uplink_queue_add(&q, &r));
    }
    r.path = NULL; /* the queue must hold its own copy of the report */
    CHECK(uplink_queue_waiting(&q) == 3u);
    CHECK(uplink_queue_service(&q, 0, wb, NULL) == 0u);
    g_buf = queued;
    g_buf_len = 0;
    unsigned passes = 0;
    while (uplink_queue_waiting(&q) != 0u && passes < 100000u)
    {
        CHECK(uplink_queue_service(&q, 3, wb, NULL) <= 3u);
        passes++;
    }
    CHECK(g_buf_len == 3u * one_len);
    for (int i = 0; i < 3; i++)
    {
        CHECK(memcmp(queued + (size_t)i * one_len, one_shot, one_len) == 0);
    }
    CHECK(q.sent_ok == 3u && q.sent_bad == 0u && q.max_waiting == 3u);
    /* 7777 B = 163 @D lines + header + END per file, 3 lines per pass */
    CHECK(passes == (3u * 165u + 2u) / 3u);

    /* queue full is reported, not overwritten */
    r = N.node[1].rx_reports[0];
    r.meta = meta;
    uplink_queue_init(&q, &N.node[1].st);
    for (unsigned i = 0; i < UPLINK_QUEUE_DEPTH; i++)
    {
        CHECK(uplink_queue_add(&q, &r));
    }
    CHECK(!uplink_queue_add(&q, &r));
    CHECK(q.queue_full == 1u);

    /* a file that disappears is aborted and the next one still goes */
    uplink_queue_init(&q, &N.node[1].st);
    alfp_rx_report_t gone = r;
    gone.path = "inbox/does_not_exist.bin";
    CHECK(uplink_queue_add(&q, &gone));
    CHECK(uplink_queue_add(&q, &r));
    g_buf = queued;
    g_buf_len = 0;
    while (uplink_queue_waiting(&q) != 0u)
    {
        (void)uplink_queue_service(&q, 8, wb, NULL);
    }
    CHECK(q.sent_bad == 1u && q.sent_ok == 1u);
    CHECK(strstr(queued, "@ALFP ABORT read error\n") != NULL);
    net_close(&N);
}

int main(void)
{
    RUN(test_uplink_stream);
    return SUMMARY();
}
