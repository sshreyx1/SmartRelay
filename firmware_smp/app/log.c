#include "log.h"

#include <string.h>

#include "rtos_port.h"

/* Round 2 review fix A5: raised from 16 - print_stats() alone writes about
 * a dozen lines in one burst and the USB-owner task only drains every
 * ~20 ms, so 16 was close to guaranteed to drop part of every "s" response. */
#define LOG_RING_DEPTH 64

/* Per-task partial lines for log_putc(). One slot per task that has printed (the node firmware
 * has three tasks, plus the kernel's own); slot 0 is shared by interrupt-handler output. */
#define LOG_ASM_SLOTS 8

static rtos_id_t s_log_q = -1;
/* Round 2 review fix A4: sized with RTOS_MSGQ_BYTES, not LOG_LINE_MAX*DEPTH -
 * see rtos_port.h for why the smaller size was an under-allocation (real
 * buffer overflow risk once the kernel is told bufsz is the larger value). */
static uint8_t s_log_buf[RTOS_MSGQ_BYTES(LOG_LINE_MAX, LOG_RING_DEPTH)];
static volatile uint32_t s_dropped;
/* Lines in the ring now: +1 per line queued, -1 per line drained. The kernel message buffer has no
 * count call; this lets the bridge's uplink send only what fits (log_free_slots). */
static volatile uint32_t s_queued;
static uint32_t s_wait_ms = 500u;
static volatile int32_t s_drainer;

typedef struct
{
    volatile int32_t owner; /* task id, 0 = free; slot 0 is reserved for handler context */
    uint32_t len;
    char buf[LOG_LINE_MAX];
} log_asm_t;

static log_asm_t s_asm[LOG_ASM_SLOTS];

void log_init(void)
{
    s_log_q = rtos_msgq_create(LOG_LINE_MAX, LOG_RING_DEPTH, s_log_buf);
}

bool log_selftest(char* p_out_line, uint32_t out_line_max)
{
    return rtos_msgq_selftest(s_log_q, LOG_LINE_MAX, LOG_RING_DEPTH, p_out_line, out_line_max);
}

void log_set_wait_ms(uint32_t msec)
{
    s_wait_ms = msec;
}

void log_set_drainer(int32_t task_id)
{
    s_drainer = task_id;
}

static void queue_line(const char* p_msg, bool b_may_wait)
{
    bool b_is_ok = b_may_wait ? rtos_msgq_send_wait(s_log_q, p_msg, LOG_LINE_MAX, (int32_t)s_wait_ms)
                       : rtos_msgq_send_nowait(s_log_q, p_msg, LOG_LINE_MAX);
    if (b_is_ok)
    {
        rtos_atomic_add_u32(&s_queued, 1);
    }
    else
    {
        /* log_write_line runs on comms (core 0), sensors (core 1), and
         * usb_owner (core 0) - round 2 review fix A8. */
        rtos_atomic_add_u32(&s_dropped, 1);
    }
}

void log_write_line(const char* p_line)
{
    char msg[LOG_LINE_MAX];
    size_t len = strlen(p_line);
    if (len >= sizeof(msg))
    {
        len = sizeof(msg) - 1;
    }
    memcpy(msg, p_line, len);
    msg[len] = '\0';
    queue_line(msg, false);
}

static log_asm_t* slot_for(rtos_id_t self_id)
{
    if (self_id == 0)
    {
        return &s_asm[0];
    }
    for (unsigned idx = 1; idx < LOG_ASM_SLOTS; idx++)
    {
        if (s_asm[idx].owner == self_id)
        {
            return &s_asm[idx];
        }
    }
    for (unsigned idx = 1; idx < LOG_ASM_SLOTS; idx++)
    {
        if (rtos_atomic_cas_i32(&s_asm[idx].owner, 0, self_id))
        {
            s_asm[idx].len = 0;
            return &s_asm[idx];
        }
    }
    return NULL; /* more printing tasks than slots: their output is dropped and counted */
}

void log_putc(char chr)
{
    rtos_id_t self_id = rtos_self_id();
    log_asm_t* p_line_asm = slot_for(self_id);
    if (p_line_asm == NULL)
    {
        if (chr == '\n')
        {
            rtos_atomic_add_u32(&s_dropped, 1);
        }
        return;
    }
    if (chr == '\r')
    {
        return; /* lines go out as "...\r\n" anyway (usb_owner) */
    }
    if (chr != '\n')
    {
        p_line_asm->buf[p_line_asm->len++] = chr;
        if (p_line_asm->len < LOG_LINE_MAX - 1u)
        {
            return;
        }
        /* longer than a ring slot: send what fits, continue on the next line */
    }
    p_line_asm->buf[p_line_asm->len] = '\0';
    p_line_asm->len = 0;
    queue_line(p_line_asm->buf, self_id != 0 && self_id != s_drainer);
}

uint32_t log_dropped_count(void)
{
    return s_dropped;
}

bool log_drain_one(char out[LOG_LINE_MAX], int32_t timeout_ms)
{
    bool b_is_ok = rtos_msgq_recv(s_log_q, out, LOG_LINE_MAX, timeout_ms);
    if (b_is_ok)
    {
        rtos_atomic_add_u32(&s_queued, (uint32_t)-1);
    }
    return b_is_ok;
}

uint32_t log_free_slots(void)
{
    /* the drain can run between a send and its +1, so s_queued may briefly read one low or wrap
     * below zero; clamp rather than trust it exactly */
    uint32_t queue = s_queued;
    return queue < LOG_RING_DEPTH ? LOG_RING_DEPTH - queue : 0u;
}
