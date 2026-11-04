/*
 * usb_sdk_compat_overlay.c - replaces third_party/mtk3smp_rp2040/lib/libtm/sysdepend/pico_rp2040/
 * usb/usb_sdk_compat.c in the submodule's working tree (copied in by build_smp.sh, never
 * committed there, same pattern as usb_tinyusb_glue_overlay.c).
 *
 * Why: the vendored file implements irq_add_shared_handler()/irq_remove_handler()/
 * irq_set_enabled() for exactly one user, TinyUSB's USBCTRL interrupt, with a single global
 * handler pointer. The SmartRelay node firmware also needs the Pico SDK IRQ API for other
 * interrupts (wst_link.c installs a UART0 RX handler with irq_set_exclusive_handler()), and the
 * Pico SDK's own hardware_irq/irq.c cannot be linked next to the vendored file (both define the
 * same three functions: "multiple definition of irq_add_shared_handler" on a real link) nor used
 * instead of it (it writes the SDK's RAM vector table layout, not this kernel's knl_inthdr_tbl).
 *
 * This keeps the vendored USB behaviour byte-for-byte on the USBCTRL path (same handler call,
 * same tk_wup_tsk() of the USB service task, same diagnostics variables) and generalises the
 * handler storage to any IRQ number, with up to IRQ_MAX_HANDLERS handlers per IRQ, each IRQ
 * routed through tk_def_int() exactly the way the USB one already was.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include "pico.h"
#include "hardware/irq.h"

typedef void (*mtk_fp_t)();
typedef struct {
    uint32_t intatr;
    mtk_fp_t inthdr;
} mtk_dint_t;

extern int32_t tk_def_int(uint32_t intno, const mtk_dint_t *definition);
extern int32_t tk_wup_tsk(int32_t tskid);

/* Read by usb_console.c / usb_tinyusb_glue.c, unchanged from the vendored file. */
volatile int32_t tm_usb_service_tskid;
volatile uint32_t tm_usb_irq_count;
volatile int32_t  tm_usb_def_int_ercd = 1;
volatile uint32_t tm_usb_nvic_enabled;

#define MTK_TA_HLNG 1U
#define NVIC_ISER0 (*(volatile uint32_t *)0xe000e100u)
#define NVIC_ICER0 (*(volatile uint32_t *)0xe000e180u)
#define NVIC_IPR0 ((volatile uint32_t *)0xe000e400u)

#define IRQ_COUNT 32u
#define IRQ_MAX_HANDLERS 4u
#define IRQ_USBCTRL 5u

static irq_handler_t s_handlers[IRQ_COUNT][IRQ_MAX_HANDLERS];

/* Called by the kernel's interrupt entry with the IRQ number. Handler slots are only ever changed
 * with that IRQ masked on the calling core (see install()), and each IRQ in this firmware is only
 * enabled on the core that installed it, so the table needs no lock here. */
static void irq_dispatch(uint32_t intno)
{
    if (intno >= IRQ_COUNT) {
        return;
    }
    if (intno == IRQ_USBCTRL) {
        tm_usb_irq_count++;
    }
    for (uint32_t i = 0; i < IRQ_MAX_HANDLERS; i++) {
        irq_handler_t h = s_handlers[intno][i];
        if (h != NULL) {
            h();
        }
    }
    if (intno == IRQ_USBCTRL && tm_usb_service_tskid > 0) {
        /* Same as the vendored file: wake the USB service task now, not on the next tick. */
        (void)tk_wup_tsk(tm_usb_service_tskid);
    }
}

