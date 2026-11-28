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
static bool gb_tx_done, gb_rx_done;
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
static uint32_t p_now(void* p_ctx)
{
    (void)p_ctx;
    return now_ms();
}
static uint32_t p_rand(void* p_ctx)
{
    (void)p_ctx;
    return rnd();
}
static const alfp_platform_t k_plat = {NULL, p_now, p_rand, NULL};

static void out(const char* p_str, void* p_user)
{
    (void)p_user;
    fputs(p_str, stdout);
}

static void on_tx(alfp_ctx_t* p_ctx, const alfp_tx_report_t* p_rep)
{
    (void)p_ctx;
    g_tx_rep = *p_rep;
    gb_tx_done = true;
}

static void on_rx(alfp_ctx_t* p_ctx, const alfp_rx_report_t* p_rep)
{
    snprintf(g_rx_path, sizeof(g_rx_path), "%s", p_rep->p_path);
    uplink_send_file(p_ctx->cfg.p_storage, p_rep, out, NULL);
    gb_rx_done = true;
}

static uint32_t file_crc(const alfp_storage_t* p_store, const char* p_path, int32_t* p_size_out)
{
    uint8_t buf[256];
    int handle = p_store->p_open_read(p_store->p_ctx, p_path);
    if (handle < 0)
    {
        *p_size_out = -1;
        return 0;
    }
    int32_t size = p_store->p_size(p_store->p_ctx, handle);
    uint32_t crc = ALFP_CRC32_INIT;
    for (int32_t off = 0; off < size;)
    {
        int len = p_store->p_read_at(p_store->p_ctx, handle, (uint32_t)off, buf, sizeof(buf));
        if (len <= 0)
        {
            break;
        }
        crc = alfp_crc32(crc, buf, (size_t)len);
        off += len;
    }
    p_store->p_close(p_store->p_ctx, handle);
    *p_size_out = size;
    return crc;
}

static void sd_speed(const alfp_storage_t* p_store)
{
    uint8_t buf[241];
    for (unsigned idx = 0; idx < sizeof(buf); idx++)
    {
        buf[idx] = (uint8_t)idx;
    }
    int handle = p_store->p_open_write(p_store->p_ctx, "selftest/speed.bin");
    uint32_t start_t = now_ms();
    for (int idx = 0; idx < 136; idx++) /* ~32 kB in fragment-sized writes, like a bridge receiving */
    {
        p_store->p_write_at(p_store->p_ctx, handle, (uint32_t)idx * sizeof(buf), buf, sizeof(buf));
    }
    uint32_t mid_t = now_ms();
    for (int idx = 0; idx < 136; idx++)
    {
        p_store->p_read_at(p_store->p_ctx, handle, (uint32_t)((idx * 37) % 136) * sizeof(buf), buf, sizeof(buf)); /* random order */
    }
    uint32_t end_t_ms = now_ms();
    p_store->p_close(p_store->p_ctx, handle);
    p_store->p_remove(p_store->p_ctx, "selftest/speed.bin");
    printf("sd: 136 x 241 B writes %lu ms (%.2f ms each), random reads %lu ms (%.2f ms each)\n",
           (unsigned long)(mid_t - start_t), (mid_t - start_t) / 136.0, (unsigned long)(end_t_ms - mid_t), (end_t_ms - mid_t) / 136.0);
}

static bool run_once(const alfp_storage_t* p_store, const char* p_src, float loss)
{
    loopback_init(now_ms, rnd, 250, 40, 20);
    loopback_set_loss(loss);
    alfp_cfg_t cfg_tx = {.node_id = 2, .bridge_id = 1, .p_storage = p_store, .p_platform = &k_plat, .p_on_tx_done = on_tx};
    alfp_cfg_t coding_rate = {.node_id = 1, .bridge_id = 1, .p_storage = p_store, .p_platform = &k_plat, .p_on_rx_done = on_rx,
                     .p_inbox_dir = "inbox", .p_tmp_dir = "rx"};
    alfp_init(&g_tx, &cfg_tx);
    alfp_init(&g_rx, &coding_rate);
    gb_tx_done = gb_rx_done = false;

    capture_meta_t meta;
    capture_meta_clear(&meta);
    meta.node_id = 2;
    meta.capture_id = 1;
    uint8_t meta_buf[CAPTURE_META_LEN];
    capture_meta_pack(&meta, meta_buf);
    if (ALFP_OK != alfp_send_file(&g_tx, &g_loopback_a, 1, p_src, meta_buf, sizeof(meta_buf)))
    {
        printf("FAIL: cannot start transfer of %s\n", p_src);
        return false;
    }
    const mesh_radio_t* radios_a[1] = {&g_loopback_a};
    const mesh_radio_t* radios_b[1] = {&g_loopback_b};
    uint32_t start_t = now_ms();
    while ((!gb_tx_done) && (now_ms() - start_t < 300000))
    {
        alfp_service(&g_tx, radios_a, 1);
        alfp_service(&g_rx, radios_b, 1);
        sleep_ms(1);
    }
    int32_t size_a, size_b;
    uint32_t crc_a = file_crc(p_store, p_src, &size_a);
    uint32_t crc_b = gb_rx_done ? file_crc(p_store, g_rx_path, &size_b) : 0;
    bool b_is_ok = gb_tx_done && (ALFP_TX_DONE == g_tx_rep.result) && gb_rx_done && (crc_a == crc_b) && (size_a == size_b);
    printf("%s: loss %2.0f%%, %ld bytes, %u frags, %u rounds, %u resent, %.1f s, crc %08lx/%08lx\n",
           b_is_ok ? "PASS" : "FAIL", loss * 100, (long)size_a, g_tx_rep.frag_count, g_tx_rep.rounds, g_tx_rep.data_resent,
           (g_tx_rep.finished_ms - g_tx_rep.started_ms) / 1000.0, (unsigned long)crc_a, (unsigned long)crc_b);
    return b_is_ok;
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
    const alfp_storage_t* p_store = storage_fatfs();
    p_store->p_mkdir(p_store->p_ctx, "selftest");
    sd_speed(p_store);

    const char* p_src = "samples/img_00.jpg";
    int handle = p_store->p_open_read(p_store->p_ctx, p_src);
    if (handle >= 0)
    {
        p_store->p_close(p_store->p_ctx, handle);
    }
    else
    {
        p_src = "selftest/test8k.bin";
        handle = p_store->p_open_write(p_store->p_ctx, p_src);
        uint8_t buf[250];
        uint32_t value_x = 12345;
        for (int off = 0; off < 8000; off += 250)
        {
            for (int idx = 0; idx < 250; idx++)
            {
                value_x = value_x * 1103515245u + 12345u;
                buf[idx] = (uint8_t)(value_x >> 16);
            }
            p_store->p_write_at(p_store->p_ctx, handle, (uint32_t)off, buf, 250);
        }
        p_store->p_close(p_store->p_ctx, handle);
        printf("no samples/img_00.jpg, using generated %s\n", p_src);
    }

    const float losses[] = {0.0f, 0.2f, 0.4f};
    int pass = 0;
    for (int idx = 0; idx < 3; idx++)
    {
        pass += run_once(p_store, p_src, losses[idx]);
    }
    printf("\nSELFTEST %s (%d/3)\n", 3 == pass ? "PASSED" : "FAILED", pass);
    for (;;)
    {
        sleep_ms(1000);
    }
}
