/*
 * rtos_port.h - the only header in firmware_smp/app that may know the kernel is
 * uT-Kernel 3.
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
extern "C"
{
#endif

    typedef void (*rtos_task_fn)(void* p_arg);

    /* Numbered 0/1 at this layer; rtos_port_mtk3.c maps 0->TP_PRC1 (core 0),
     * 1->TP_PRC2 (core 1). Ignored entirely when built SMP=0 (see
     * build/README). */
    typedef enum
    {
        RTOS_CORE_0 = 0,
        RTOS_CORE_1 = 1,
    } rtos_core_t;

/* Smaller priority number = runs first, matching tk_cre_tsk directly
 * (MIN_TSKPRI=1 highest .. MAX_TSKPRI=32 lowest). Priority only matters
 * between tasks pinned to the same core (brief 3.1). Gaps left for later
 * insertions, per brief 3.3. */
#define RTOS_PRIO_COMMS     10
#define RTOS_PRIO_USB_OWNER 20
/* alone on core 1: value doesn't interact with core 0's */
#define RTOS_PRIO_SENSORS 10

#define RTOS_WAIT_FOREVER (-1)
#define RTOS_NO_WAIT      0

    typedef int32_t rtos_id_t;

    /* Returns a task id > 0, or a negative kernel error code. The task is
     * created and started (tk_cre_tsk + tk_sta_tsk); it must never return. */
    rtos_id_t rtos_task_create(const char* p_name, rtos_task_fn p_callback,
                               void* p_arg, unsigned priority,
                               unsigned stack_bytes, rtos_core_t core);

    void rtos_delay_ms(uint32_t msec);

    /* Event flags: one flag group holds up to 32 independent bits. */
    rtos_id_t rtos_flags_create(void);
    void      rtos_flags_set(rtos_id_t flags, uint32_t bits);
    void      rtos_flags_clear(rtos_id_t flags, uint32_t bits);
    /* Waits for ANY bit in mask; on success *out_bits holds the matched flags.
     * timeout_ms: RTOS_NO_WAIT, RTOS_WAIT_FOREVER, or a ms value. Returns false
     * on timeout. Only the matched bits are cleared on return (TWF_BITCLR, not
     * TWF_CLR - round 2 review fix A3: TWF_CLR would zero every bit in the
     * group, including ones a different task is still waiting on, since
     * EVT_WANT_STATS/EVT_FORCE_CAPTURE/etc. all share one group in
     * main_smp.c - see the citation in rtos_port_mtk3.c). */
    bool rtos_flags_wait_any(rtos_id_t flags, uint32_t mask,
                             uint32_t* p_out_bits, int32_t timeout_ms);

/* Required backing_buf size for rtos_msgq_create (round 2 review fix A4):
 * each queued message costs 4 bytes of header plus msg_size rounded up to a
 * multiple of 4, not just msg_size - verified in
 * third_party/mtk3smp_rp2040/kernel/tkernel/messagebuf.c/.h (HEADERSZ=4,
 * ROUNDSZ rounds to a multiple of 4). Sizing backing_buf as msg_size*count
 * under-allocates it relative to what T_CMBF.bufsz is told it is, which is
 * a real buffer overflow risk, not just a capacity mismatch. */
