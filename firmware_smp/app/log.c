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

bool log_selftest(char* out_line, uint32_t out_line_max)
{
    return rtos_msgq_selftest(s_log_q, LOG_LINE_MAX, LOG_RING_DEPTH, out_line, out_line_max);
}

void log_set_wait_ms(uint32_t ms)
{
    s_wait_ms = ms;
}

void log_set_drainer(int32_t task_id)
{
    s_drainer = task_id;
}

static void queue_line(const char* msg, bool may_wait)
{
    bool ok = may_wait ? rtos_msgq_send_wait(s_log_q, msg, LOG_LINE_MAX, (int32_t)s_wait_ms)
                       : rtos_msgq_send_nowait(s_log_q, msg, LOG_LINE_MAX);
    if (!ok)
    {
        /* log_write_line runs on comms (core 0), sensors (core 1), and
         * usb_owner (core 0) - round 2 review fix A8. */
        rtos_atomic_add_u32(&s_dropped, 1);
    }
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
    queue_line(msg, false);
}

static log_asm_t* slot_for(rtos_id_t me)
{
    if (me == 0)
    {
        return &s_asm[0];
    }
    for (unsigned i = 1; i < LOG_ASM_SLOTS; i++)
    {
        if (s_asm[i].owner == me)
        {
            return &s_asm[i];
        }
    }
    for (unsigned i = 1; i < LOG_ASM_SLOTS; i++)
    {
        if (rtos_atomic_cas_i32(&s_asm[i].owner, 0, me))
        {
            s_asm[i].len = 0;
            return &s_asm[i];
        }
    }
    return NULL; /* more printing tasks than slots: their output is dropped and counted */
}

void log_putc(char c)
{
    rtos_id_t me = rtos_self_id();
    log_asm_t* a = slot_for(me);
    if (a == NULL)
    {
        if (c == '\n')
        {
            rtos_atomic_add_u32(&s_dropped, 1);
        }
        return;
    }
    if (c == '\r')
    {
        return; /* lines go out as "...\r\n" anyway (usb_owner) */
    }
    if (c != '\n')
    {
        a->buf[a->len++] = c;
        if (a->len < LOG_LINE_MAX - 1u)
        {
            return;
        }
        /* longer than a ring slot: send what fits, continue on the next line */
    }
    a->buf[a->len] = '\0';
    a->len = 0;
    queue_line(a->buf, me != 0 && me != s_drainer);
}

uint32_t log_dropped_count(void)
{
    return s_dropped;
}

bool log_drain_one(char out[LOG_LINE_MAX], int32_t timeout_ms)
{
    return rtos_msgq_recv(s_log_q, out, LOG_LINE_MAX, timeout_ms);
}
