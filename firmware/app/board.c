/*
 * board.c - see board.h.
 */
#include "board.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"

void board_spi1_idle(void)
{
    const uint pins[2] = { BOARD_LORA_CS_GPIO, BOARD_SD_CS_GPIO };
    for (int idx = 0; idx < 2; idx++)
    {
        gpio_init(pins[idx]);
        gpio_put(pins[idx], 1);
        gpio_set_dir(pins[idx], GPIO_OUT);
    }
}

void board_spi1_after_lora(void)
{
    spi_set_baudrate(spi1, SPI1_BAUD_HZ);
}
