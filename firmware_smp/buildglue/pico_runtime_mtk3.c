/*
 * pico_runtime_mtk3.c - the parts of the Pico SDK runtime the SmartRelay drivers rely on, provided
 * on top of uT-Kernel 3 SMP instead of by Pico SDK's crt0/runtime_init (which this kernel replaces
 * with its own reset handler, clock setup and vector table).
 *
 * Everything here exists because a driver the FreeRTOS firmware links (firmware/CMakeLists.txt:
 * carlk3 FatFs/SD over SPI, Mridula's LR1121 LoRa driver, Goutham's ESP-01 UART link, the sensor
 * drivers) calls into that runtime and would otherwise misbehave silently:
 *
 *   - TIMER: time_us_64()/busy_wait_us() read the RP2040 TIMER, which counts only once the
 *     watchdog tick generator runs and the block is out of reset. The kernel does neither (it
 *     uses SysTick). Without this, every SDK timeout loop (SD card, LR1121 BUSY wait, WiFi frame
 *     timeout) would spin forever.
 *   - clocks: spi_set_baudrate()/uart_set_baudrate() divide clock_get_hz(clk_peri), which the SDK
 *     only knows if its own clock_configure() ran. The kernel programs the PLLs itself, so the
 *     real frequencies are measured with the RP2040 frequency counter and reported to the SDK.
 *   - DMA/PWM/PIO resets: the kernel releases only IO/PADS/UART0 (hw_setting.c module_tbl);
 *     the mic capture uses DMA.
 *   - pico/mutex.h: the carlk3 SD driver locks its card and SPI bus with SDK mutexes, some
 *     declared with auto_init_mutex(), which Pico SDK's runtime would initialise before main().
 *     Implemented here with lazy initialisation, owner = kernel task id, and a kernel delay while
 *     waiting (no spinning on a lock held by a lower-priority task on the same core).
 *   - sleep_ms()/sleep_us(): a kernel delay when called from a task, a busy wait otherwise, so
 *     driver delays (LR1121 reset, DHT22 start pulse) give the CPU to other tasks like they did
 *     under FreeRTOS.
 *   - get_rand_32(): ALFP/mesh jitter only needs a well-mixed value; seeded from the ROSC random
 *     bit and the microsecond timer instead of linking pico_rand (whose board-id entropy source
 *     reads the flash unique id with XIP disabled, unsafe while the other core runs from flash).
 *   - newlib: malloc (FatFs long file names use it) gets a cross-core lock and a bounded heap;
 *     printf/puts/putchar and stdout writes are routed to smartrelay_stdout_putc(), which the node
 *     firmware implements as a per-task line buffer feeding its log ring, so output from both
 *     cores never interleaves mid-line. Formatting uses Pico SDK's pico_printf (float support,
 *     no malloc, reentrant), the same formatter the FreeRTOS build's printf uses.
 */

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <reent.h>

#include "hardware/clocks.h"
#include "hardware/resets.h"
#include "hardware/structs/rosc.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "pico/mutex.h"
#include "pico/time.h"

#include "pico_runtime_mtk3.h"

/* Kernel calls, declared here rather than through <tk/tkernel.h>: the kernel's register-name
 * macros collide with the Pico SDK hardware headers this file needs (same approach as the
 * kernel's own usb_sdk_compat.c). */
extern int32_t tk_dly_tsk(int32_t dlytim);
extern int32_t tk_get_tid(void);
extern int32_t tk_slp_tsk(int32_t tmout);
extern void tm_usb_console_put(const uint8_t *buf, int32_t size);

#define KERNEL_TMO_FEVR (-1)

/* Hardware spin locks 0-2 belong to the kernel (kernel/sysdepend/cpu/rp2040/smp_atomic.c).
 * PICO_SPINLOCK_ID_OS1 (14) is the one Pico SDK reserves for an RTOS; this file uses it for its
 * short critical sections (never held across a call that can block). */
#define GLUE_SPINLOCK_ID 14u

#ifndef SMARTRELAY_HEAP_BYTES
#define SMARTRELAY_HEAP_BYTES (24u * 1024u)
#endif

static spin_lock_t *glue_lock(void)
{
    return spin_lock_instance(GLUE_SPINLOCK_ID);
}

/* ---- context ---------------------------------------------------------------------------- */

