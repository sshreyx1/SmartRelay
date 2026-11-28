/*
 * smp_smoke.c - CURVEBALL round 3, Task C2: a small, separate app (no radios,
 * no SD, no ALFP) that proves the dual-core uT-Kernel 3 SMP port actually works
 * on real hardware, independent of anything else SmartRelay does. Two tasks:
 * core0_task plays the "comms + usb_owner" role (core 0, drives every check and
 * owns the console/log drain, same split as main_smp.c), core1_task plays the
 * "sensors" role (core 1, the cooperating side of each check). Every check
 * prints one "PASS <name> ..." or "FAIL <name> ..." line with the numbers it
 * measured, and the whole run ends with one line starting "SMOKE " that
 * tools/smp_smoke_check.py greps for.
 *
 * Known, honest limitation (see rtos_port.h): this port's
 * rtos_now_ms()/rtos_elapsed_ms() wrap tk_get_tim(), a MILLISECOND counter --
 * there is no verified microsecond hardware-timer call available without
 * pulling in Pico SDK headers this build profile does not put on the include
 * path yet (Task C3 resolves that). So check 3 below measures tick accuracy at
 * millisecond resolution only; a 1 ms delay measured with a 1 ms clock is
 * inherently coarse, and the PASS/FAIL threshold accounts for that. Do not read
 * more precision into these numbers than the clock has.
 *
 * Nothing in this file has been compiled (no ARM toolchain in the environment
 * that wrote it, see docs/curveball_findings.md); it is written against
 * rtos_port.h/log.h as they stand, not guessed.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "log.h"
#include "rtos_port.h"

/* tm_sprintf/tm_printf: the monitor library, not a kernel tk_* call -- same as
 * main_smp.c */
#include <tm/tmonitor.h>

/* ---- cross-task signalling (brief 3.3: event flags, not volatile bools)
 * ------------------- */
/* core0 -> core1: start spinning (check 2 phase B) */
#define EVT_START_CONCURRENCY (1u << 0)
#define EVT_CONCURRENCY_DONE  (1u << 1) /* core1 -> core0 */
/* core0 -> core1: start producing (check 4) */
#define EVT_START_MSGQ_STRESS (1u << 2)
#define EVT_MSGQ_STRESS_DONE  (1u << 3) /* core1 -> core0 */
/* core0 -> core1: start adding (check 6) */
#define EVT_START_ATOMIC (1u << 4)
#define EVT_ATOMIC_DONE  (1u << 5) /* core1 -> core0 */
/* check 5: two different bits set together */
#define EVT_FLAGTEST_BIT_A (1u << 6)
#define EVT_FLAGTEST_BIT_B (1u << 7)

static rtos_id_t g_flags    = -1;
static rtos_id_t g_core0_id = -1;
static rtos_id_t g_core1_id = -1;

/* ---- check 2: real concurrency
 * ------------------------------------------------------------- */
static volatile uint32_t g_spin0_count;
static volatile uint32_t g_spin1_count;

static void spin_for_ms (volatile uint32_t* p_counter, uint32_t msec)
{
    uint32_t start = rtos_now_ms();
    *p_counter     = 0;
    while (rtos_elapsed_ms(start) < msec)
    {
        (*p_counter)++;
    }
}

/* ---- check 3: tick accuracy
 * ----------------------------------------------------------------- */
typedef struct
{
    uint32_t min_ms;
    uint32_t max_ms;
    uint32_t sum_ms;
    uint32_t count;
} delay_stats_t;

static void measure_delay (uint32_t request_ms, uint32_t trials,
                           delay_stats_t* p_out)
{
    p_out->min_ms = UINT32_MAX;
    p_out->max_ms = 0;
    p_out->sum_ms = 0;
    p_out->count  = trials;
    for (uint32_t idx = 0; idx < trials; idx++)
    {
        uint32_t start = rtos_now_ms();
        rtos_delay_ms(request_ms);
        uint32_t took = rtos_elapsed_ms(start);
        if (took < p_out->min_ms)
        {
            p_out->min_ms = took;
        }
        if (took > p_out->max_ms)
        {
            p_out->max_ms = took;
        }
        p_out->sum_ms += took;
    }
}

