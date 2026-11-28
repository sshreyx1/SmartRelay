/*
 * test_uplink.c - end to end: sender -> mesh -> bridge -> USB uplink text ->
 * (Python receiver).
 *
 * Runs one WiFi transfer in the simulator with capture metadata, then writes
 * the bridge's uplink stream (with some ordinary log lines mixed in) to
 * uplink_stream.txt. The ctest step "uplink_python" then feeds that file to
 * tools/bridge_receiver.py and checks the saved image.
 */
#include <string.h>

#include "capture_meta.h"
#include "sim_net.h"
#include "testlib.h"
#include "uplink.h"

static net_t net;
static FILE* gp_out;

static bool done (net_t* p_net)
{
    return p_net->node[2].n_tx >= 1;
}

static void w (const char* p_str, void* p_user)
{
    (void)p_user;
    fputs(p_str, gp_out);
}

static char*  gp_buf;
static size_t g_buf_len;

static void wb (const char* p_str, void* p_user)
{
    (void)p_user;
    size_t len = strlen(p_str);
    memcpy(gp_buf + g_buf_len, p_str, len);
    g_buf_len += len;
    gp_buf[g_buf_len] = 0;
}

static void test_uplink_stream (void)
{
    sim_cfg_t cfg;
    sim_default_cfg(&cfg, 2);
    sim_topology_full(&cfg, -1);
    net_init(&net, &cfg, "sim_out/uplink", 2, false);
    net_run_ms(&net, 200);
    net_make_file(&net, 2, "samples/img_00.jpg", 7777, 99);
    capture_meta_t cap_meta;
    capture_meta_clear(&cap_meta);
    cap_meta.node_id       = 2;
    cap_meta.capture_id    = 5;
    cap_meta.temp_c_x10    = 297;
    cap_meta.humidity_x10  = 842;
    cap_meta.trigger_flags = CM_TRIG_PIR | CM_TRIG_SOUND;
    uint8_t meta_buf[CAPTURE_META_LEN];
    capture_meta_pack(&cap_meta, meta_buf);
    sim_set_current(2);
    CHECK(ALFP_OK == alfp_send_file(&net.node[2].alfp, &g_sim_wifi, 1,
                                    "samples/img_00.jpg", meta_buf,
                                    sizeof(meta_buf)));
    CHECK(net_run_until(&net, done, 60000));
    CHECK(1 == net.node[1].n_rx);

    /* rebuild the report the bridge's callback received (metadata comes from
     * the .met sidecar) */
    alfp_rx_report_t rep = net.node[1].rx_reports[0];
    uint8_t          meta[64];
    char             meta_path[600];
    snprintf(meta_path, sizeof(meta_path), "%s/node1/%s", net.root, rep.p_path);
    strcpy(strrchr(meta_path, '.'), ".met");
    FILE* p_file = fopen(meta_path, "rb");
    CHECK(NULL != p_file);
    rep.meta_len = p_file ? (uint8_t)fread(meta, 1, sizeof(meta), p_file) : 0;
    if (p_file)
    {
        fclose(p_file);
    }
    rep.p_meta = meta;

    gp_out = fopen("uplink_stream.txt", "w");
    fputs("node 1 ready (bridge)\n", gp_out);
    CHECK(0 == uplink_send_file(&net.node[1].store, &rep, w, NULL));
    fputs("some other log line\n", gp_out);
    fclose(gp_out);

    /* The queued path the firmware uses must produce exactly the same text,
     * whatever the per-pass line budget, and must keep several finished files
     * in order. */
    static char one_shot[64 * 1024];
    static char queued[3 * 64 * 1024];
    gp_buf    = one_shot;
    g_buf_len = 0;
    CHECK(0 == uplink_send_file(&net.node[1].store, &rep, wb, NULL));
    size_t one_len = g_buf_len;
    CHECK(one_len > 7777u);

    static uplink_queue_t queue;
    uplink_queue_init(&queue, &net.node[1].store);
    for (int idx = 0; idx < 3; idx++)
    {
        CHECK(uplink_queue_add(&queue, &rep));
    }
    rep.p_path = NULL; /* the queue must hold its own copy of the report */
    CHECK(3u == uplink_queue_waiting(&queue));
    CHECK(0u == uplink_queue_service(&queue, 0, wb, NULL));
    gp_buf          = queued;
    g_buf_len       = 0;
    unsigned passes = 0;
    while ((0u != uplink_queue_waiting(&queue)) && (passes < 100000u))
    {
        CHECK(uplink_queue_service(&queue, 3, wb, NULL) <= 3u);
        passes++;
    }
    CHECK((3u * one_len) == g_buf_len);
    for (int idx = 0; idx < 3; idx++)
    {
        CHECK(0 == memcmp(queued + (size_t)idx * one_len, one_shot, one_len));
    }
    CHECK((3u == queue.sent_ok) && (0u == queue.sent_bad) &&
          (3u == queue.max_waiting));
    /* 7777 B = 163 @D lines + header + END per file, 3 lines per pass */
    CHECK(passes == (3u * 165u + 2u) / 3u);

    /* queue full is reported, not overwritten */
    rep        = net.node[1].rx_reports[0];
    rep.p_meta = meta;
    uplink_queue_init(&queue, &net.node[1].store);
    for (unsigned idx = 0; idx < UPLINK_QUEUE_DEPTH; idx++)
    {
        CHECK(uplink_queue_add(&queue, &rep));
    }
    CHECK(!uplink_queue_add(&queue, &rep));
    CHECK(1u == queue.queue_full);

    /* a file that disappears is aborted and the next one still goes */
    uplink_queue_init(&queue, &net.node[1].store);
    alfp_rx_report_t gone = rep;
    gone.p_path           = "inbox/does_not_exist.bin";
    CHECK(uplink_queue_add(&queue, &gone));
    CHECK(uplink_queue_add(&queue, &rep));
    gp_buf    = queued;
    g_buf_len = 0;
    while (0u != uplink_queue_waiting(&queue))
    {
        (void)uplink_queue_service(&queue, 8, wb, NULL);
    }
    CHECK((1u == queue.sent_bad) && (1u == queue.sent_ok));
    CHECK(NULL != strstr(queued, "@ALFP ABORT read error\n"));
    net_close(&net);
}

int main (void)
{
    RUN(test_uplink_stream);
    return SUMMARY();
}

/*** end of file ***/

