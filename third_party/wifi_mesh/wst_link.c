/*
 * wst_link.c - Pico-side driver for the WiFi Serial Transceiver (WST)
 * See wst_link.h for the big picture.
 *
 * How it works, in short:
 *   RX: a UART interrupt grabs every incoming byte into a ring buffer, so no
 *       byte is lost even if the rest of the program is busy. poll() then
 *       reads that buffer and rebuilds frames, checking the CRC.
 *   TX: send() copies the packet into a queue. poll() feeds one frame at a
 *       time into the UART as space allows, then waits for the WST's TXDONE
 *       before starting the next one (only one radio send in flight).
 */
#include "wst_link.h"

#include <string.h>
#include "pico/stdlib.h"
#include "pico/platform.h" /* get_core_num(), CURVEBALL 3.4 */
#include "hardware/uart.h"
#include "hardware/irq.h"

/* ---------- Pins (Maker Pi Pico ESP-01 socket) ---------- */
#define WST_UART        uart0
#define WST_UART_IRQ    UART0_IRQ
#define WST_TX_PIN      16      /* Pico TX -> WST RX */
#define WST_RX_PIN      17      /* Pico RX <- WST TX */
#define WST_BAUD        115200

/* ---------- Frame constants ---------- */
#define SOF       0xAA
#define T_SEND    0x01
#define T_RECV    0x02
#define T_TXDONE  0x03
#define T_HELLO   0x04
#define T_WHOAMI  0x05

#define TXDONE_TIMEOUT_US   100000   /* give up waiting for TXDONE after 100 ms */
#define FRAME_TIMEOUT_US     50000   /* drop a half-received frame after 50 ms  */

