/*
 * usb_tinyusb_glue_overlay.c - round 2 review fix A2.
 *
 * OVERLAY, not a submodule edit: build_smp.sh copies this file over
 * third_party/mtk3smp_rp2040/lib/libtm/sysdepend/pico_rp2040/usb/
 * usb_tinyusb_glue.c at build time, never committed into the submodule
 * (third_party/MTK3SMP_PROVENANCE.md). This is the original file at commit
 * 900e863 verbatim (see its own header comment below) with exactly one
 * functional change: tm_tusb_discard_input() used to throw every received
 * byte away (`while (tud_cdc_available()) tud_cdc_read_char();`); it now
 * pushes those same bytes into a small ring that tm_console_rx_getchar()
 * (declared in firmware_smp/rtos_port.h, called from
 * firmware_smp/app/main_smp.c's usb_owner_task) can read. tm_getchar(0)
 * cannot be used for this - third_party/mtk3smp_rp2040/lib/libtm/
 * sysdepend/pico_rp2040/tm_com.c:95-106 returns 0-filled bytes whenever
 * TM_CONSOLE_UART is off (our CONSOLE=usb_cdc, WIFI=none profile never
 * turns it on), so T-Monitor's own RX path is dead on this build - the
 * console keys (c/e/s/d/t/x/z/h) never worked before this fix.
 *
 * Both tm_tusb_discard_input() and the new accessor only ever run from
 * usb_service_task (the existing CDC service task in usb_console.c, pinned
 * to processor 1/core 0) and usb_owner_task (firmware_smp's console task,
 * also core 0) respectively - two different tasks on the same core, same
 * single-producer/single-consumer shape already used for wst_link.c's UART
 * RX ring, so the same volatile+barrier treatment (no ISpinLock needed)
 * applies here too.
 */

/* Keep Pico SDK types out of translation units that include µT-Kernel. */

#include "tusb.h"

uint32_t tusb_time_millis_api(void)
{
    volatile uint32_t const* timer_raw_low = (volatile uint32_t const*)0x40054028u;
    return *timer_raw_low / 1000U;
}

int tm_tusb_init(void)
{
    return tusb_init() ? 1 : 0;
}

void tm_tusb_service(void)
{
    tud_task();
}

/* True while the device stack still has queued events to process.  Used to
   drain a whole burst without returning to the scheduler between events. */
int tm_tusb_event_ready(void)
{
    return tud_task_event_ready() ? 1 : 0;
}

/*
 * Enumerated and configured by the host.  This, not tud_cdc_connected(), is
 * the correct gate for draining output: tud_cdc_connected() reports the CDC
 * line state (DTR), which many terminal programs never assert, so gating on
 * it silently discards all output on an otherwise working link.
 */
int tm_tusb_mounted(void)
{
    return tud_mounted() ? 1 : 0;
}

/* DTR from the host.  Diagnostic only -- do not gate output on this. */
int tm_tusb_connected(void)
{
    return tud_cdc_connected() ? 1 : 0;
}

unsigned tm_tusb_write_available(void)
{
    return tud_cdc_write_available();
}

void tm_tusb_write_char(char value)
{
    (void)tud_cdc_write_char(value);
}

void tm_tusb_write_flush(void)
{
    (void)tud_cdc_write_flush();
}

/* CURVEBALL round 2 fix A2: small ring between this (producer, called from
 * usb_service_task) and tm_console_rx_getchar (consumer, called from
 * usb_owner_task). Both tasks are pinned to core 0, the same same-core
 * producer-task/consumer-task shape as wst_link.c's UART ring - volatile
 * plus a compiler barrier is enough, no spinlock needed. */
#define CONSOLE_RX_RING_SIZE 32u
static volatile unsigned char console_rx_ring[CONSOLE_RX_RING_SIZE];
static volatile unsigned console_rx_head, console_rx_tail;

void tm_tusb_discard_input(void)
{
    while (tud_cdc_available())
    {
        unsigned char b = (unsigned char)tud_cdc_read_char();
        unsigned next = (console_rx_head + 1u) % CONSOLE_RX_RING_SIZE;
        if (next == console_rx_tail)
        {
            continue; /* ring full: drop the byte, same policy as the old discard */
        }
        console_rx_ring[console_rx_head] = b;
        __asm volatile("" ::: "memory");
        console_rx_head = next;
    }
}

int tm_console_rx_getchar(void)
{
    if (console_rx_tail == console_rx_head)
    {
        return -1;
    }
    unsigned char b = console_rx_ring[console_rx_tail];
    __asm volatile("" ::: "memory");
    console_rx_tail = (console_rx_tail + 1u) % CONSOLE_RX_RING_SIZE;
    return (int)b;
}

/*
 * Diagnostics.  The console cannot report on the console, so these are read
 * out over GPIO when USB fails to enumerate.
 */
#include "hardware/structs/usb.h"
#include "hardware/regs/usb.h"

uint32_t tm_tusb_diag_controller_en(void)
{
    return (usb_hw->main_ctrl & USB_MAIN_CTRL_CONTROLLER_EN_BITS) ? 1u : 0u;
}

uint32_t tm_tusb_diag_pullup_en(void)
{
    return (usb_hw->sie_ctrl & USB_SIE_CTRL_PULLUP_EN_BITS) ? 1u : 0u;
}

uint32_t tm_tusb_diag_bus_seen(void)
{
    /* Either the host has driven a bus reset, or the SIE reports a
       connection.  Both indicate the host is actually talking to us. */
    return (usb_hw->sie_status & (USB_SIE_STATUS_BUS_RESET_BITS | USB_SIE_STATUS_CONNECTED_BITS)) ? 1u : 0u;
}

uint32_t tm_tusb_diag_inte(void)
{
    return usb_hw->inte;
}