/* ---- check 4: cross-core message buffer stress
 * ----------------------------------------------
 * Producer (core1) must not drop a message: rtos_msgq_send_nowait() never
 * blocks, so "nothing lost" means the producer retries until it succeeds, not
 * that loss is tolerated. */
typedef struct
{
    uint32_t seq;
    /* seq * 2654435761u, just something the consumer can recompute and check */
    uint32_t checksum;
} stress_msg_t;

#define STRESS_Q_DEPTH 8
#define STRESS_COUNT   100000u
static uint8_t
    g_stress_buf[RTOS_MSGQ_BYTES(sizeof(stress_msg_t), STRESS_Q_DEPTH)];
static rtos_id_t         g_stress_q = -1;
static volatile uint32_t g_stress_lost;
static volatile uint32_t g_stress_reordered;
static volatile uint32_t g_stress_bad_checksum;
static volatile uint32_t g_stress_received;

/* same shape as main_smp.c's capture queue: exactly 4 deep */
#define CAPQ_DEPTH 4
static uint8_t   g_capq_buf[RTOS_MSGQ_BYTES(sizeof(uint32_t), CAPQ_DEPTH)];
static rtos_id_t g_capq = -1;

/* ---- check 5: event flags
 * --------------------------------------------------------------------
 * (state lives entirely in g_flags itself; nothing else needed here) */

/* ---- check 6: atomic counters
 * ----------------------------------------------------------------
 * 32-bit wraps harmlessly past 2,000,000 so no special handling needed; checked
 * for the exact value, not just "close". */
#define ATOMIC_ITERS 1000000u
static volatile uint32_t g_atomic_counter;

/* ---- check 7: log ring under a stalled terminal
 * ----------------------------------------------
 * Driven from core0_task's own main loop (it owns the drain); see
 * run_log_stall_check(). */
static volatile uint32_t g_log_stall_longest_loop_ms;

/* ---- core1_task: the cooperating side of every check
 * ------------------------------------------ */
static void core1_task (void* p_arg)
{
    (void)p_arg;
    char line[LOG_LINE_MAX];
    tm_sprintf((UB*)line, (UB*)"[1] core1_task: core %u", rtos_core_id());
    log_write_line(line);

    for (;;)
    {
        uint32_t bits = 0;
        rtos_flags_wait_any(g_flags,
                            EVT_START_CONCURRENCY | EVT_START_MSGQ_STRESS |
                                EVT_START_ATOMIC,
                            &bits, RTOS_WAIT_FOREVER);

        if (bits & EVT_START_CONCURRENCY)
        {
            spin_for_ms(&g_spin1_count, 1000);
            rtos_flags_set(g_flags, EVT_CONCURRENCY_DONE);
        }

        if (bits & EVT_START_MSGQ_STRESS)
        {
            for (uint32_t seq = 0; seq < STRESS_COUNT; seq++)
            {
                stress_msg_t msg = {seq, seq * 2654435761u};
                while (!rtos_msgq_send_nowait(g_stress_q, &msg, sizeof(msg)))
                {
                    /* queue full: retry rather than drop -- "nothing lost" is a
                     * contract on the producer, not a property the message
                     * buffer gives for free. */
                }
            }
            rtos_flags_set(g_flags, EVT_MSGQ_STRESS_DONE);
        }

        if (bits & EVT_START_ATOMIC)
        {
            for (uint32_t idx = 0; idx < ATOMIC_ITERS; idx++)
            {
                rtos_atomic_add_u32(&g_atomic_counter, 1u);
            }
            rtos_flags_set(g_flags, EVT_ATOMIC_DONE);
        }
    }
}

/* ---- individual checks, run from core0_task
 * ---------------------------------------------------- */

static void check1_core_ids (void)
{
    char     line[LOG_LINE_MAX];
    unsigned core_start = rtos_core_id();
    bool     b_is_ok    = (RTOS_CORE_0 == core_start);
    tm_sprintf((UB*)line,
               (UB*)"%s check1_core_ids: core0_task on core %u (want %u)",
               b_is_ok ? "PASS" : "FAIL", core_start, (unsigned)RTOS_CORE_0);
    log_write_line(line);
    /* core1_task already logged its own core id at startup (see above); this
     * just confirms core0_task's own side, since a task can only read
     * rtos_core_id() about itself. */
}