bool smartrelay_in_task(void)
{
    uint32_t ipsr;
    uint32_t primask;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    __asm volatile("mrs %0, primask" : "=r"(primask));
    if ((ipsr & 0x3fu) != 0u || (primask & 1u) != 0u) {
        return false;
    }
    return tk_get_tid() > 0;
}

static int32_t self_id(void)
{
    if (smartrelay_in_task()) {
        return tk_get_tid();
    }
    /* handler context: never a task id, and never -1 (LOCK_INVALID_OWNER_ID, "mutex free") */
    return -2 - (int32_t)get_core_num();
}

static void glue_yield(void)
{
    if (smartrelay_in_task()) {
        (void)tk_dly_tsk(1);
    }
}

/* ---- start-up --------------------------------------------------------------------------- */

#define REF_HZ 12000000u /* kernel cpu_clock.c: clk_ref = XOSC (12 MHz) / 1 */

static uint32_t measure_hz(uint fc0_src)
{
    return frequency_count_khz(fc0_src) * 1000u;
}

/* smartrelay_sections.ld: NOLOAD, so zeroed here (an all-zero mutex_t is "not initialised yet"). */
extern uint8_t __mutex_array_start[];
extern uint8_t __mutex_array_end[];

void smartrelay_runtime_init(void)
{
    memset(__mutex_array_start, 0, (size_t)(__mutex_array_end - __mutex_array_start));

    /* Blocks Pico SDK's runtime would have released but the kernel does not. Releasing a block
     * that is already running is harmless; nothing is put INTO reset here. */
    unreset_block_wait(RESETS_RESET_DMA_BITS | RESETS_RESET_TIMER_BITS | RESETS_RESET_PWM_BITS |
                       RESETS_RESET_PIO0_BITS | RESETS_RESET_PIO1_BITS);

    /* TIMER ticks at 1 MHz from clk_ref via the watchdog tick divider. */
    watchdog_hw->tick = WATCHDOG_TICK_ENABLE_BITS | (REF_HZ / 1000000u);

    clock_set_reported_hz(clk_ref, REF_HZ);
    clock_set_reported_hz(clk_sys, measure_hz(CLOCKS_FC0_SRC_VALUE_CLK_SYS));
    clock_set_reported_hz(clk_peri, measure_hz(CLOCKS_FC0_SRC_VALUE_CLK_PERI));
    clock_set_reported_hz(clk_usb, measure_hz(CLOCKS_FC0_SRC_VALUE_CLK_USB));
    clock_set_reported_hz(clk_adc, measure_hz(CLOCKS_FC0_SRC_VALUE_CLK_ADC));
    clock_set_reported_hz(clk_rtc, measure_hz(CLOCKS_FC0_SRC_VALUE_CLK_RTC));
}

/* ---- pico/mutex.h ----------------------------------------------------------------------- */

void mutex_init(mutex_t *mtx)
{
    uint32_t save = spin_lock_blocking(glue_lock());
    mtx->owner = LOCK_INVALID_OWNER_ID;
    mtx->core.spin_lock = glue_lock();
    spin_unlock(glue_lock(), save);
}

/* auto_init_mutex() objects start all-zero (spin_lock == NULL): initialise on first use. */
static void mutex_lazy_init(mutex_t *mtx)
{
    if (mtx->core.spin_lock == NULL) {
        uint32_t save = spin_lock_blocking(glue_lock());
        if (mtx->core.spin_lock == NULL) {
            mtx->owner = LOCK_INVALID_OWNER_ID;
            mtx->core.spin_lock = glue_lock();
        }
        spin_unlock(glue_lock(), save);
    }
}

bool mutex_try_enter(mutex_t *mtx, uint32_t *owner_out)
{
    mutex_lazy_init(mtx);
    lock_owner_id_t me = (lock_owner_id_t)self_id();
    bool ok;
    uint32_t save = spin_lock_blocking(glue_lock());
    if (mtx->owner == LOCK_INVALID_OWNER_ID) {
        mtx->owner = me;
        ok = true;
    } else {
        if (owner_out != NULL) {
            *owner_out = (uint32_t)(int32_t)mtx->owner;
        }
        ok = false;
    }
    spin_unlock(glue_lock(), save);
    return ok;
}

void mutex_enter_blocking(mutex_t *mtx)
{
    while (!mutex_try_enter(mtx, NULL)) {
        glue_yield();
    }
}