#define RTOS_MSGQ_BYTES(msg_size, count)                                       \
    ((uint32_t)(count) * (4u + (((uint32_t)(msg_size) + 3u) & ~3u)))

    /* Fixed-size message buffer, msg_size bytes per message, count messages
     * deep.
     * backing_buf must be caller-allocated static/global storage of at least
     * RTOS_MSGQ_BYTES(msg_size, count) bytes that outlives the queue (the
     * kernel's T_CMBF keeps a bare pointer into it, not a copy). */
    rtos_id_t rtos_msgq_create(uint32_t msg_size, uint32_t count,
                               void* p_backing_buf);
    /* Never blocks (TMO_POL). Returns false ("queue full") without sending. */
    bool rtos_msgq_send_nowait(rtos_id_t queue, const void* p_msg,
                               uint32_t msg_size);
    /* Waits up to timeout_ms (RTOS_NO_WAIT, RTOS_WAIT_FOREVER, or a ms value)
     * for room. Only valid from a task. Returns false if the message could not
     * be queued in time. */
    bool rtos_msgq_send_wait(rtos_id_t queue, const void* p_msg,
                             uint32_t msg_size, int32_t timeout_ms);
    /* timeout_ms: RTOS_NO_WAIT, RTOS_WAIT_FOREVER, or a ms value. */
    bool rtos_msgq_recv(rtos_id_t queue, void* p_msg, uint32_t msg_size,
                        int32_t timeout_ms);

    /* Round 2 review fix A4: startup self-check. Sends `count` dummy messages,
     * confirms the (count+1)th is refused, drains everything back out (leaves
     * the queue empty), and returns whether all three held. If log_line is
     * non-NULL and log_line_max >= 64, writes a one-line summary into it (the
     * caller logs it - rtos_port does not depend on log.h). */
    bool rtos_msgq_selftest(rtos_id_t queue, uint32_t msg_size, uint32_t count,
                            char* p_log_line, uint32_t log_line_max);

    /* Free-running millisecond counter, for every timeout the brief moves off
     * FreeRTOS ticks (3.3): TXDONE 100 ms, heartbeat 3 s, bridge-lost 10 s.
     * This port has no verified microsecond hardware-timer call that doesn't
     * pull in Pico-SDK headers our build profile doesn't put on the include
     * path (see docs/curveball_findings.md); tk_get_tim()'s millisecond SYSTIM
     * is kernel- native and plenty of resolution for 100 ms/3 s/10 s timeouts,
     * so that is what this wraps, not time_us_32(). Always pair with
     * rtos_elapsed_ms, which does the unsigned subtraction so the 32-bit wrap
     * (~49.7 days) is harmless. */
    uint32_t rtos_now_ms(void);
    uint32_t rtos_elapsed_ms(uint32_t start_ms);

    /* 0 or 1. Every task logs this once at start (brief 3.6). */
    unsigned rtos_core_id(void);

    /* The calling task's id (> 0), or 0 when called from an interrupt handler
     * or with interrupts disabled (where nothing may block). */
    rtos_id_t rtos_self_id(void);

    /* Round 2 review fix A2: tm_getchar(0) is dead on the CONSOLE=usb_cdc
     * profile (see firmware_smp/buildglue/usb_tinyusb_glue_overlay.c for why
     * and the build-time overlay that fixes it). This is what usb_owner_task
     * calls instead. Returns the byte, or -1 if none is waiting. */
    int rtos_console_getchar(void);

    /* Calling task's priority get/set (kept for callers that need it; the SMP
     * DHT22 read now masks interrupts instead, main_smp.c
     * dht_timing_enter()). */
    unsigned rtos_priority_self_get(void);
    void     rtos_priority_self_set(unsigned prio);

    /* Current priority of any task, asked from the kernel (tk_ref_tsk); 0 if
     * the id is not valid.
     * Used by 's' so the printed table is what the kernel holds, not what the
     * code meant to set. */
    unsigned rtos_task_priority(rtos_id_t task);

    /* Follow-up F8: rtos_msgq_send_nowait()/send_wait() return false both for
     * "queue full" and for any other kernel error. These count only the other
     * errors (not E_TMOUT) and keep the last code, so a refusal can be told
     * apart from a fault. */
    uint32_t rtos_msgq_error_count(void);
    int32_t  rtos_msgq_last_error(void);

    /* Canary-fill stack high-water check (brief 6.6: no kernel call for this
     * exists in mtk3smp-rp2040 - see docs/curveball_findings.md - so this fills
     * the task's stack with a known pattern at creation and scans for the
     * lowest byte ever touched). Returns bytes never touched, or 0 if task is
     * not a valid rtos_port-created task id. */
    uint32_t rtos_task_stack_free(rtos_id_t task);

    /* Round 2 review fix A8: a plain x++ on these diagnostic counters is not
     * atomic across the two Cortex-M0+ cores (no LDREX/STREX on armv6m), so an
     * increment from the "wrong" core relative to another one in flight can be
     * lost. rtos_atomic_init() must run once, before any task starts (called
     * from app_main_smp()); rtos_atomic_add_u32 is then safe to call from any
     * task/core for a counter only ever touched this way. */
    void rtos_atomic_init(void);
    void rtos_atomic_add_u32(volatile uint32_t* p_counter, uint32_t delta);
    /* Sets *value to desired only if it currently equals expected; returns
     * whether it did. Same lock as rtos_atomic_add_u32. */
    bool rtos_atomic_cas_i32(volatile int32_t* p_value, int32_t expected,
                             int32_t desired);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_PORT_H */

/*** end of file ***/

