/*
 * fake_lora_radio.c - laptop replacement for Mridula's lora_radio.c (the LR1121 driver layer).
 *
 * Implements exactly the lora_radio.h API that her lora_mesh.c calls, so her mesh code runs
 * UNCHANGED on the laptop. Each node is a separate process; the "air" is UDP on localhost.
 *   - time on air from Semtech's formula with her settings (SF7, BW125, CR4/5, 8 preamble, CRC on)
 *   - half duplex: a packet arriving while this node is transmitting is lost
 *   - LORA_LOSS env var: random loss per received packet (0.0 - 1.0)
 */
#include "float_types.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "lora_radio.h"
#include "pico/stdlib.h"

#define BASE_PORT 47100
#define MAX_NODES 5

static int sock = -1;
static int self;
static bool b_tx_on;
static uint32_t tx_end;
static uint32_t tx_start;
static uint8_t tx_buf[256];
static uint8_t tx_len;
static float64_t loss;

void fake_radio_set_node(int node_id) { self = node_id; }

static uint32_t now(void) { return to_ms_since_boot(get_absolute_time()); }

uint32_t lora_radio_time_on_air_ms(uint8_t len)
{
    const int spread = 7, coding_rate = 1, crc = 1, npre = 8;
    float64_t time_spec = (float64_t)(1 << spread) / 125000.0 * 1000.0;
    float64_t sym = ceil((8.0 * len - 4.0 * spread + 28 + 16 * crc) / (4.0 * spread)) * (coding_rate + 4);
    if (sym < 0) sym = 0;
    return (uint32_t)ceil((npre + 4.25) * time_spec + (8 + sym) * time_spec);
}

bool lora_radio_init(void)
{
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)(BASE_PORT + self));
    if (0 != bind(sock, (struct sockaddr*)&addr, sizeof(addr))) { perror("bind"); return false; }
    fcntl(sock, F_SETFL, O_NONBLOCK);
    const char* p_loss_env = getenv("LORA_LOSS");
    loss = p_loss_env ? atof(p_loss_env) : 0.0;
    srand((unsigned)(self * 7919 + (unsigned)now()));
    printf("fake LR1121: node %d on udp %d, loss %.0f%%\n", self, BASE_PORT + self, loss * 100);
    return true;
}

bool lora_radio_start_tx(const uint8_t* p_buf, uint8_t len)
{
    if (b_tx_on || (0 == len)) return false;
    memcpy(tx_buf, p_buf, len);
    tx_len = len;
    b_tx_on = true;
    tx_start = now();
    tx_end = tx_start + lora_radio_time_on_air_ms(len);
    return true;
}

bool lora_radio_tx_active(void) { return b_tx_on; }

lora_radio_event_t lora_radio_service(uint8_t* p_buf, uint8_t* p_len, int8_t* p_rssi, int8_t* p_snr)
{
    if (b_tx_on && ((int32_t)(now() - tx_end) >= 0))
    {
        for (int node = 1; node <= MAX_NODES; node++)
        {
            if (node == self) continue;
            struct sockaddr_in addr = {0};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons((uint16_t)(BASE_PORT + node));
            sendto(sock, tx_buf, tx_len, 0, (struct sockaddr*)&addr, sizeof(addr));
        }
        b_tx_on = false;
        return LORA_EVT_TX_DONE;
    }
    uint8_t tmp[256];
    ssize_t ret = recv(sock, tmp, sizeof(tmp), 0);
    if (ret <= 0) return LORA_EVT_NONE;
    if (b_tx_on) return LORA_EVT_RX_ERROR;                 /* half duplex: we were talking */
    if ((float64_t)rand() / RAND_MAX < loss) return LORA_EVT_NONE; /* lost in the air */
    memcpy(p_buf, tmp, (size_t)ret);
    *p_len = (uint8_t)ret;
    *p_rssi = -60;
    *p_snr = 9;
    return LORA_EVT_RX_DONE;
}
