/*
 * selftest_main.c - run the whole fragmentation pipeline on ONE Pico with a real microSD card.
 *                   OWNER: Member 2.  Needs: Pico W on the Maker Pi Pico + FAT32 microSD. No radios.
 *
 * What it does, over USB serial:
 *   1. mounts the SD card and measures raw read/write speed in fragment-sized chunks
 *   2. picks samples/img_00.jpg (or writes an 8 kB test file if there are no samples)
 *   3. runs a sender (node 2) and a bridge (node 1) on the same chip, joined by an in-memory
 *      "radio" that loses 0%, 20% and then 40% of packets
 *   4. checks the bridge's reassembled file against the original with CRC-32, byte for byte
 *   5. streams the received file in the uplink format, so tools/bridge_receiver.py can save it
 *   6. prints PASS or FAIL for each loss level, plus RAM used by the ALFP state
 *
 * This proves the SD code, the protocol and the RAM budget on real hardware before any radio works.
 */
#include <stdio.h>
#include <string.h>

#include "alfp.h"
#include "alfp_crc.h"
#include "capture_meta.h"
#include "loopback_radio.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "board.h"
#include "storage_fatfs.h"
#include "uplink.h"

static alfp_ctx_t g_tx, g_rx;
static bool g_tx_done, g_rx_done;
static alfp_tx_report_t g_tx_rep;
static char g_rx_path[80];

static uint32_t now_ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}
static uint32_t rnd(void)
{
    return get_rand_32();
}
static uint32_t p_now(void* c)
{
    (void)c;
    return now_ms();
}
static uint32_t p_rand(void* c)
{
    (void)c;
    return rnd();
}
static const alfp_platform_t k_plat = {NULL, p_now, p_rand, NULL};

static void out(const char* s, void* u)
{
    (void)u;
    fputs(s, stdout);
}

static void on_tx(alfp_ctx_t* c, const alfp_tx_report_t* r)
{
    (void)c;
    g_tx_rep = *r;
    g_tx_done = true;
}

static void on_rx(alfp_ctx_t* c, const alfp_rx_report_t* r)
{
    snprintf(g_rx_path, sizeof(g_rx_path), "%s", r->path);
    uplink_send_file(c->cfg.storage, r, out, NULL);
    g_rx_done = true;
}

static uint32_t file_crc(const alfp_storage_t* st, const char* path, int32_t* size_out)
{
    uint8_t buf[256];
    int h = st->open_read(st->ctx, path);
    if (h < 0)
    {
        *size_out = -1;
        return 0;
    }
    int32_t size = st->size(st->ctx, h);
    uint32_t crc = ALFP_CRC32_INIT;
    for (int32_t off = 0; off < size;)
    {
        int n = st->read_at(st->ctx, h, (uint32_t)off, buf, sizeof(buf));
        if (n <= 0)
        {
            break;
        }
        crc = alfp_crc32(crc, buf, (size_t)n);
        off += n;
    }
    st->close(st->ctx, h);
    *size_out = size;
    return crc;
}

static void sd_speed(const alfp_storage_t* st)
{
    uint8_t buf[241];
    for (unsigned i = 0; i < sizeof(buf); i++)
    {
        buf[i] = (uint8_t)i;
    }
    int h = st->open_write(st->ctx, "selftest/speed.bin");
    uint32_t t0 = now_ms();
    for (int i = 0; i < 136; i++) /* ~32 kB in fragment-sized writes, like a bridge receiving */
    {
        st->write_at(st->ctx, h, (uint32_t)i * sizeof(buf), buf, sizeof(buf));
    }
    uint32_t t1 = now_ms();
    for (int i = 0; i < 136; i++)
    {
        st->read_at(st->ctx, h, (uint32_t)((i * 37) % 136) * sizeof(buf), buf, sizeof(buf)); /* random order */
    }
    uint32_t t2 = now_ms();
    st->close(st->ctx, h);
    st->remove(st->ctx, "selftest/speed.bin");
    printf("sd: 136 x 241 B writes %lu ms (%.2f ms each), random reads %lu ms (%.2f ms each)\n",
           (unsigned long)(t1 - t0), (t1 - t0) / 136.0, (unsigned long)(t2 - t1), (t2 - t1) / 136.0);
}