/* ---------- CRC-8 (poly 0x07) ---------- */
uint8_t wst_crc8(const uint8_t *d, uint32_t n, uint8_t c) {
    while (n--) {
        c ^= *d++;
        for (int i = 0; i < 8; i++)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}

/* ---------- RX ring buffer (filled by interrupt) ---------- */
#define RXBUF_SIZE 2048            /* power of two */
static volatile uint8_t  rxbuf[RXBUF_SIZE];
static volatile uint16_t rx_head, rx_tail;

static wst_link_stats_t stats;

/* CURVEBALL 3.4: on an SMP build the ISR and wst_link_poll() both run on
 * core 0 (comms_task owns the UART and enables this IRQ from there, so the
 * IRQ is taken on the core that enabled it) -- there is no cross-core race
 * on this ring, but the compiler barrier still stops the store to rxbuf[]
 * from being reordered past the index publish, and logging the core once
 * gives T2 something to check against. */
static volatile bool s_rx_isr_core_logged;

static void on_uart_rx(void) {
    if (!s_rx_isr_core_logged) {
        s_rx_isr_core_logged = true;
        stats.rx_isr_core = (uint8_t)get_core_num();
    }
    while (uart_is_readable(WST_UART)) {
        uint8_t b = (uint8_t)uart_getc(WST_UART);
        uint16_t next = (rx_head + 1) & (RXBUF_SIZE - 1);
        if (next == rx_tail) { stats.uart_overflow++; continue; }
        rxbuf[rx_head] = b;
        __asm volatile("" ::: "memory"); /* data write before index publish */
        rx_head = next;
        uint16_t fill = (uint16_t)((rx_head - rx_tail) & (RXBUF_SIZE - 1));
        if (fill > stats.rx_high_watermark) stats.rx_high_watermark = fill;
    }
}

/* ---------- Received-packet queue (for the mesh to read) ---------- */
static wst_rx_pkt_t rxq[WST_RX_QUEUE];
static uint8_t rxq_head, rxq_tail;

/* ---------- Transmit queue (complete frames ready to write) ---------- */
typedef struct {
    uint16_t len;
    uint8_t  bytes[WST_MAX_PKT + 4];   /* AA, type, len, packet, crc */
} tx_frame_t;
static tx_frame_t txq[WST_TX_QUEUE];
static uint8_t  txq_head, txq_tail, txq_count;
static uint16_t tx_pos;          /* how much of the front frame is written  */
static bool     tx_waiting;      /* front frame written, waiting for TXDONE */
static uint64_t tx_wait_start;

/* Small out-of-band frame (WHOAMI) that skips the queue */
static uint8_t  ctl_frame[3];
static uint8_t  ctl_len, ctl_pos;

/* ---------- Our MAC ---------- */
static uint8_t my_mac[6];
static bool    have_mac;

/* ---------- Frame parser state ---------- */
enum { P_SOF, P_TYPE, P_LEN, P_MAC, P_BODY, P_STATUS, P_CRC };
static uint8_t  p_state = P_SOF;
static uint8_t  p_type, p_len, p_pos, p_status;
static uint8_t  p_mac[6];
static uint8_t  p_body[WST_MAX_PKT];
static uint64_t p_start;

static void frame_done(void) {
    stats.rx_ok++;
    switch (p_type) {
    case T_RECV: {
        uint8_t next = (rxq_head + 1) % WST_RX_QUEUE;
        if (next == rxq_tail) { stats.rx_queue_full++; break; }
        memcpy(rxq[rxq_head].mac, p_mac, 6);
        rxq[rxq_head].len = p_len;
        memcpy(rxq[rxq_head].data, p_body, p_len);
        rxq_head = next;
        break;
    }
    case T_TXDONE:
        if (tx_waiting) {
            if (p_status == 0) stats.tx_ok++; else stats.tx_fail++;
            tx_waiting = false;
            txq_tail = (txq_tail + 1) % WST_TX_QUEUE;
            txq_count--;
            tx_pos = 0;
        }
        break;
    case T_HELLO:
        memcpy(my_mac, p_mac, 6);
        have_mac = true;
        break;
    }
}

static void parse_byte(uint8_t b) {
    switch (p_state) {
    case P_SOF:
        if (b == SOF) { p_state = P_TYPE; p_start = time_us_64(); }
        break;
    case P_TYPE:
        p_type = b; p_pos = 0;
        if      (b == T_RECV)   p_state = P_LEN;
        else if (b == T_TXDONE) p_state = P_STATUS;
        else if (b == T_HELLO)  { p_len = 0; p_state = P_MAC; }
        else                    p_state = P_SOF;          /* unknown: resync */
        break;
    case P_LEN:
        if (b == 0 || b > WST_MAX_PKT) { p_state = P_SOF; break; }
        p_len = b; p_state = P_MAC;
        break;
    case P_MAC:
        p_mac[p_pos++] = b;
        if (p_pos == 6) { p_pos = 0; p_state = (p_len ? P_BODY : P_CRC); }
        break;
    case P_BODY:
        p_body[p_pos++] = b;
        if (p_pos == p_len) p_state = P_CRC;
        break;
    case P_STATUS:
        p_status = b; p_state = P_CRC;
        break;
    case P_CRC: {
        uint8_t c = wst_crc8(&p_type, 1, 0);
        if (p_type == T_RECV) {
            c = wst_crc8(&p_len, 1, c);
            c = wst_crc8(p_mac, 6, c);
            c = wst_crc8(p_body, p_len, c);
        } else if (p_type == T_TXDONE) {
            c = wst_crc8(&p_status, 1, c);
        } else if (p_type == T_HELLO) {
            c = wst_crc8(p_mac, 6, c);
        }
        if (c == b) frame_done(); else stats.rx_crc_err++;
        p_state = P_SOF;
        break;
    }
    }
}

/* ---------- Public API ---------- */
void wst_link_init(void) {
    uart_init(WST_UART, WST_BAUD);
    gpio_set_function(WST_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(WST_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(WST_UART, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(WST_UART, false, false);
    uart_set_fifo_enabled(WST_UART, true);

    irq_set_exclusive_handler(WST_UART_IRQ, on_uart_rx);
    irq_set_enabled(WST_UART_IRQ, true);
    uart_set_irq_enables(WST_UART, true, false);   /* RX interrupt only */

    wst_link_request_mac();
}

bool wst_link_send(const uint8_t *pkt, uint8_t len) {
    if (len == 0 || len > WST_MAX_PKT) return false;
    if (txq_count == WST_TX_QUEUE) { stats.tx_queue_full++; return false; }

    tx_frame_t *f = &txq[txq_head];
    f->bytes[0] = SOF;
    f->bytes[1] = T_SEND;
    f->bytes[2] = len;
    memcpy(&f->bytes[3], pkt, len);
    f->bytes[3 + len] = wst_crc8(&f->bytes[1], 2 + len, 0);
    f->len = 4 + len;

    txq_head = (txq_head + 1) % WST_TX_QUEUE;
    txq_count++;
    stats.tx_queued++;
    return true;
}

void wst_link_poll(void) {
    /* 1. Rebuild frames from bytes the interrupt collected */
    while (rx_tail != rx_head) {
        uint8_t b = rxbuf[rx_tail];
        __asm volatile("" ::: "memory"); /* data read before index publish */
        rx_tail = (rx_tail + 1) & (RXBUF_SIZE - 1);
        parse_byte(b);
    }
    if (p_state != P_SOF && time_us_64() - p_start > FRAME_TIMEOUT_US)
        p_state = P_SOF;                        /* half frame went stale */

    /* 2. Control frame (WHOAMI) jumps ahead, but never mid-way through
          another frame, or the WST would see garbage */
    if (ctl_pos < ctl_len && tx_pos == 0) {
        while (ctl_pos < ctl_len && uart_is_writable(WST_UART))
            uart_putc_raw(WST_UART, ctl_frame[ctl_pos++]);
        if (ctl_pos < ctl_len) return;
    }

    /* 3. Waiting for the WST to confirm the last send? */
    if (tx_waiting) {
        if (time_us_64() - tx_wait_start > TXDONE_TIMEOUT_US) {
            stats.tx_timeout++;                 /* WST never answered: move on */
            tx_waiting = false;
            txq_tail = (txq_tail + 1) % WST_TX_QUEUE;
            txq_count--;
            tx_pos = 0;
        } else {
            return;
        }
    }

    /* 4. Feed the front frame into the UART, as much as fits right now */
    if (txq_count == 0) return;
    tx_frame_t *f = &txq[txq_tail];
    while (tx_pos < f->len && uart_is_writable(WST_UART))
        uart_putc_raw(WST_UART, f->bytes[tx_pos++]);
    if (tx_pos == f->len) {
        tx_waiting = true;
        tx_wait_start = time_us_64();
    }
}

bool wst_link_recv(wst_rx_pkt_t *out) {
    if (rxq_tail == rxq_head) return false;
    *out = rxq[rxq_tail];
    rxq_tail = (rxq_tail + 1) % WST_RX_QUEUE;
    return true;
}

bool wst_link_tx_busy(void) {
    return txq_count > 0 || tx_waiting;
}

bool wst_link_get_mac(uint8_t mac[6]) {
    if (have_mac) memcpy(mac, my_mac, 6);
    return have_mac;
}

void wst_link_request_mac(void) {
    ctl_frame[0] = SOF;
    ctl_frame[1] = T_WHOAMI;
    ctl_frame[2] = wst_crc8(&ctl_frame[1], 1, 0);
    ctl_len = 3;
    ctl_pos = 0;
}

void wst_link_get_stats(wst_link_stats_t *out) {
    *out = stats;
}