static void check2_concurrency (void)
{
    char line[LOG_LINE_MAX];
    spin_for_ms(&g_spin0_count, 1000); /* phase A: core0 alone */
    uint32_t solo0 = g_spin0_count;

    uint32_t bits = 0;
    rtos_flags_set(g_flags, EVT_START_CONCURRENCY); /* phase B: both at once */
    spin_for_ms(&g_spin0_count, 1000);
    rtos_flags_wait_any(g_flags, EVT_CONCURRENCY_DONE, &bits, 5000);
    uint32_t both0 = g_spin0_count;
    uint32_t both1 = g_spin1_count;

    /* Real concurrency: core0's rate barely changes with core1 also running
     * (independent cores, not one core time-sliced), and core1 made real,
     * comparable progress. Not a speed-up claim, just "both cores were actually
     * counting during the same 1 s window". */
    /* generous: just rule out "core1 starved core0" */
    bool b_is_ok = (both1 > 0) && (both0 > solo0 / 2u);
    tm_sprintf((UB*)line,
               (UB*)"%s check2_concurrency: solo core0=%" PRIu32
                    " both core0=%" PRIu32 " both core1=%" PRIu32
                    " ratio(both1/solo0)=%" PRIu32 ".%02" PRIu32,
               b_is_ok ? "PASS" : "FAIL", (uint32_t)solo0, (uint32_t)both0,
               (uint32_t)both1,
               (uint32_t)(both1 * 100u / (solo0 ? solo0 : 1u) / 100u),
               (uint32_t)(both1 * 100u / (solo0 ? solo0 : 1u) % 100u));
    log_write_line(line);
}

static void check3_tick_accuracy (void)
{
    char          line[LOG_LINE_MAX];
    delay_stats_t delay_a, delay_b;
    measure_delay(1, 1000, &delay_a);
    measure_delay(2, 1000, &delay_b);
    /* Millisecond-resolution clock measuring millisecond delays (see file
     * header): PASS means "close to 1 ms / 2 ms", not "exactly". A
     * CNF_TIMER_PERIOD=10 misconfiguration (round 2 A1) would show up here as
     * min/mean/max all sitting near 10, which is the point of this check. */
    /* 1 ms requested: allow slack for ms-quantization + scheduling */
    bool b_ok1 = delay_a.max_ms <= 3u;
    bool b_ok2 = delay_b.max_ms <= 4u; /* 2 ms requested */
    tm_sprintf((UB*)line,
               (UB*)"%s check3_tick_1ms: min=%" PRIu32 " mean=%" PRIu32
                    ".%02" PRIu32 " max=%" PRIu32 " (n=%" PRIu32
                    ", ms resolution)",
               b_ok1 ? "PASS" : "FAIL", (uint32_t)delay_a.min_ms,
               (uint32_t)(delay_a.sum_ms / delay_a.count),
               (uint32_t)(delay_a.sum_ms * 100u / delay_a.count % 100u),
               (uint32_t)delay_a.max_ms, (uint32_t)delay_a.count);
    log_write_line(line);
    tm_sprintf((UB*)line,
               (UB*)"%s check3_tick_2ms: min=%" PRIu32 " mean=%" PRIu32
                    ".%02" PRIu32 " max=%" PRIu32 " (n=%" PRIu32
                    ", ms resolution)",
               b_ok2 ? "PASS" : "FAIL", (uint32_t)delay_b.min_ms,
               (uint32_t)(delay_b.sum_ms / delay_b.count),
               (uint32_t)(delay_b.sum_ms * 100u / delay_b.count % 100u),
               (uint32_t)delay_b.max_ms, (uint32_t)delay_b.count);
    log_write_line(line);
}

