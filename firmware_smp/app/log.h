/*
 * log.h - CURVEBALL 3.6: comms and sensors never call the console directly.
 * They format a line (tm_sprintf - no va_list forwarder exists in this
 * port's tm library, see docs/curveball_findings.md) and hand it here. The
 * USB-owner task is the only task that ever touches the console.
 */
#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdbool.h>
#include <stdint.h>

#define LOG_LINE_MAX 160

void log_init(void);

/* Round 2 review fix A4: call once, right after log_init(), before any
 * log_write_line - confirms the ring really holds LOG_RING_DEPTH lines, no
 * more, no less, and leaves it empty afterward. */
bool log_selftest(char* out_line, uint32_t out_line_max);

/* Never blocks. Drops the line and counts it if the ring is full - a
 * stalled terminal/dashboard must never stall comms or sensors (3.6). */
void log_write_line(const char* line);

uint32_t log_dropped_count(void);

/* USB-owner task only: blocks up to timeout_ms for the next line. */
bool log_drain_one(char out[LOG_LINE_MAX], int32_t timeout_ms);

#endif
