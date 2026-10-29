/*
 * rtos_port_mtk3.c - rtos_port.h implemented over third_party/mtk3smp_rp2040.
 * Every call here is cited in docs/curveball_findings.md Step 2; nothing
 * below is a guessed signature.
 */
#include "rtos_port.h"

#include <string.h>

#include <tk/tkernel.h>
#if TK_SUPPORT_SMP
#include <tk/smp.h>
#endif

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
 * canary-filled at creation and scanned later instead. */
static uint8_t s_stack[RTOS_MAX_TASKS][RTOS_MAX_STACK_BYTES];
static unsigned s_task_count;

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
    if (s_task_count >= RTOS_MAX_TASKS || stack_bytes > RTOS_MAX_STACK_BYTES)
    {
        return -1;
    }
    unsigned slot = s_task_count++;
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
    ER er = tk_wai_flg((ID)flags, (UINT)mask, TWF_ORW | TWF_CLR, &ptn, (TMO)timeout_ms);
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
    cmbf.mbfatr = TA_TFIFO;
    cmbf.bufsz = (SZ)(msg_size * count);
    cmbf.maxmsz = (INT)msg_size;
    cmbf.bufptr = backing_buf;
    ID id = tk_cre_mbf(&cmbf);
    return (rtos_id_t)id;
}

bool rtos_msgq_send_nowait(rtos_id_t q, const void* msg, uint32_t msg_size)
{
    ER er = tk_snd_mbf((ID)q, msg, (INT)msg_size, TMO_POL);
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