static void install(uint num, irq_handler_t handler, bool exclusive)
{
    if (num >= IRQ_COUNT || handler == NULL) {
        return;
    }
    uint32_t was_enabled = (NVIC_ISER0 >> num) & 1u;
    NVIC_ICER0 = 1UL << num;
    if (exclusive) {
        for (uint32_t i = 0; i < IRQ_MAX_HANDLERS; i++) {
            s_handlers[num][i] = NULL;
        }
        s_handlers[num][0] = handler;
    } else {
        bool placed = false;
        for (uint32_t i = 0; i < IRQ_MAX_HANDLERS && !placed; i++) {
            if (s_handlers[num][i] == handler) {
                placed = true;
            }
        }
        for (uint32_t i = 0; i < IRQ_MAX_HANDLERS && !placed; i++) {
            if (s_handlers[num][i] == NULL) {
                s_handlers[num][i] = handler;
                placed = true;
            }
        }
        if (!placed) {
            panic("irq %u: handler table full", num);
        }
    }
    mtk_dint_t dint;
    dint.intatr = MTK_TA_HLNG;
    dint.inthdr = (mtk_fp_t)irq_dispatch;
    int32_t er = tk_def_int(num, &dint);
    if (num == IRQ_USBCTRL) {
        tm_usb_def_int_ercd = er;
    }
    if (was_enabled) {
        NVIC_ISER0 = 1UL << num;
    }
}

void irq_set_exclusive_handler(uint num, irq_handler_t handler)
{
    install(num, handler, true);
}

void irq_add_shared_handler(uint num, irq_handler_t handler, uint8_t order_priority)
{
    (void)order_priority;
    install(num, handler, false);
}

irq_handler_t irq_get_exclusive_handler(uint num)
{
    return num < IRQ_COUNT ? s_handlers[num][0] : NULL;
}

/* hardware_timer's alarm code asks this to see whether its IRQ already has a handler. */
irq_handler_t irq_get_vtable_handler(uint num)
{
    return irq_get_exclusive_handler(num);
}

void irq_remove_handler(uint num, irq_handler_t handler)
{
    if (num >= IRQ_COUNT) {
        return;
    }
    NVIC_ICER0 = 1UL << num;
    bool any = false;
    for (uint32_t i = 0; i < IRQ_MAX_HANDLERS; i++) {
        if (s_handlers[num][i] == handler) {
            s_handlers[num][i] = NULL;
        }
        any = any || s_handlers[num][i] != NULL;
    }
    if (!any) {
        (void)tk_def_int(num, NULL);
    }
}

void irq_set_enabled(uint num, bool enabled)
{
    if (num >= IRQ_COUNT) {
        return;
    }
    if (enabled) {
        NVIC_ISER0 = 1UL << num;
    } else {
        NVIC_ICER0 = 1UL << num;
    }
    if (num == IRQ_USBCTRL) {
        tm_usb_nvic_enabled = (NVIC_ISER0 >> num) & 1UL;
    }
}

bool irq_is_enabled(uint num)
{
    return num < IRQ_COUNT && ((NVIC_ISER0 >> num) & 1u) != 0;
}

void irq_set_mask_enabled(uint32_t mask, bool enabled)
{
    if (enabled) {
        NVIC_ISER0 = mask;
    } else {
        NVIC_ICER0 = mask;
    }
}

void irq_set_priority(uint num, uint8_t hardware_priority)
{
    /* Left at the kernel's default level on purpose: every device IRQ in this kernel (USB
     * included) runs at the reset priority, and the kernel's interrupt masking assumes so. */
    (void)num;
    (void)hardware_priority;
}

uint8_t rp2040_chip_version(void)
{
    volatile uint32_t const *chip_id = (volatile uint32_t const *)0x40000000u;
    return (uint8_t)(*chip_id >> 28);
}

/* Fatal-error path for SDK code (hard_assert/panic). Defined weak so the node firmware can route
 * the message to its console first; this fallback just stops, as the vendored one did. */
__attribute__((weak)) void smartrelay_fatal(const char *msg)
{
    (void)msg;
}

void hard_assertion_failure(void)
{
    smartrelay_fatal("hard assertion failure");
    NVIC_ICER0 = 1UL << IRQ_USBCTRL;
    for (;;) __asm__ volatile ("wfi");
}

void __attribute__((noreturn)) panic(const char *format, ...)
{
    smartrelay_fatal(format);
    NVIC_ICER0 = 1UL << IRQ_USBCTRL;
    for (;;) __asm__ volatile ("wfi");
    __builtin_unreachable();
}
