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
uint8_t wst_crc8(const uint8_t *p_data, uint32_t len, uint8_t crc) {
    while (len--) {
        crc ^= *p_data++;
        for (int idx = 0; idx < 8; idx++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

/* ---------- RX ring buffer (filled by interrupt) ---------- */
#define RXBUF_SIZE 2048            /* power of two */
static volatile uint8_t  rxbuf[RXBUF_SIZE];
static volatile uint16_t rx_head;
static volatile uint16_t rx_tail;

/* volatile (BARR-C 1.8.c): the UART RX ISR below writes uart_overflow,
 * rx_isr_core and rx_high_watermark, and task code reads the whole struct. */
static volatile wst_link_stats_t stats;

/* CURVEBALL 3.4: on an SMP build the ISR and wst_link_poll() both run on
 * core 0 (comms_task owns the UART and enables this IRQ from there, so the
 * IRQ is taken on the core that enabled it) -- there is no cross-core race
 * on this ring, but the compiler barrier still stops the store to rxbuf[]
 * from being reordered past the index publish, and logging the core once
 * gives T2 something to check against. */
static volatile bool b_s_rx_isr_core_logged;

static void wst_uart_rx_isr(void) {
    if (!b_s_rx_isr_core_logged) {
        b_s_rx_isr_core_logged = true;
        /* same register get_core_num() reads, but through the SDK's volatile io_ro_32 field so
         * it is always one 32-bit load; cast is safe, CPUID is 0 or 1 */
        stats.rx_isr_core = (uint8_t)sio_hw->cpuid;
    }
    while (uart_is_readable(WST_UART)) {
        uint8_t byte = (uint8_t)uart_getc(WST_UART);
        uint16_t next = (rx_head + 1) & (RXBUF_SIZE - 1);
        if (next == rx_tail) { stats.uart_overflow++; continue; }
        rxbuf[rx_head] = byte;
        __asm volatile("" ::: "memory"); /* data write before index publish */
        rx_head = next;
        uint16_t fill = (uint16_t)((rx_head - rx_tail) & (RXBUF_SIZE - 1));
        if (fill > stats.rx_high_watermark) {
            stats.rx_high_watermark = fill;
        }
    }
}

/* ---------- Received-packet queue (for the mesh to read) ---------- */
static wst_rx_pkt_t rxq[WST_RX_QUEUE];
static uint8_t rxq_head;
static uint8_t rxq_tail;

/* ---------- Transmit queue (complete frames ready to write) ---------- */
typedef struct {
    uint16_t len;
    uint8_t  bytes[WST_MAX_PKT + 4];   /* AA, type, len, packet, crc */
} tx_frame_t;
static tx_frame_t txq[WST_TX_QUEUE];
static uint8_t  txq_head;
static uint8_t txq_tail;
static uint8_t txq_count;
static uint16_t tx_pos;          /* how much of the front frame is written  */
static bool     b_tx_waiting;      /* front frame written, waiting for TXDONE */
static uint64_t tx_wait_start;

/* Small out-of-band frame (WHOAMI) that skips the queue */
static uint8_t  ctl_frame[3];
static uint8_t  ctl_len;
static uint8_t ctl_pos;

/* ---------- Our MAC ---------- */
static uint8_t my_mac[6];
static bool    b_have_mac;

/* ---------- Frame parser state ---------- */
enum { P_SOF, P_TYPE, P_LEN, P_MAC, P_BODY, P_STATUS, P_CRC };
static uint8_t  state = P_SOF;
static uint8_t  type;
static uint8_t len;
static uint8_t pos;
static uint8_t status;
static uint8_t  mac[6];
static uint8_t  body[WST_MAX_PKT];
static uint64_t start;

static void frame_done(void) {
    stats.rx_ok++;
    switch (type) {
    case T_RECV: {
        uint8_t next = (rxq_head + 1) % WST_RX_QUEUE;
        if (next == rxq_tail) { stats.rx_queue_full++; break; }
        memcpy(rxq[rxq_head].mac, mac, 6);
        rxq[rxq_head].len = len;
        memcpy(rxq[rxq_head].data, body, len);
        rxq_head = next;
        break;
    }
    case T_TXDONE:
        if (b_tx_waiting) {
            if (0 == status) stats.tx_ok++; else stats.tx_fail++;
            b_tx_waiting = false;
            txq_tail = (txq_tail + 1) % WST_TX_QUEUE;
            txq_count--;
            tx_pos = 0;
        }
        break;
    case T_HELLO:
        memcpy(my_mac, mac, 6);
        b_have_mac = true;
        break;
    default:
        /* parse_byte() only accepts the three types above; nothing to do. */
        break;
    }
}

static void parse_byte(uint8_t byte) {
    switch (state) {
    case P_SOF:
        if (SOF == byte) { state = P_TYPE; start = time_us_64(); }
        break;
    case P_TYPE:
        type = byte; pos = 0;
        if      (T_RECV == byte)   state = P_LEN;
        else if (T_TXDONE == byte) state = P_STATUS;
        else if (T_HELLO == byte)  { len = 0; state = P_MAC; }
        else                    state = P_SOF;          /* unknown: resync */
        break;
    case P_LEN:
        if ((0 == byte) || (byte > WST_MAX_PKT)) { state = P_SOF; break; }
        len = byte; state = P_MAC;
        break;
    case P_MAC:
        mac[pos++] = byte;
        if (6 == pos) { pos = 0; state = (len ? P_BODY : P_CRC); }
        break;
    case P_BODY:
        body[pos++] = byte;
        if (pos == len) state = P_CRC;
        break;
    case P_STATUS:
        status = byte; state = P_CRC;
        break;
    case P_CRC: {
        uint8_t crc = wst_crc8(&type, 1, 0);
        if (T_RECV == type) {
            crc = wst_crc8(&len, 1, crc);
            crc = wst_crc8(mac, 6, crc);
            crc = wst_crc8(body, len, crc);
        } else if (T_TXDONE == type) {
            crc = wst_crc8(&status, 1, crc);
        } else if (T_HELLO == type) {
            crc = wst_crc8(mac, 6, crc);
        } else {
            /* P_TYPE never lets another type reach P_CRC; CRC covers the type byte only. */
        }
        if (crc == byte) frame_done(); else stats.rx_crc_err++;
        state = P_SOF;
        break;
    }
    default:
        /* p_state only ever holds the P_* states above; nothing to do. */
        break;
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

    irq_set_exclusive_handler(WST_UART_IRQ, wst_uart_rx_isr);
    irq_set_enabled(WST_UART_IRQ, true);
    uart_set_irq_enables(WST_UART, true, false);   /* RX interrupt only */

    wst_link_request_mac();
}

bool wst_link_send(const uint8_t *p_pkt, uint8_t len) {
    if ((0 == len) || (len > WST_MAX_PKT)) return false;
    if (WST_TX_QUEUE == txq_count) { stats.tx_queue_full++; return false; }

    tx_frame_t *p_frame = &txq[txq_head];
    p_frame->bytes[0] = SOF;
    p_frame->bytes[1] = T_SEND;
    p_frame->bytes[2] = len;
    memcpy(&p_frame->bytes[3], p_pkt, len);
    p_frame->bytes[3 + len] = wst_crc8(&p_frame->bytes[1], 2 + len, 0);
    p_frame->len = 4 + len;

    txq_head = (txq_head + 1) % WST_TX_QUEUE;
    txq_count++;
    stats.tx_queued++;
    return true;
}

void wst_link_poll(void) {
    /* 1. Rebuild frames from bytes the interrupt collected */
    while (rx_tail != rx_head) {
        uint8_t byte = rxbuf[rx_tail];
        __asm volatile("" ::: "memory"); /* data read before index publish */
        rx_tail = (rx_tail + 1) & (RXBUF_SIZE - 1);
        parse_byte(byte);
    }
    if ((P_SOF != state) && (time_us_64() - start > FRAME_TIMEOUT_US))
        state = P_SOF;                        /* half frame went stale */

    /* 2. Control frame (WHOAMI) jumps ahead, but never mid-way through
          another frame, or the WST would see garbage */
    if ((ctl_pos < ctl_len) && (0 == tx_pos)) {
        while ((ctl_pos < ctl_len) && (uart_is_writable(WST_UART)))
            uart_putc_raw(WST_UART, ctl_frame[ctl_pos++]);
        if (ctl_pos < ctl_len) return;
    }

    /* 3. Waiting for the WST to confirm the last send? */
    if (b_tx_waiting) {
        if (time_us_64() - tx_wait_start > TXDONE_TIMEOUT_US) {
            stats.tx_timeout++;                 /* WST never answered: move on */
            b_tx_waiting = false;
            txq_tail = (txq_tail + 1) % WST_TX_QUEUE;
            txq_count--;
            tx_pos = 0;
        } else {
            return;
        }
    }

    /* 4. Feed the front frame into the UART, as much as fits right now */
    if (0 == txq_count) return;
    tx_frame_t *p_frame = &txq[txq_tail];
    while ((tx_pos < p_frame->len) && (uart_is_writable(WST_UART)))
        uart_putc_raw(WST_UART, p_frame->bytes[tx_pos++]);
    if (tx_pos == p_frame->len) {
        b_tx_waiting = true;
        tx_wait_start = time_us_64();
    }
}

bool wst_link_recv(wst_rx_pkt_t *p_out) {
    if (rxq_tail == rxq_head) return false;
    *p_out = rxq[rxq_tail];
    rxq_tail = (rxq_tail + 1) % WST_RX_QUEUE;
    return true;
}

bool wst_link_tx_busy(void) {
    return (txq_count > 0) || b_tx_waiting;
}

bool wst_link_get_mac(uint8_t mac[6]) {
    if (b_have_mac) memcpy(mac, my_mac, 6);
    return b_have_mac;
}

void wst_link_request_mac(void) {
    ctl_frame[0] = SOF;
    ctl_frame[1] = T_WHOAMI;
    ctl_frame[2] = wst_crc8(&ctl_frame[1], 1, 0);
    ctl_len = 3;
    ctl_pos = 0;
}

void wst_link_get_stats(wst_link_stats_t *p_out) {
    *p_out = stats;
}
