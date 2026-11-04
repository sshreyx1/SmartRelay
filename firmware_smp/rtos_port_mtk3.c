/*
 * rtos_port_mtk3.c - rtos_port.h implemented over third_party/mtk3smp_rp2040.
 * Every call here is cited in docs/curveball_findings.md Step 2; nothing
 * below is a guessed signature.
 */
#include "rtos_port.h"

/* tk/syslib.h typedefs size_t itself (as SZ, a long int), which conflicts with the real size_t
 * (<string.h>'s stddef.h, unsigned int) the moment both are in one translation unit -- confirmed
 * on a real toolchain run, round 3: GCC rejects the second, different-underlying-type typedef,
 * regardless of include order. The kernel's own lwip glue
 * (lib/liblwip/sysdepend/pico_rp2040/sys_arch.c) hits the same thing and already defines
 * PROHIBIT_DEF_SIZE_T (tk/syslib.h's own guard around that typedef) before including any kernel
 * header, for exactly this reason; this file needs the same guard since it is the one place
 * rtos_port.h's implementation pulls in both a kernel header and a standard C one. */
#define PROHIBIT_DEF_SIZE_T
#include <string.h>

#include <tk/tkernel.h>
#if TK_SUPPORT_SMP
#include <tk/smp.h>
#endif
#include <tm/tmonitor.h>

#define RTOS_MAX_TASKS 8
#define RTOS_MAX_STACK_BYTES 8192
#define RTOS_CANARY 0xA5u

/* tk_cre_tsk's retention behaviour for pk_ctsk/exinf past the call is not
 * documented in this port's headers, so both the T_CTSK and the trampoline
 * context are kept in static storage for the task's lifetime (CURVEBALL
 * tasks are created once at boot and never deleted) rather than risking a
 * stack temporary. */
typedef struct
{
    rtos_task_fn fn;
    void* arg;
} task_ctx_t;

static task_ctx_t s_task_ctx[RTOS_MAX_TASKS];
static T_CTSK s_ctsk[RTOS_MAX_TASKS];
static ID s_task_id[RTOS_MAX_TASKS];
static uint32_t s_task_stack_bytes[RTOS_MAX_TASKS];
/* 6.6: no stack-high-water kernel call exists in this port (findings doc,
 * Step 2 item 7's follow-up); own the stack buffer (TA_USERBUF) so it can be
 * canary-filled at creation and scanned later instead. 8-byte aligned: the
 * armv6m EABI requires 8-byte stack alignment at any external call/interrupt
 * boundary, which TA_USERBUF hands straight to the dispatcher as the task's
 * initial SP (round 2 review fix A7). */
/* One pool carved up in creation order, instead of RTOS_MAX_TASKS fixed 8 KB slots (64 KB of the
 * RP2040's 264 KB whether used or not): the node firmware's three tasks need 20 KB in all, and the
 * rest of RAM is needed by FatFs, the LoRa/WiFi meshes and the audio capture buffers. */
#ifndef RTOS_STACK_POOL_BYTES
#define RTOS_STACK_POOL_BYTES (24u * 1024u)
#endif
static uint8_t s_stack_pool[RTOS_STACK_POOL_BYTES] __attribute__((aligned(8)));
static uint32_t s_stack_pool_used;
static uint8_t* s_stack[RTOS_MAX_TASKS];
static unsigned s_task_count;

#if TK_SUPPORT_SMP
static T_SPLOCK s_counter_lock;
#endif

/* round 2 review fix A8: a plain `x++` on a shared counter is not atomic
 * across the two Cortex-M0+ cores (no LDREX/STREX on armv6m, unlike M3+),
 * so a cross-core increment can be lost. Matches the ISpinLock pattern this
 * port's own usb_console.c already uses for the same reason (tk/smp.h).
 * Call once from app_main_smp(), before any task starts - InitSpinLock's
 * own thread-safety for a concurrent first call is not documented in this
 * port, so this avoids relying on it. */
void rtos_atomic_init(void)
{
#if TK_SUPPORT_SMP
    InitSpinLock(&s_counter_lock);
#endif
}

void rtos_atomic_add_u32(volatile uint32_t* counter, uint32_t delta)
{
#if TK_SUPPORT_SMP
    UINT intsts;
    ISpinLock(&s_counter_lock, &intsts);
    *counter += delta;
    ISpinUnlock(&s_counter_lock, intsts);
#else
    *counter += delta;
#endif
}

bool rtos_atomic_cas_i32(volatile int32_t* value, int32_t expected, int32_t desired)
{
    bool swapped;
#if TK_SUPPORT_SMP
    UINT intsts;
    ISpinLock(&s_counter_lock, &intsts);
#else
    UINT intsts;
    DI(intsts);
#endif
    swapped = (*value == expected);
    if (swapped)
    {
        *value = desired;
    }
#if TK_SUPPORT_SMP
    ISpinUnlock(&s_counter_lock, intsts);
#else
    EI(intsts);
#endif
    return swapped;
}