static void check4_msgq_stress (void)
{
    char line[LOG_LINE_MAX];
    g_stress_received     = 0;
    g_stress_lost         = 0;
    g_stress_reordered    = 0;
    g_stress_bad_checksum = 0;

    uint32_t bits = 0;
    rtos_flags_set(g_flags, EVT_START_MSGQ_STRESS);
    uint32_t expect_seq = 0;
    uint32_t drained    = 0;
    while (drained < STRESS_COUNT)
    {
        stress_msg_t msg;
        if (rtos_msgq_recv(g_stress_q, &msg, sizeof(msg), 1000))
        {
            if (msg.checksum != msg.seq * 2654435761u)
            {
                g_stress_bad_checksum++;
            }
            if (msg.seq != expect_seq)
            {
                g_stress_reordered++;
            }
            expect_seq = msg.seq + 1u;
            drained++;
            g_stress_received++;
        }
        else
        {
            /* producer stalled for a full second: treat the rest as lost, don't
             * spin forever */
            break;
        }
    }
    g_stress_lost = STRESS_COUNT - g_stress_received;
    rtos_flags_wait_any(g_flags, EVT_MSGQ_STRESS_DONE, &bits, 5000);

    bool b_is_ok = (0 == g_stress_lost) && (0 == g_stress_reordered) &&
                   (0 == g_stress_bad_checksum);
    tm_sprintf((UB*)line,
               (UB*)"%s check4_msgq_stress: received=%" PRIu32 "/%" PRIu32
                    " lost=%" PRIu32 " reordered=%" PRIu32
                    " bad_checksum=%" PRIu32,
               b_is_ok ? "PASS" : "FAIL", (uint32_t)g_stress_received,
               (uint32_t)STRESS_COUNT, (uint32_t)g_stress_lost,
               (uint32_t)g_stress_reordered, (uint32_t)g_stress_bad_checksum);
    log_write_line(line);

    /* Full-queue case, same shape as main_smp.c's 4-deep capture queue: the 5th
     * nowait send into an otherwise-empty, never-drained queue must be refused,
     * not block and not silently drop something already queued. */
    uint32_t value = 0;
    bool     b_ok4 = (rtos_msgq_send_nowait(g_capq, &value, sizeof(value))) &&
                     (rtos_msgq_send_nowait(g_capq, &value, sizeof(value))) &&
                     (rtos_msgq_send_nowait(g_capq, &value, sizeof(value))) &&
                     (rtos_msgq_send_nowait(g_capq, &value, sizeof(value)))
                     /* 5th must be refused */
                     && (!rtos_msgq_send_nowait(g_capq, &value, sizeof(value)));
    tm_sprintf((UB*)line, (UB*)"%s check4_capqueue_full: depth=%u, 5th send %s",
               b_ok4 ? "PASS" : "FAIL", (unsigned)CAPQ_DEPTH,
               b_ok4 ? "correctly refused" : "NOT refused (bug)");
    log_write_line(line);
    while (rtos_msgq_recv(g_capq, &value, sizeof(value), RTOS_NO_WAIT))
    {
        /* drain back to empty for anything that runs after this check */
    }

    /* Log ring: fill it to exactly LOG_RING_DEPTH without draining, confirm
     * log_dropped_count() climbs on the next line rather than this call
     * blocking. Done here (not in check 7) because it is a one-shot capacity
     * check, not the sustained-stall check check 7 does. */
    uint32_t dropped_before = log_dropped_count();
    for (int idx = 0; idx < 70; idx++) /* > LOG_RING_DEPTH(64) on purpose */
    {
        log_write_line("log ring capacity probe");
    }
    uint32_t dropped_after = log_dropped_count();
    bool     b_ok_ring     = dropped_after > dropped_before;
    tm_sprintf((UB*)line,
               (UB*)"%s check4_log_ring_capacity: dropped %" PRIu32
                    " -> %" PRIu32 " (rose: %s)",
               b_ok_ring ? "PASS" : "FAIL", (uint32_t)dropped_before,
               (uint32_t)dropped_after, b_ok_ring ? "yes" : "no");
    log_write_line(line);
}