bool mutex_enter_block_until(mutex_t *mtx, absolute_time_t until)
{
    while (!mutex_try_enter(mtx, NULL)) {
        if (time_reached(until)) {
            return false;
        }
        glue_yield();
    }
    return true;
}

bool mutex_enter_timeout_ms(mutex_t *mtx, uint32_t timeout_ms)
{
    return mutex_enter_block_until(mtx, make_timeout_time_ms(timeout_ms));
}

bool mutex_enter_timeout_us(mutex_t *mtx, uint32_t timeout_us)
{
    return mutex_enter_block_until(mtx, make_timeout_time_us(timeout_us));
}

void mutex_exit(mutex_t *mtx)
{
    uint32_t save = spin_lock_blocking(glue_lock());
    mtx->owner = LOCK_INVALID_OWNER_ID;
    spin_unlock(glue_lock(), save);
}

/* ---- pico/time.h sleeps ------------------------------------------------------------------ */

void sleep_us(uint64_t us)
{
    if (us >= 1000u && smartrelay_in_task()) {
        (void)tk_dly_tsk((int32_t)(us / 1000u));
        busy_wait_us_32((uint32_t)(us % 1000u));
    } else {
        busy_wait_us(us);
    }
}

void sleep_ms(uint32_t ms)
{
    sleep_us((uint64_t)ms * 1000u);
}

void sleep_until(absolute_time_t target)
{
    int64_t d = absolute_time_diff_us(get_absolute_time(), target);
    if (d > 0) {
        sleep_us((uint64_t)d);
    }
}

bool best_effort_wfe_or_timeout(absolute_time_t timeout_timestamp)
{
    if (time_reached(timeout_timestamp)) {
        return true;
    }
    glue_yield();
    return time_reached(timeout_timestamp);
}

/* ---- random numbers ---------------------------------------------------------------------- */

static uint64_t s_rng_state;

uint64_t get_rand_64(void)
{
    uint32_t save = spin_lock_blocking(glue_lock());
    if (s_rng_state == 0u) {
        uint64_t seed = time_us_64();
        for (int i = 0; i < 64; i++) {
            seed = (seed << 1) ^ (seed >> 63) ^ (uint64_t)(rosc_hw->randombit & 1u);
            busy_wait_at_least_cycles(37);
        }
        s_rng_state = seed | 1u;
    }
    /* xorshift64*, mixed with the free-running timer on every draw */
    uint64_t x = s_rng_state ^ (time_us_64() << 17);
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s_rng_state = x ? x : 0x9E3779B97F4A7C15ull;
    spin_unlock(glue_lock(), save);
    return x * 0x2545F4914F6CDD1Dull;
}

uint32_t get_rand_32(void)
{
    return (uint32_t)(get_rand_64() >> 32);
}

/* ---- newlib heap -------------------------------------------------------------------------- */

static uint8_t s_heap[SMARTRELAY_HEAP_BYTES] __attribute__((aligned(8)));
static size_t s_heap_used;

void *_sbrk(ptrdiff_t incr)
{
    if (incr < 0 || (size_t)incr > sizeof(s_heap) - s_heap_used) {
        errno = ENOMEM;
        return (void *)-1;
    }
    void *p = &s_heap[s_heap_used];
    s_heap_used += (size_t)incr;
    return p;
}

uint32_t smartrelay_heap_used(void)
{
    return (uint32_t)s_heap_used;
}

static volatile int32_t s_malloc_owner; /* 0 = free */
static uint32_t s_malloc_depth;

void __malloc_lock(struct _reent *r)
{
    (void)r;
    int32_t me = self_id();
    for (;;) {
        uint32_t save = spin_lock_blocking(glue_lock());
        if (s_malloc_owner == 0 || s_malloc_owner == me) {
            s_malloc_owner = me;
            s_malloc_depth++;
            spin_unlock(glue_lock(), save);
            return;
        }
        spin_unlock(glue_lock(), save);
        glue_yield();
    }
}

void __malloc_unlock(struct _reent *r)
{
    (void)r;
    uint32_t save = spin_lock_blocking(glue_lock());
    if (s_malloc_depth > 0u && --s_malloc_depth == 0u) {
        s_malloc_owner = 0;
    }
    spin_unlock(glue_lock(), save);
}

