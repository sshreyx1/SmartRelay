/*
 * board.h - the CSC09 node wiring, in one place. Every pin the firmware uses is listed here.
 *
 * Maker Pi Pico + Pico W. The microSD card sits in the Maker board's own slot, which is wired to
 * SPI1 (GP10 SCK, GP11 MOSI, GP12 MISO, GP15 CS). The LoRa module (Core1121 / LR1121) is on the
 * SAME SPI1 bus with its own chip select (GP13). One task (comms) owns both, so they never talk
 * at the same time.
 *
 *   GP0   PIR OUT (Grove port 1)                    Maryam    pir.h
 *   GP4   DHT22 DATA                                Maryam    dht22.c
 *   GP5   LoRa RESET                                Mridula   gpio.h
 *   GP6   LoRa BUSY        (moved from GP14)        Mridula   CMake LORA_BUSY_PIN
 *   GP7   LoRa DIO9 / IRQ  (moved from GP15)        Mridula   CMake LORA_IRQ_PIN
 *   GP10  SPI1 SCK   shared: LoRa SCK + SD CLK
 *   GP11  SPI1 MOSI  shared: LoRa MOSI + SD CMD
 *   GP12  SPI1 MISO  shared: LoRa MISO + SD DAT0
 *   GP13  LoRa CS (NSS)   (also the SD slot's DAT1, unused in SPI mode)
 *   GP14  sound trigger OUT (also the SD slot's DAT2, unused in SPI mode)  Rhaylene  sound.h
 *   GP15  microSD CS (Maker slot, fixed on the board)
 *   GP16  UART0 TX -> ESP-01 (WST) RX                Goutham   wst_link.c
 *   GP17  UART0 RX <- ESP-01 (WST) TX                Goutham   wst_link.c
 *   GP20  Maker button 1 = "capture now" (demo)
 *   GP26  ADC0  MAX9814 mic OUT                      Rhaylene  mic.h
 *   GP27  ADC1  light module AO                      Maryam
 *
 * USB serial is the console. UART stdio is OFF because UART0 belongs to the ESP-01 and the
 * default UART stdio pin (GP0) is the PIR.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

#define BOARD_LORA_CS_GPIO 13
#define BOARD_SD_CS_GPIO 15
#define BOARD_BUTTON_GPIO 20
#define BOARD_LIGHT_GPIO 27
#define BOARD_LIGHT_ADC 1 /* GP27 = ADC input 1 */
#define BOARD_MIC_ADC 0   /* GP26 = ADC input 0 */

#ifndef SPI1_BAUD_HZ
#define SPI1_BAUD_HZ                                                                                                   \
    4000000u /* shared bus speed; 1 MHz (what Mridula first tested) made every SD                                      \
              * directory lookup ~4x slower, see docs/verify/RESULTS.md */
#endif

/* Call first thing in main(): both chip selects high, so neither chip listens while the other
 * one is being set up. Without this, the SD card (still in SD mode before its first command)
 * could answer LoRa traffic and corrupt it. */
void board_spi1_idle(void);

/* Call after lora_mesh_init(): Mridula's driver re-initialises SPI1 at its own speed. This puts
 * the bus back to SPI1_BAUD_HZ, the speed the SD driver was told it runs at. */
void board_spi1_after_lora(void);

#endif
