/*
 * pico_runtime_mtk3.h - see pico_runtime_mtk3.c.
 */
#ifndef PICO_RUNTIME_MTK3_H
#define PICO_RUNTIME_MTK3_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Call once from usermain(), before any task touches a Pico SDK driver. */
    void smartrelay_runtime_init(void);

    /* true when running in a kernel task with interrupts enabled (so a kernel delay is allowed). */
    bool smartrelay_in_task(void);

    /* Every printf/puts/putchar byte lands here. Weak default discards; the firmware overrides it. */
    void smartrelay_stdout_putc(char c);

    /* Bytes of the newlib heap handed out so far (it never shrinks). */
    uint32_t smartrelay_heap_used(void);

    /* Reboot into the USB bootloader (RPI-RP2 drive). Does not return. */
    void smartrelay_reboot_to_bootsel(void);

#ifdef __cplusplus
}
#endif

#endif /* PICO_RUNTIME_MTK3_H */