static bool run_once(const alfp_storage_t* st, const char* src, float loss)
{
    loopback_init(now_ms, rnd, 250, 40, 20);
    loopback_set_loss(loss);
    alfp_cfg_t ct = {.node_id = 2, .bridge_id = 1, .storage = st, .platform = &k_plat, .on_tx_done = on_tx};
    alfp_cfg_t cr = {.node_id = 1, .bridge_id = 1, .storage = st, .platform = &k_plat, .on_rx_done = on_rx,
                     .inbox_dir = "inbox", .tmp_dir = "rx"};
    alfp_init(&g_tx, &ct);
    alfp_init(&g_rx, &cr);
    g_tx_done = g_rx_done = false;

    capture_meta_t m;
    capture_meta_clear(&m);
    m.node_id = 2;
    m.capture_id = 1;
    uint8_t mb[CAPTURE_META_LEN];
    capture_meta_pack(&m, mb);
    if (alfp_send_file(&g_tx, &loopback_a, 1, src, mb, sizeof(mb)) != ALFP_OK)
    {
        printf("FAIL: cannot start transfer of %s\n", src);
        return false;
    }
    const mesh_radio_t* ra[1] = {&loopback_a};
    const mesh_radio_t* rb[1] = {&loopback_b};
    uint32_t t0 = now_ms();
    while (!g_tx_done && now_ms() - t0 < 300000)
    {
        alfp_service(&g_tx, ra, 1);
        alfp_service(&g_rx, rb, 1);
        sleep_ms(1);
    }
    int32_t s1, s2;
    uint32_t c1 = file_crc(st, src, &s1);
    uint32_t c2 = g_rx_done ? file_crc(st, g_rx_path, &s2) : 0;
    bool ok = g_tx_done && g_tx_rep.result == ALFP_TX_DONE && g_rx_done && c1 == c2 && s1 == s2;
    printf("%s: loss %2.0f%%, %ld bytes, %u frags, %u rounds, %u resent, %.1f s, crc %08lx/%08lx\n",
           ok ? "PASS" : "FAIL", loss * 100, (long)s1, g_tx_rep.frag_count, g_tx_rep.rounds, g_tx_rep.data_resent,
           (g_tx_rep.finished_ms - g_tx_rep.started_ms) / 1000.0, (unsigned long)c1, (unsigned long)c2);
    return ok;
}

int main(void)
{
    board_spi1_idle(); /* LoRa CS + SD CS high: they share SPI1 */
    stdio_init_all();
    sleep_ms(3000);
    printf("\nALFP self-test (Pico + microSD, no radios)\n");
    printf("ram: alfp context %u bytes, outbox job max %u bytes\n", (unsigned)sizeof(alfp_ctx_t),
           (unsigned)(4u + ALFP_PATH_MAX + ALFP_MAX_META));
    while (!storage_fatfs_mount())
    {
        sleep_ms(2000);
    }
    const alfp_storage_t* st = storage_fatfs();
    st->mkdir(st->ctx, "selftest");
    sd_speed(st);

    const char* src = "samples/img_00.jpg";
    int h = st->open_read(st->ctx, src);
    if (h >= 0)
    {
        st->close(st->ctx, h);
    }
    else
    {
        src = "selftest/test8k.bin";
        h = st->open_write(st->ctx, src);
        uint8_t b[250];
        uint32_t x = 12345;
        for (int off = 0; off < 8000; off += 250)
        {
            for (int i = 0; i < 250; i++)
            {
                x = x * 1103515245u + 12345u;
                b[i] = (uint8_t)(x >> 16);
            }
            st->write_at(st->ctx, h, (uint32_t)off, b, 250);
        }
        st->close(st->ctx, h);
        printf("no samples/img_00.jpg, using generated %s\n", src);
    }

    const float losses[] = {0.0f, 0.2f, 0.4f};
    int pass = 0;
    for (int i = 0; i < 3; i++)
    {
        pass += run_once(st, src, losses[i]);
    }
    printf("\nSELFTEST %s (%d/3)\n", pass == 3 ? "PASSED" : "FAILED", pass);
    for (;;)
    {
        sleep_ms(1000);
    }
}
