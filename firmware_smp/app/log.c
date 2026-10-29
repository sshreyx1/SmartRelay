#include "log.h"

#include <string.h>

#include "rtos_port.h"

/* Round 2 review fix A5: raised from 16 - print_stats() alone writes about
 * a dozen lines in one burst and the USB-owner task only drains every
 * ~20 ms, so 16 was close to guaranteed to drop part of every "s" response. */
#define LOG_RING_DEPTH 64

static rtos_id_t s_log_q = -1;
/* Round 2 review fix A4: sized with RTOS_MSGQ_BYTES, not LOG_LINE_MAX*DEPTH -
 * see rtos_port.h for why the smaller size was an under-allocation (real
 * buffer overflow risk once the kernel is told bufsz is the larger value). */
static uint8_t s_log_buf[RTOS_MSGQ_BYTES(LOG_LINE_MAX, LOG_RING_DEPTH)];
static volatile uint32_t s_dropped;

void log_init(void)
{
    s_log_q = rtos_msgq_create(LOG_LINE_MAX, LOG_RING_DEPTH, s_log_buf);
}

bool log_selftest(char* out_line, uint32_t out_line_max)
{
    return rtos_msgq_selftest(s_log_q, LOG_LINE_MAX, LOG_RING_DEPTH, out_line, out_line_max);
}

void log_write_line(const char* line)
{
    char msg[LOG_LINE_MAX];
    size_t n = strlen(line);
    if (n >= sizeof(msg))
    {
        n = sizeof(msg) - 1;
    }
    memcpy(msg, line, n);
    msg[n] = '\0';
    if (!rtos_msgq_send_nowait(s_log_q, msg, LOG_LINE_MAX))
    {
        /* log_write_line runs on comms (core 0), sensors (core 1), and
         * usb_owner (core 0) - round 2 review fix A8. */
        rtos_atomic_add_u32(&s_dropped, 1);
    }
}

uint32_t log_dropped_count(void)
{
    return s_dropped;
}

bool log_drain_one(char out[LOG_LINE_MAX], int32_t timeout_ms)
{
    return rtos_msgq_recv(s_log_q, out, LOG_LINE_MAX, timeout_ms);
}