static void task_trampoline(INT stacd, void* exinf)
{
    (void)stacd;
    task_ctx_t* ctx = (task_ctx_t*)exinf;
    ctx->fn(ctx->arg);
    /* rtos tasks loop forever; returning here would shut the kernel down. */
    for (;;)
    {
        tk_dly_tsk(1000);
    }
}

rtos_id_t rtos_task_create(const char* name, rtos_task_fn fn, void* arg, unsigned priority,
                            unsigned stack_bytes, rtos_core_t core)
{
    (void)name; /* T_CTSK has an optional dsname field; skipped, not load-bearing here */
    stack_bytes = (stack_bytes + 7u) & ~7u; /* keeps every carved stack 8-byte aligned */
    if (s_task_count >= RTOS_MAX_TASKS || stack_bytes > RTOS_MAX_STACK_BYTES ||
        stack_bytes > RTOS_STACK_POOL_BYTES - s_stack_pool_used)
    {
        return -1;
    }
    unsigned slot = s_task_count++;
    s_stack[slot] = &s_stack_pool[s_stack_pool_used];
    s_stack_pool_used += stack_bytes;
    s_task_ctx[slot].fn = fn;
    s_task_ctx[slot].arg = arg;
    s_task_stack_bytes[slot] = stack_bytes;
    memset(s_stack[slot], RTOS_CANARY, stack_bytes);

    T_CTSK* ctsk = &s_ctsk[slot];
    memset(ctsk, 0, sizeof(*ctsk));
    ctsk->exinf = &s_task_ctx[slot];
    ctsk->task = task_trampoline;
    ctsk->itskpri = (PRI)priority;
    ctsk->stksz = (SZ)stack_bytes;
    ctsk->bufptr = s_stack[slot];
    ctsk->tskatr = TA_HLNG | TA_RNG3 | TA_USERBUF;
#if TK_SUPPORT_SMP
    ctsk->tskatr |= TA_ASSPRC;
    ctsk->assprc = (core == RTOS_CORE_1) ? TP_PRC2 : TP_PRC1;
#else
    (void)core; /* SMP=0 build: single core, TA_ASSPRC dropped (brief 3.1 fallback) */
#endif

    ID tid = tk_cre_tsk(ctsk);
    if (tid <= E_OK)
    {
        return (rtos_id_t)tid;
    }
    ER er = tk_sta_tsk(tid, 0);
    if (er < E_OK)
    {
        return (rtos_id_t)er;
    }
    s_task_id[slot] = tid;
    return (rtos_id_t)tid;
}

uint32_t rtos_task_stack_free(rtos_id_t task)
{
    for (unsigned i = 0; i < s_task_count; i++)
    {
        if (s_task_id[i] != (ID)task)
        {
            continue;
        }
        uint32_t free_bytes = 0;
        while (free_bytes < s_task_stack_bytes[i] && s_stack[i][free_bytes] == RTOS_CANARY)
        {
            free_bytes++;
        }
        return free_bytes;
    }
    return 0;
}

void rtos_delay_ms(uint32_t ms)
{
    tk_dly_tsk((RELTIM)(ms ? ms : 1));
}

rtos_id_t rtos_flags_create(void)
{
    T_CFLG cflg;
    memset(&cflg, 0, sizeof(cflg));
    cflg.flgatr = TA_TFIFO | TA_WMUL; /* TA_WMUL: more than one task may wait on this group */
    cflg.iflgptn = 0;
    ID id = tk_cre_flg(&cflg);
    return (rtos_id_t)id;
}

void rtos_flags_set(rtos_id_t flags, uint32_t bits)
{
    tk_set_flg((ID)flags, (UINT)bits);
}

void rtos_flags_clear(rtos_id_t flags, uint32_t bits)
{
    tk_clr_flg((ID)flags, (UINT)~bits);
}

bool rtos_flags_wait_any(rtos_id_t flags, uint32_t mask, uint32_t* out_bits, int32_t timeout_ms)
{
    UINT ptn = 0;
    /* round 2 review fix A3: TWF_CLR zeroes the WHOLE flag group on any
     * match (kernel/tkernel/eventflag.c:170 "flgcb->flgptn = 0"), which
     * would wipe out an unrelated task's pending bit in this one shared
     * group. TWF_BITCLR clears only the bits in `mask` that were actually
     * set (eventflag.c:168-172: "flgcb->flgptn &= ~waiptn"). */
    ER er = tk_wai_flg((ID)flags, (UINT)mask, TWF_ORW | TWF_BITCLR, &ptn, (TMO)timeout_ms);
    if (er < E_OK)
    {
        return false;
    }
    if (out_bits)
    {
        *out_bits = (uint32_t)ptn;
    }
    return true;
}

