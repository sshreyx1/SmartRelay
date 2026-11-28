/*
 * hw_config.c - microSD wiring for the carlk3 FatFs driver.
 *
 * The card is in the Maker Pi Pico's built-in slot: SPI1, GP10 SCK, GP11 MOSI, GP12 MISO, GP15 CS.
 * The LoRa module shares GP10/11/12 and has its own CS on GP13 (see board.h). Both run at
 * SPI1_BAUD_HZ (4 MHz by default, was 1 MHz); baud_rate below is kept equal to it so both chips always see the
 * same clock, whichever of them set the bus up last. Pins can be overridden with -DSD_SCK=... etc.
 */
#include "hw_config.h"
#include "board.h"

#ifndef SD_SPI_INST
#define SD_SPI_INST 1
#endif
#ifndef SD_SCK
#define SD_SCK 10
#endif
#ifndef SD_MOSI
#define SD_MOSI 11
#endif
#ifndef SD_MISO
#define SD_MISO 12
#endif
#ifndef SD_CS
#define SD_CS 15
#endif

#if SD_SPI_INST
#define SD_SPI_HW spi1
#else
#define SD_SPI_HW spi0
#endif

static spi_t spi = {
    .hw_inst = SD_SPI_HW,
    .sck_gpio = SD_SCK,
    .mosi_gpio = SD_MOSI,
    .miso_gpio = SD_MISO,
    .baud_rate = SPI1_BAUD_HZ, /* shared with the LoRa module, see board.h */
};

static sd_spi_if_t spi_if = {
    .spi = &spi,
    .ss_gpio = SD_CS,
};

static sd_card_t sd_card = {
    .type = SD_IF_SPI,
    .spi_if_p = &spi_if,
};

size_t sd_get_num(void)
{
    return 1;
}

sd_card_t* sd_get_by_num(size_t num)
{
    return 0 == num ? &sd_card : NULL;
}