static void check5_event_flags (void)
{
    char line[LOG_LINE_MAX];
    /* Set two different bits together (as if two different signals arrived back
     * to back), wait for only one of them, and confirm the other is still there
     * afterwards (TWF_BITCLR, not TWF_CLR -- round 2 review fix A3). */
    rtos_flags_set(g_flags, EVT_FLAGTEST_BIT_A | EVT_FLAGTEST_BIT_B);
    uint32_t bits = 0;
    bool     b_got_a =
        (rtos_flags_wait_any(g_flags, EVT_FLAGTEST_BIT_A, &bits, 1000)) &&
        (0 != (bits & EVT_FLAGTEST_BIT_A));
    uint32_t bits2 = 0;
    bool     b_got_b =
        (rtos_flags_wait_any(g_flags, EVT_FLAGTEST_BIT_B, &bits2, 1000)) &&
        (0 != (bits2 & EVT_FLAGTEST_BIT_B));
    bool b_is_ok = b_got_a && b_got_b;
    tm_sprintf((UB*)line,
               (UB*)"%s check5_event_flags: bit A %s, bit B %s (not lost when "
                    "A was waited on alone)",
               b_is_ok ? "PASS" : "FAIL", b_got_a ? "seen" : "LOST",
               b_got_b ? "seen" : "LOST");
    log_write_line(line);
}

static void check6_atomic_counters (void)
{
    char line[LOG_LINE_MAX];
    g_atomic_counter = 0;
    uint32_t bits    = 0;
    rtos_flags_set(g_flags, EVT_START_ATOMIC);
    for (uint32_t idx = 0; idx < ATOMIC_ITERS; idx++)
    {
        rtos_atomic_add_u32(&g_atomic_counter, 1u);
    }
    rtos_flags_wait_any(g_flags, EVT_ATOMIC_DONE, &bits, 30000);
    uint32_t want    = ATOMIC_ITERS * 2u;
    bool     b_is_ok = g_atomic_counter == want;
    tm_sprintf((UB*)line,
               (UB*)"%s check6_atomic_counters: got %" PRIu32 " want %" PRIu32
                    " (both cores, %" PRIu32 " each)",
               b_is_ok ? "PASS" : "FAIL", (uint32_t)g_atomic_counter,
               (uint32_t)want, (uint32_t)ATOMIC_ITERS);
    log_write_line(line);
}

static void check7_log_ring_stall (void)
{
    char     line[LOG_LINE_MAX];
    uint32_t dropped_before     = log_dropped_count();
    g_log_stall_longest_loop_ms = 0;
    uint32_t stall_until        = rtos_now_ms() + 5000u;
    uint32_t count              = 0;
    while ((int32_t)(rtos_now_ms() - stall_until) < 0)
    {
        uint32_t loop_start = rtos_now_ms();
        char     probe[LOG_LINE_MAX];
        tm_sprintf((UB*)probe, (UB*)"stall probe %" PRIu32, (uint32_t)count++);
        /* never blocks even though nothing is draining right now */
        log_write_line(probe);
        uint32_t loop_ms = rtos_elapsed_ms(loop_start);
        if (loop_ms > g_log_stall_longest_loop_ms)
        {
            g_log_stall_longest_loop_ms = loop_ms;
        }
    }
    uint32_t dropped_after = log_dropped_count();
    /* Drain whatever is left so later checks' log lines are not stuck behind
     * this backlog. */
    char drained[LOG_LINE_MAX];
    while (log_drain_one(drained, RTOS_NO_WAIT))
    {
    }
    bool b_is_ok = (dropped_after > dropped_before) &&
                   (g_log_stall_longest_loop_ms < 100u);
    tm_sprintf((UB*)line,
               (UB*)"%s check7_log_ring_stall: dropped %" PRIu32 " -> %" PRIu32
                    " over 5 s, longest single loop %" PRIu32 " ms",
               b_is_ok ? "PASS" : "FAIL", (uint32_t)dropped_before,
               (uint32_t)dropped_after, (uint32_t)g_log_stall_longest_loop_ms);
    log_write_line(line);
}

