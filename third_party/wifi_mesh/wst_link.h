/*
 * wst_link.h - Pico-side driver for the WiFi Serial Transceiver (WST)
 * CSC09 SmartRelay - WiFi mesh, layer 1 (the "wire" between Pico and WST)
 *
 * What this does:
 *   The WST is plugged into the Maker Pi Pico ESP-01 socket (UART0, GP16/GP17).
 *   This driver wraps packets in frames, sends them to the WST, and unwraps
 *   frames coming back from the WST. It knows nothing about the mesh.
 *
 * Rules (match the Radio Interface Contract):
 *   - Nothing here ever blocks. wst_link_send() only queues.
 *   - Call wst_link_poll() often (every loop / every few ms). It moves bytes
 *     in and out of the UART.
 *
 * Frame format (same as wst_bridge.ino):
 *   Pico -> WST   SEND    [AA][01][len][packet][crc]
 *                 WHOAMI  [AA][05][crc]
 *   WST  -> Pico  RECV    [AA][02][len][mac x6][packet][crc]
 *                 TXDONE  [AA][03][status][crc]   0 = ok, 1 = fail
 *                 HELLO   [AA][04][mac x6][crc]
 *   CRC-8, poly 0x07, init 0, over everything after the AA.
 */
#ifndef WST_LINK_H
#define WST_LINK_H

#include <stdbool.h>
#include <stdint.h>

#define WST_MAX_PKT  250 /* biggest packet ESP-NOW can carry      */
#define WST_TX_QUEUE 8   /* packets waiting to go to the WST      */
#define WST_RX_QUEUE 8   /* packets waiting for the mesh to read  */

/* One packet heard over the air */
typedef struct
{
    uint8_t mac[6]; /* sender's WST MAC -> gives prev_hop */
    uint8_t len;
    uint8_t data[WST_MAX_PKT];
} wst_rx_pkt_t;

/* Counters, useful for get_stats() and the report */
typedef struct
{
    uint32_t tx_queued;     /* packets accepted by wst_link_send        */
    uint32_t tx_ok;         /* WST said "sent"                          */
    uint32_t tx_fail;       /* WST said "failed"                        */
    uint32_t tx_timeout;    /* WST never answered                       */
    uint32_t tx_queue_full; /* send() refused because queue was full    */
    uint32_t rx_ok;         /* good frames received                     */
    uint32_t rx_crc_err;    /* frames thrown away for bad CRC           */
    uint32_t rx_queue_full; /* packets lost because nobody read them    */
    uint32_t uart_overflow; /* bytes lost because poll() was too slow   */
    /* peak RX ring fill, bytes of RXBUF_SIZE (CURVEBALL 3.4) */
    uint16_t rx_high_watermark;
    /* core the UART0 RX IRQ ran on, logged once (CURVEBALL 3.4) */
    uint8_t rx_isr_core;
} wst_link_stats_t;

/* Set up UART0 on GP16/GP17 at 115200 and ask the WST for its MAC. */
void wst_link_init(void);

/* Queue one packet to broadcast. Returns false if too big or queue full. */
bool wst_link_send(const uint8_t* p_pkt, uint8_t len);

/* Move bytes in/out of the UART. Call this very often. */
void wst_link_poll(void);

/* Get the next received packet. Returns false if nothing waiting. */
bool wst_link_recv(wst_rx_pkt_t* p_out);

/* True while packets are still queued or the WST is still sending. */
bool wst_link_tx_busy(void);

/* Our own WST's MAC. Returns false until the WST has said HELLO. */
bool wst_link_get_mac(uint8_t mac[6]);

/* Ask the WST "who are you?" again (answer arrives via poll). */
void wst_link_request_mac(void);

/* Copy out the counters. */
void wst_link_get_stats(wst_link_stats_t* p_out);

/* CRC-8 used on the wire (exposed so tests and Python can match it). */
uint8_t wst_crc8(const uint8_t* p_data, uint32_t len, uint8_t crc);

#endif

/*** end of file ***/

