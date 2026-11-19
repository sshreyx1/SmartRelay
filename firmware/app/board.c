/*
 * board.c - see board.h.
 */
#include "board.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"

void board_spi1_idle(void)
{
    const uint pins[2] = { BOARD_LORA_CS_GPIO, BOARD_SD_CS_GPIO };
    for (int i = 0; i < 2; i++)
    {
        gpio_init(pins[i]);
        gpio_put(pins[i], 1);
        gpio_set_dir(pins[i], GPIO_OUT);
    }
}

void board_spi1_after_lora(void)
{
    spi_set_baudrate(spi1, SPI1_BAUD_HZ);
}
