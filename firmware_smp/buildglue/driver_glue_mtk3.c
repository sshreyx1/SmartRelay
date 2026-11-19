/*
 * driver_glue_mtk3.c - the few driver entry points the SMP build references but does not compile
 * the original source for (see sources.mk). Each one is listed with why.
 *
 * carlk3 FatFs/SD (SRC_fatfs):
 *   sd_sdio_ctor / rp2040_sdio_get_sd_status: referenced by sd_card.c's SD_IF_SDIO branches.
 *     The SDIO driver (a PIO program plus a shared DMA interrupt) is not built; hw_config.c only
 *     ever describes an SD_IF_SPI card, so these are unreachable. They stop loudly if that ever
 *     changes instead of failing at link time.
 *   get_fattime: FatFs file timestamps. The FreeRTOS build gets it from carlk3's my_rtc.c, which
 *     returns 0 until something sets the RP2040 RTC -- and nothing in SmartRelay does. Same value
 *     here, without linking the RTC/always-on-timer code.
 *   capture_assert / capture_assert_case_not: carlk3's crash.c records a failed assertion in
 *     no-init RAM and resets through the watchdog. Here the message goes to the console and the
 *     failing task stops (panic), which keeps the other core and the console alive to report it.
 *
 * LR1121 driver (SRC_lora):
 *   lr11xx_hal_abort_blocking_cmd: declared by the Semtech driver's HAL interface and called only
 *     from lr11xx_system_abort_blocking_cmd(), which the mesh never calls. Mridula's HAL
 *     (lr11xx_hal.c) does not implement it; the FreeRTOS build only links because its
 *     -ffunction-sections/--gc-sections drops the unused caller, which this kernel's link does
 *     not do.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ff.h"
#include "lr11xx_hal.h"
#include "pico.h"
#include "sd_card.h"

void sd_sdio_ctor(sd_card_t* sd_card_p)
{
    (void)sd_card_p;
    panic("sd: SDIO interface is not built into the SMP firmware (use SD_IF_SPI)");
}

bool rp2040_sdio_get_sd_status(sd_card_t* sd_card_p, uint8_t response[64])
{
    (void)sd_card_p;
    (void)response;
    return false;
}

DWORD get_fattime(void)
{
    return 0; /* what my_rtc.c's get_fattime() returns while the RTC has never been set */
}

void capture_assert(const char* file, int line, const char* func, const char* pred)
{
    printf("assertion \"%s\" failed: %s:%d (%s)\n", pred, file, line, func);
    panic("assertion failed in the SD card driver");
}

void capture_assert_case_not(const char* file, int line, const char* func, int v)
{
    printf("unexpected case %d: %s:%d (%s)\n", v, file, line, func);
    panic("unexpected case in the SD card driver");
}

lr11xx_hal_status_t lr11xx_hal_abort_blocking_cmd(const void* context)
{
    (void)context;
    return LR11XX_HAL_STATUS_ERROR;
}