/* ---- stdout --------------------------------------------------------------------------------- */

/* pico_printf (compiled with PICO_PRINTF_ALWAYS_INCLUDED=1, see sources.mk) */
extern int __wrap_vsnprintf(char *buffer, size_t count, const char *format, va_list va);
extern int vfctprintf(void (*out)(char character, void *arg), void *arg, const char *format, va_list va);

__attribute__((weak)) void smartrelay_stdout_putc(char c)
{
    (void)c;
}

static void out_char(char c, void *arg)
{
    (void)arg;
    smartrelay_stdout_putc(c);
}

int vsnprintf(char *buffer, size_t count, const char *format, va_list va)
{
    return __wrap_vsnprintf(buffer, count, format, va);
}

int snprintf(char *buffer, size_t count, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    int n = __wrap_vsnprintf(buffer, count, format, va);
    va_end(va);
    return n;
}

int sprintf(char *buffer, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    int n = __wrap_vsnprintf(buffer, (size_t)-1, format, va);
    va_end(va);
    return n;
}

int vsprintf(char *buffer, const char *format, va_list va)
{
    return __wrap_vsnprintf(buffer, (size_t)-1, format, va);
}

int vprintf(const char *format, va_list va)
{
    return vfctprintf(out_char, NULL, format, va);
}

int printf(const char *format, ...)
{
    va_list va;
    va_start(va, format);
    int n = vfctprintf(out_char, NULL, format, va);
    va_end(va);
    return n;
}

int puts(const char *s)
{
    int n = 0;
    while (s[n] != '\0') {
        smartrelay_stdout_putc(s[n]);
        n++;
    }
    smartrelay_stdout_putc('\n');
    return n + 1;
}

int putchar(int c)
{
    smartrelay_stdout_putc((char)c);
    return c;
}

/* pico/stdio.h: lines leave as soon as they are complete (log.c), nothing to flush. */
void stdio_flush(void)
{
}

/* newlib's own stdio (if any library code reaches it) ends up here for stdout/stderr. */
int _write(int fd, const char *buf, int len)
{
    if (fd != 1 && fd != 2) {
        errno = EBADF;
        return -1;
    }
    for (int i = 0; i < len; i++) {
        smartrelay_stdout_putc(buf[i]);
    }
    return len;
}

/* ---- fatal errors (usb_sdk_compat_overlay.c's panic()/hard_assertion_failure()) ---------- */

void smartrelay_fatal(const char *msg)
{
    static const char prefix[] = "\r\nFATAL: ";
    tm_usb_console_put((const uint8_t *)prefix, (int32_t)(sizeof(prefix) - 1u));
    if (msg != NULL) {
        tm_usb_console_put((const uint8_t *)msg, (int32_t)strlen(msg));
    }
    tm_usb_console_put((const uint8_t *)"\r\n", 2);
    if (smartrelay_in_task()) {
        /* Park only the failing task; the USB service task keeps the message flowing out and
         * the other core keeps running. */
        for (;;) {
            (void)tk_slp_tsk(KERNEL_TMO_FEVR);
        }
    }
}

/* ---- odds and ends Pico SDK's crt0/linker script would provide -------------------------- */

/* Referenced by hardware_irq headers' default-handler machinery; no SDK vector table here. */
void __unhandled_user_irq(void)
{
    for (;;) {
    }
}

/* Reboot into the RP2040 USB bootloader (BOOTSEL) via the boot ROM, so a node can be reflashed
 * from the console without touching the button. Same ROM call Pico SDK's reset_usb_boot() makes. */
void smartrelay_reboot_to_bootsel(void)
{
    typedef void *(*rom_table_lookup_fn)(uint16_t *table, uint32_t code);
    typedef void (*rom_reset_usb_boot_fn)(uint32_t gpio_mask, uint32_t disable_interface_mask);
    uint16_t *func_table = (uint16_t *)(uintptr_t)(*(uint16_t *)0x14u);
    rom_table_lookup_fn lookup = (rom_table_lookup_fn)(uintptr_t)(*(uint16_t *)0x18u);
    rom_reset_usb_boot_fn reset_usb_boot =
        (rom_reset_usb_boot_fn)lookup(func_table, (uint32_t)'U' | ((uint32_t)'B' << 8));
    reset_usb_boot(0u, 0u);
    for (;;) {
    }
}
