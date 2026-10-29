/*
 * rtos_port.h - the only header in firmware_smp/app that may know the kernel is uT-Kernel 3.
 * Application code calls these, never tk_* directly (CURVEBALL brief Step 3).
 *
 * Verified against third_party/mtk3smp_rp2040 commit 900e863 - see
 * docs/curveball_findings.md Step 2 for the citations behind every mapping in
 * rtos_port_mtk3.c. Nothing here has been compile-tested (no toolchain in this
 * environment); it is written from the cited source, not guessed.
 */
#ifndef RTOS_PORT_H
#define RTOS_PORT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*rtos_task_fn)(void* arg);

/* Numbered 0/1 at this layer; rtos_port_mtk3.c maps 0->TP_PRC1 (core 0),
 * 1->TP_PRC2 (core 1). Ignored entirely when built SMP=0 (see build/README). */
typedef enum
{
    RTOS_CORE_0 = 0,
    RTOS_CORE_1 = 1,
} rtos_core_t;

/* Smaller priority number = runs first, matching tk_cre_tsk directly
 * (MIN_TSKPRI=1 highest .. MAX_TSKPRI=32 lowest). Priority only matters
 * between tasks pinned to the same core (brief 3.1). Gaps left for later
 * insertions, per brief 3.3. */
#define RTOS_PRIO_COMMS 10
#define RTOS_PRIO_USB_OWNER 20
#define RTOS_PRIO_SENSORS 10 /* alone on core 1: value doesn't interact with core 0's */

#define RTOS_WAIT_FOREVER (-1)
#define RTOS_NO_WAIT 0

typedef int32_t rtos_id_t;

/* Returns a task id > 0, or a negative kernel error code. The task is
 * created and started (tk_cre_tsk + tk_sta_tsk); it must never return. */
rtos_id_t rtos_task_create(const char* name, rtos_task_fn fn, void* arg, unsigned priority,
                            unsigned stack_bytes, rtos_core_t core);

void rtos_delay_ms(uint32_t ms);

/* Event flags: one flag group holds up to 32 independent bits. */
rtos_id_t rtos_flags_create(void);
void rtos_flags_set(rtos_id_t flags, uint32_t bits);
void rtos_flags_clear(rtos_id_t flags, uint32_t bits);
/* Waits for ANY bit in mask; on success *out_bits holds the flags, pre-clear.
 * timeout_ms: RTOS_NO_WAIT, RTOS_WAIT_FOREVER, or a ms value. Returns false
 * on timeout. The matched bits are cleared atomically on return (TWF_CLR). */
bool rtos_flags_wait_any(rtos_id_t flags, uint32_t mask, uint32_t* out_bits, int32_t timeout_ms);

/* Fixed-size message buffer, msg_size bytes per message, count messages deep.
 * backing_buf must be caller-allocated static/global storage of at least
 * msg_size*count bytes that outlives the queue (the kernel's T_CMBF keeps a
 * bare pointer into it, not a copy - see rtos_port_mtk3.c). */
rtos_id_t rtos_msgq_create(uint32_t msg_size, uint32_t count, void* backing_buf);
/* Never blocks (TMO_POL). Returns false ("queue full") without sending. */
bool rtos_msgq_send_nowait(rtos_id_t q, const void* msg, uint32_t msg_size);
/* timeout_ms: RTOS_NO_WAIT, RTOS_WAIT_FOREVER, or a ms value. */
bool rtos_msgq_recv(rtos_id_t q, void* msg, uint32_t msg_size, int32_t timeout_ms);

/* Free-running millisecond counter, for every timeout the brief moves off
 * FreeRTOS ticks (3.3): TXDONE 100 ms, heartbeat 3 s, bridge-lost 10 s. This
 * port has no verified microsecond hardware-timer call that doesn't pull in
 * Pico-SDK headers our build profile doesn't put on the include path (see
 * docs/curveball_findings.md); tk_get_tim()'s millisecond SYSTIM is kernel-
 * native and plenty of resolution for 100 ms/3 s/10 s timeouts, so that is
 * what this wraps, not time_us_32(). Always pair with rtos_elapsed_ms, which
 * does the unsigned subtraction so the 32-bit wrap (~49.7 days) is harmless. */
uint32_t rtos_now_ms(void);
uint32_t rtos_elapsed_ms(uint32_t start_ms);

/* 0 or 1. Every task logs this once at start (brief 3.6). */
unsigned rtos_core_id(void);

/* For the DHT22 priority boost (brief 3.7/3.8, Gap 14 default: keep as is). */
unsigned rtos_priority_self_get(void);
void rtos_priority_self_set(unsigned prio);

/* Canary-fill stack high-water check (brief 6.6: no kernel call for this
 * exists in mtk3smp-rp2040 - see docs/curveball_findings.md - so this fills
 * the task's stack with a known pattern at creation and scans for the
 * lowest byte ever touched). Returns bytes never touched, or 0 if task is
 * not a valid rtos_port-created task id. */
uint32_t rtos_task_stack_free(rtos_id_t task);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_PORT_H */
