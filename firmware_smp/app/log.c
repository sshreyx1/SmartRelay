#include "log.h"

#include <string.h>

#include "rtos_port.h"

#define LOG_RING_DEPTH 16

static rtos_id_t s_log_q = -1;
static uint8_t s_log_buf[LOG_RING_DEPTH][LOG_LINE_MAX];
static uint32_t s_dropped;

void log_init(void)
{
    s_log_q = rtos_msgq_create(LOG_LINE_MAX, LOG_RING_DEPTH, s_log_buf);
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
        s_dropped++;
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
