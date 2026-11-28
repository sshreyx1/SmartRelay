/*
 * log.h - CURVEBALL 3.6: comms and sensors never call the console directly. Every line goes
 * through one message-buffer ring, and the USB-owner task is the only task that ever touches the
 * console.
 *
 * Two ways in:
 *   log_write_line()  one finished line, never blocks (drops and counts if the ring is full).
 *   log_putc()        byte stream (printf/puts/putchar land here, via pico_runtime_mtk3.c's
 *                     smartrelay_stdout_putc). Bytes are collected per task until '\n', so a line
 *                     is queued whole and output from the two cores never interleaves mid-line.
 *                     A finished line waits up to log_set_wait_ms() for room in the ring (the
 *                     FreeRTOS build's USB stdio waits the same way), so a burst like the
 *                     bridge's base64 uplink of a whole image is not cut short; a host that
 *                     stops reading still cannot stall a task for longer than that per line.
 */
#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdbool.h>
#include <stdint.h>

/* Longest line, including the NUL. The bridge's "@ALFP FILE ... meta=<hex>" uplink header is up
 * to ~210 characters (common/uplink.c, 64-byte meta), so 160 was too short. */
#define LOG_LINE_MAX 256

void log_init(void);

/* Round 2 review fix A4: call once, right after log_init(), before any
 * log_write_line - confirms the ring really holds LOG_RING_DEPTH lines, no
 * more, no less, and leaves it empty afterward. */
bool log_selftest(char* p_out_line, uint32_t out_line_max);

/* Never blocks. Drops the line and counts it if the ring is full - a
 * stalled terminal/dashboard must never stall comms or sensors (3.6). */
void log_write_line(const char* p_line);

/* See the header comment. Safe from any task on either core; from an interrupt handler the line
 * is queued without waiting. */
void log_putc(char chr);

/* How long a finished log_putc() line may wait for room (default 500 ms). */
void log_set_wait_ms(uint32_t msec);

/* The task that drains the ring: its own output is never made to wait for itself. */
void log_set_drainer(int32_t task_id);

uint32_t log_dropped_count(void);

/* Lines the ring can take right now without waiting (approximate by at most one line while
 * another task is mid-send). */
uint32_t log_free_slots(void);

/* USB-owner task only: blocks up to timeout_ms for the next line. */
bool log_drain_one(char out[LOG_LINE_MAX], int32_t timeout_ms);

#endif
