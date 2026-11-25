/*
 * fake_wst_link.c - laptop replacement for Goutham's wst_link.c (the UART driver for the ESP-01).
 *
 * Implements the wst_link.h API that his wifi_mesh.c calls, so his mesh code runs UNCHANGED on
 * the laptop. Each node is a process; "the air" is UDP on localhost (ports 47200 + node).
 *   - every packet carries the sender's real WST MAC from wifi_mesh_config.h, so his topology
 *     filter (A: B C D, C: A B D E, E: C D ...) decides who hears whom, exactly as on the boards
 *   - a packet takes as long as the 115200 baud UART needs to push it to the ESP-01 (~22 ms for
 *     250 bytes) plus 1 ms on air; tx_busy() is true meanwhile, like the real TXDONE wait
 *   - WIFI_LOSS env var: random loss per received packet (0.0 - 1.0)
 */
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

static int sock = -1, self;
static double loss;
static wst_link_stats_t st;
static struct
{
    uint8_t len;
    uint8_t b[WST_MAX_PKT];
} q[WST_TX_QUEUE];
static int qh, qn;
static uint64_t busy_until_us;

void fake_wst_set_node(int node_id)
{
    self = node_id;
}

static uint64_t now_us(void)
{
    return get_absolute_time();
}

/* UART: start byte + type + len + packet + crc, 10 bits each at 115200, then ~1 ms over the air */
static uint64_t frame_us(uint8_t len)
{
    return (uint64_t)(4 + len) * 10u * 1000000u / 115200u + 1000u;
}

void wst_link_init(void)
{
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)(BASE_PORT + self));
    if (bind(sock, (struct sockaddr*)&a, sizeof(a)) != 0)
    {
        perror("bind");
        /* DEVIATION (BARR-C 8.5.b, exit()): host integration-test stand-in for wst_link.c, never
         * in firmware. wst_link_init() returns void (the real API), so a test node that cannot
         * bind its UDP port has to stop here (and the integration test then fails).
         * Approved by: ____________________ (project manager), date: __________ */
        exit(5);
    }
    fcntl(sock, F_SETFL, O_NONBLOCK);
    const char* l = getenv("WIFI_LOSS");
    loss = l ? atof(l) : 0.0;
    srand((unsigned)(self * 104729));
    printf("fake WST: node %d (WST-%d MAC) on udp %d, loss %.0f%%\n", self, self, BASE_PORT + self, loss * 100);
}

bool wst_link_send(const uint8_t* pkt, uint8_t len)
{
    if (len == 0 || len > WST_MAX_PKT)
    {
        return false;
    }
    if (qn == WST_TX_QUEUE)
    {
        st.tx_queue_full++;
        return false;
    }
    if (qn == 0)
    {
        busy_until_us = now_us() + frame_us(len);
    }
    int i = (qh + qn) % WST_TX_QUEUE;
    q[i].len = len;
    memcpy(q[i].b, pkt, len);
    qn++;
    st.tx_queued++;
    return true;
}

void wst_link_poll(void)
{
    uint64_t t = now_us();
    if (qn && t >= busy_until_us)
    {
        /* the frame at the head has finished its UART + air time: broadcast it */
        uint8_t out[6 + WST_MAX_PKT];
        memcpy(out, WIFI_MESH_MACS[self], 6);
        memcpy(out + 6, q[qh].b, q[qh].len);
        for (int n = 1; n <= 5; n++)
        {
            if (n == self)
            {
                continue;
            }
            struct sockaddr_in a = {0};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = htons((uint16_t)(BASE_PORT + n));
            sendto(sock, out, (size_t)(6 + q[qh].len), 0, (struct sockaddr*)&a, sizeof(a));
        }
        st.tx_ok++;
        qh = (qh + 1) % WST_TX_QUEUE;
        qn--;
        if (qn)
        {
            busy_until_us = t + frame_us(q[qh].len);
        }
    }
}

bool wst_link_recv(wst_rx_pkt_t* out)
{
    for (;;)
    {
        uint8_t tmp[6 + WST_MAX_PKT];
        ssize_t r = recv(sock, tmp, sizeof(tmp), 0);
        if (r <= 6)
        {
            return false;
        }
        if ((double)rand() / RAND_MAX < loss)
        {
            continue; /* lost in the air */
        }
        memcpy(out->mac, tmp, 6);
        out->len = (uint8_t)(r - 6);
        memcpy(out->data, tmp + 6, out->len);
        st.rx_ok++;
        return true;
    }
}

bool wst_link_tx_busy(void)
{
    return qn > 0;
}

bool wst_link_get_mac(uint8_t mac[6])
{
    memcpy(mac, WIFI_MESH_MACS[self], 6);
    return true;
}

void wst_link_request_mac(void) {}

void wst_link_get_stats(wst_link_stats_t* o)
{
    *o = st;
}

uint8_t wst_crc8(const uint8_t* d, uint32_t n, uint8_t c)
{
    while (n--)
    {
        c ^= *d++;
        for (int i = 0; i < 8; i++)
        {
            c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
        }
    }
    return c;
}
