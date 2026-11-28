/*
 * fake_wst_link.c - laptop replacement for Goutham's wst_link.c (the UART
 * driver for the ESP-01).
 *
 * Implements the wst_link.h API that his wifi_mesh.c calls, so his mesh code
 * runs UNCHANGED on the laptop. Each node is a process; "the air" is UDP on
 * localhost (ports 47200 + node).
 *   - every packet carries the sender's real WST MAC from wifi_mesh_config.h,
 *   so his topology
 *     filter (A: B C D, C: A B D E, E: C D ...) decides who hears whom, exactly
 *     as on the boards
 *   - a packet takes as long as the 115200 baud UART needs to push it to the
 *   ESP-01 (~22 ms for
 *     250 bytes) plus 1 ms on air; tx_busy() is true meanwhile, like the real
 *     TXDONE wait
 *   - WIFI_LOSS env var: random loss per received packet (0.0 - 1.0)
 */
#include "float_types.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "pico/stdlib.h"
#include "wifi_mesh_config.h"
#include "wst_link.h"

#define BASE_PORT 47200

static int              sock = -1;
static int              self;
static float64_t        loss;
static wst_link_stats_t stats;
static struct
{
    uint8_t len;
    uint8_t buf[WST_MAX_PKT];
} queue[WST_TX_QUEUE];
static int      queue_head;
static int      queue_count;
static uint64_t busy_until_us;

void fake_wst_set_node (int node_id)
{
    self = node_id;
}

static uint64_t now_us (void)
{
    return get_absolute_time();
}

/* UART: start byte + type + len + packet + crc, 10 bits each at 115200, then ~1
 * ms over the air */
static uint64_t frame_us (uint8_t len)
{
    return (uint64_t)(4 + len) * 10u * 1000000u / 115200u + 1000u;
}

void wst_link_init (void)
{
    sock                    = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family         = AF_INET;
    addr.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    addr.sin_port           = htons((uint16_t)(BASE_PORT + self));
    if (0 != bind(sock, (struct sockaddr*)&addr, sizeof(addr)))
    {
        perror("bind");
        /* DEVIATION (BARR-C 8.5.b, exit()): host integration-test stand-in for
         * wst_link.c, never in firmware. wst_link_init() returns void (the real
         * API), so a test node that cannot bind its UDP port has to stop here
         * (and the integration test then fails).
         * Approved by: ____________________ (project manager), date:
         * __________ */
        exit(5);
    }
    fcntl(sock, F_SETFL, O_NONBLOCK);
    const char* p_loss_env = getenv("WIFI_LOSS");
    loss                   = p_loss_env ? atof(p_loss_env) : 0.0;
    srand((unsigned)(self * 104729));
    printf("fake WST: node %d (WST-%d MAC) on udp %d, loss %.0f%%\n", self,
           self, BASE_PORT + self, loss * 100);
}

bool wst_link_send (const uint8_t* p_pkt, uint8_t len)
{
    if ((0 == len) || (len > WST_MAX_PKT))
    {
        return false;
    }
    if (WST_TX_QUEUE == queue_count)
    {
        stats.tx_queue_full++;
        return false;
    }
    if (0 == queue_count)
    {
        busy_until_us = now_us() + frame_us(len);
    }
    int idx        = (queue_head + queue_count) % WST_TX_QUEUE;
    queue[idx].len = len;
    memcpy(queue[idx].buf, p_pkt, len);
    queue_count++;
    stats.tx_queued++;
    return true;
}

void wst_link_poll (void)
{
    uint64_t now_t = now_us();
    if (queue_count && (now_t >= busy_until_us))
    {
        /* the frame at the head has finished its UART + air time: broadcast
         * it */
        uint8_t out[6 + WST_MAX_PKT];
        memcpy(out, wifi_mesh_macs[self], 6);
        memcpy(out + 6, queue[queue_head].buf, queue[queue_head].len);
        for (int node = 1; node <= 5; node++)
        {
            if (node == self)
            {
                continue;
            }
            struct sockaddr_in addr = {0};
            addr.sin_family         = AF_INET;
            addr.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
            addr.sin_port           = htons((uint16_t)(BASE_PORT + node));
            sendto(sock, out, (size_t)(6 + queue[queue_head].len), 0,
                   (struct sockaddr*)&addr, sizeof(addr));
        }
        stats.tx_ok++;
        queue_head = (queue_head + 1) % WST_TX_QUEUE;
        queue_count--;
        if (queue_count)
        {
            busy_until_us = now_t + frame_us(queue[queue_head].len);
        }
    }
}

bool wst_link_recv (wst_rx_pkt_t* p_out)
{
    for (;;)
    {
        uint8_t tmp[6 + WST_MAX_PKT];
        ssize_t ret = recv(sock, tmp, sizeof(tmp), 0);
        if (ret <= 6)
        {
            return false;
        }
        if ((float64_t)rand() / RAND_MAX < loss)
        {
            continue; /* lost in the air */
        }
        memcpy(p_out->mac, tmp, 6);
        p_out->len = (uint8_t)(ret - 6);
        memcpy(p_out->data, tmp + 6, p_out->len);
        stats.rx_ok++;
        return true;
    }
}

bool wst_link_tx_busy (void)
{
    return queue_count > 0;
}

bool wst_link_get_mac (uint8_t mac[6])
{
    memcpy(mac, wifi_mesh_macs[self], 6);
    return true;
}

void wst_link_request_mac (void)
{
}

void wst_link_get_stats (wst_link_stats_t* p_out)
{
    *p_out = stats;
}

uint8_t wst_crc8 (const uint8_t* p_data, uint32_t len, uint8_t crc)
{
    while (len--)
    {
        crc ^= *p_data++;
        for (int idx = 0; idx < 8; idx++)
        {
            crc = (uint8_t)((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
        }
    }
    return crc;
}

/*** end of file ***/