static void check8_stack_high_water (void)
{
    char     line[LOG_LINE_MAX];
    uint32_t free0 = rtos_task_stack_free(g_core0_id);
    uint32_t free1 = rtos_task_stack_free(g_core1_id);
    /* "not close to exhausted": free > some fraction of the stack this app
     * created each task with (4096 B, see smoke_main() below) -- not a hard
     * pass/fail per the brief, just reported for a human to judge; still prints
     * FAIL if a task is already within 10% of its stack. */
    bool b_is_ok = (free0 > 409u) && (free1 > 409u);
    tm_sprintf((UB*)line,
               (UB*)"%s check8_stack_high_water: core0_task %" PRIu32
                    " B free, core1_task %" PRIu32 " B free",
               b_is_ok ? "PASS" : "FAIL", (uint32_t)free0, (uint32_t)free1);
    log_write_line(line);
}

static void print_stats (void)
{
    char line[LOG_LINE_MAX];
    tm_sprintf((UB*)line,
               (UB*)"[stats] atomic_counter=%" PRIu32 " log_dropped=%" PRIu32
                    " stress_received=%" PRIu32,
               (uint32_t)g_atomic_counter, (uint32_t)log_dropped_count(),
               (uint32_t)g_stress_received);
    log_write_line(line);
}

static void drain_log (void)
{
    char drained[LOG_LINE_MAX];
    while (log_drain_one(drained, RTOS_NO_WAIT))
    {
        /* host-side tooling (smp_smoke_check.py) reads these lines */
        tm_printf((UB*)"%s\n", drained);
    }
}

/* ---- core0_task: orchestrates every check, drains the log, reads the console
 * -------------------- */
static void core0_task (void* p_arg)
{
    (void)p_arg;
    char line[LOG_LINE_MAX];
    tm_sprintf((UB*)line, (UB*)"[0] core0_task: core %u", rtos_core_id());
    log_write_line(line);

    /* give the USB CDC link a moment, same as main_smp.c */
    for (int idx = 0; idx < 200; idx++)
    {
        rtos_delay_ms(100);
        drain_log();
    }

    /* Drained between every check (not just before/after all of them):
     * otherwise a check's own PASS/FAIL line can sit in the ring behind another
     * check's output for tens of seconds with nobody reading it, and check4's
     * own deliberate ring-overflow probe would make that worse (round 3
     * real-hardware run: a host connecting any time after boot but before
     * "SMOKE done" saw only the last couple of checks' lines until this was
     * added). */
    check1_core_ids();
    drain_log();
    check2_concurrency();
    drain_log();
    check3_tick_accuracy();
    drain_log();
    check4_msgq_stress();
    drain_log();
    check5_event_flags();
    drain_log();
    check6_atomic_counters();
    drain_log();
    check7_log_ring_stall();
    drain_log();
    check8_stack_high_water();
    drain_log();

    /* check 9 (console input) is the loop below: 's' prints stats, same key as
     * main_smp.c. Runs forever so the serial log stays open for
     * tools/smp_smoke_check.py and so a human can press 's' interactively; the
     * SMOKE summary line below is printed once, before this loop starts. */
    log_write_line(
        "SMOKE done: see PASS/FAIL lines above for each check's result");

    for (;;)
    {
        drain_log();
        int chan = rtos_console_getchar();
        if ('s' == chan)
        {
            print_stats();
        }
        rtos_delay_ms(20);
    }
}

void app_main_smoke (void)
{
    rtos_atomic_init();
    log_init();

    char selftest_line[LOG_LINE_MAX];
    log_selftest(selftest_line, sizeof(selftest_line));
    log_write_line(selftest_line);

    g_flags = rtos_flags_create();
    g_stress_q =
        rtos_msgq_create(sizeof(stress_msg_t), STRESS_Q_DEPTH, g_stress_buf);
    g_capq = rtos_msgq_create(sizeof(uint32_t), CAPQ_DEPTH, g_capq_buf);

    g_core0_id = rtos_task_create("core0", core0_task, NULL, RTOS_PRIO_COMMS,
                                  4096, RTOS_CORE_0);
    g_core1_id = rtos_task_create("core1", core1_task, NULL, RTOS_PRIO_SENSORS,
                                  4096, RTOS_CORE_1);
}

/*** end of file ***/