rtos_id_t rtos_msgq_create(uint32_t msg_size, uint32_t count, void* backing_buf)
{
    T_CMBF cmbf;
    memset(&cmbf, 0, sizeof(cmbf));
    /* TA_USERBUF added in this round: without it, and with USE_IMALLOC=1
     * (config/config.h:107), tk_cre_mbf silently allocates its OWN memory
     * from the kernel heap and ignores bufptr entirely
     * (kernel/tkernel/messagebuf.c ~145-159) - backing_buf was being passed
     * in and then never actually used. Found while fixing A4, not something
     * the round 2 report asked about directly. */
    cmbf.mbfatr = TA_TFIFO | TA_USERBUF;
    /* round 2 review fix A4: each queued message costs HEADERSZ(4) +
     * ROUNDSZ(msgsz) bytes of ring space, not just msgsz
     * (kernel/tkernel/messagebuf.c:65,145-159 and messagebuf.h:58-62) - use
     * RTOS_MSGQ_BYTES so this matches the backing_buf size the caller
     * allocated via the same macro. */
    cmbf.bufsz = (SZ)RTOS_MSGQ_BYTES(msg_size, count);
    cmbf.maxmsz = (INT)msg_size;
    cmbf.bufptr = backing_buf;
    ID id = tk_cre_mbf(&cmbf);
    return (rtos_id_t)id;
}

bool rtos_msgq_selftest(rtos_id_t q, uint32_t msg_size, uint32_t count, char* log_line, uint32_t log_line_max)
{
    uint8_t dummy[256]; /* >= the largest queued message (log.h LOG_LINE_MAX) */
    if (msg_size > sizeof(dummy))
    {
        return false; /* self-test buffer too small; not a queue defect */
    }
    memset(dummy, 0, msg_size);

    uint32_t sent = 0;
    for (; sent < count; sent++)
    {
        if (!rtos_msgq_send_nowait(q, dummy, msg_size))
        {
            break;
        }
    }
    bool extra_accepted = rtos_msgq_send_nowait(q, dummy, msg_size);

    uint32_t drained = 0;
    while (rtos_msgq_recv(q, dummy, msg_size, RTOS_NO_WAIT))
    {
        drained++;
    }

    bool ok = (sent == count) && !extra_accepted && (drained == count);
    if (log_line && log_line_max >= 64)
    {
        tm_sprintf((UB*)log_line, (UB*)"msgq selftest: wanted %u, sent %u, extra accepted %d, drained %u - %s",
                   count, sent, (int)extra_accepted, drained, ok ? "ok" : "FAILED");
    }
    return ok;
}

bool rtos_msgq_send_nowait(rtos_id_t q, const void* msg, uint32_t msg_size)
{
    ER er = tk_snd_mbf((ID)q, msg, (INT)msg_size, TMO_POL);
    return er >= E_OK;
}

bool rtos_msgq_send_wait(rtos_id_t q, const void* msg, uint32_t msg_size, int32_t timeout_ms)
{
    ER er = tk_snd_mbf((ID)q, msg, (INT)msg_size, (TMO)timeout_ms);
    return er >= E_OK;
}

bool rtos_msgq_recv(rtos_id_t q, void* msg, uint32_t msg_size, int32_t timeout_ms)
{
    INT sz = tk_rcv_mbf((ID)q, msg, (TMO)timeout_ms);
    return sz == (INT)msg_size;
}

uint32_t rtos_now_ms(void)
{
    SYSTIM t;
    if (tk_get_tim(&t) < E_OK)
    {
        return 0;
    }
    return (uint32_t)t.lo;
}

uint32_t rtos_elapsed_ms(uint32_t start_ms)
{
    return (uint32_t)(rtos_now_ms() - start_ms);
}

unsigned rtos_priority_self_get(void)
{
    T_RTSK rtsk;
    if (tk_ref_tsk(TSK_SELF, &rtsk) < E_OK)
    {
        return 0;
    }
    return (unsigned)rtsk.tskpri;
}

void rtos_priority_self_set(unsigned prio)
{
    tk_chg_pri(TSK_SELF, (PRI)prio);
}

unsigned rtos_core_id(void)
{
#if TK_SUPPORT_SMP
    /* tk_get_prc() numbers from 1 (TP_PRC1=core0, TP_PRC2=core1, brief Step 2
     * item 1); used instead of Pico SDK's get_core_num() because this port's
     * Make build does not put hardware_structs/pico_platform on the include
     * path for every console/wifi profile (docs/curveball_findings.md). */
    return (unsigned)(tk_get_prc() - 1);
#else
    return 0;
#endif
}

rtos_id_t rtos_self_id(void)
{
    uint32_t ipsr;
    uint32_t primask;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    __asm volatile("mrs %0, primask" : "=r"(primask));
    if ((ipsr & 0x3fu) != 0u || (primask & 1u) != 0u)
    {
        return 0;
    }
    ID tid = tk_get_tid();
    return tid > 0 ? (rtos_id_t)tid : 0;
}

/* firmware_smp/buildglue/usb_tinyusb_glue_overlay.c (round 2 review fix A2). */
extern int tm_console_rx_getchar(void);

int rtos_console_getchar(void)
{
    return tm_console_rx_getchar();
}
