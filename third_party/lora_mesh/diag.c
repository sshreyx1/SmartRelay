/*
 * diag.c - Wiring diagnostic for the Core1121 (LR1121). Temporary tool, not
 * part of the mesh.
 *
 * Talks to the chip directly with the Pico SDK (no Semtech driver), tests each
 * wire, and ends every round with a plain-English VERDICT. Repeats every 3 s,
 * so you can change a wire (with USB unplugged) and watch the verdict change.
 *
 * Pins come from src/gpio/gpio.h, the SPI port from src/spi/spi.h.
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/spi.h"
#include "gpio.h" // RADIO_* pins
#include "spi.h"  // SPI_PORT

/* Read a pin with pull-up, then pull-down. Same value both times = something
 * drives it.
 * Different = floating (nothing connected / chip not powered). */
static bool is_driven (uint pin)
{
    gpio_pull_up(pin);
    sleep_us(200);
    const int link_up = gpio_get(pin);
    gpio_pull_down(pin);
    sleep_us(200);
    const int down = gpio_get(pin);
    gpio_disable_pulls(pin);
    return link_up == down;
}

/* Returns ms until BUSY went low, or -1 if it stayed high */
static int wait_busy_low (uint32_t timeout_ms)
{
    const absolute_time_t start_t = get_absolute_time();
    while (gpio_get(RADIO_BUSY))
    {
        if (absolute_time_diff_us(start_t, get_absolute_time()) >
            (int64_t)timeout_ms * 1000)
        {
            return -1;
        }
    }
    return (int)(absolute_time_diff_us(start_t, get_absolute_time()) / 1000);
}

/* Sends GetVersion and prints the 5 reply bytes. Returns true if the chip said
 * "LR1121". */
static bool get_version_raw (uint baud, bool b_miso_pull_up)
{
    uint8_t cmd[2]    = {0x01, 0x01}; // GetVersion
    uint8_t rx_buf[5] = {0};

    spi_set_baudrate(SPI_PORT, baud);
    if (b_miso_pull_up)
    {
        gpio_pull_up(RADIO_MISO);
    }
    else
    {
        gpio_pull_down(RADIO_MISO);
    }

    gpio_put(RADIO_CS, 0);
    spi_write_blocking(SPI_PORT, cmd, 2);
    gpio_put(RADIO_CS, 1);
    wait_busy_low(1000);
    gpio_put(RADIO_CS, 0);
    spi_read_blocking(SPI_PORT, 0x00, rx_buf, 5);
    gpio_put(RADIO_CS, 1);
    gpio_disable_pulls(RADIO_MISO);

    const bool b_is_ok = 0x03 == rx_buf[2];
    printf("  %4u kHz, MISO pull-%-4s: %02X %02X %02X %02X %02X%s\n",
           baud / 1000, b_miso_pull_up ? "up" : "down", rx_buf[0], rx_buf[1],
           rx_buf[2], rx_buf[3], rx_buf[4], b_is_ok ? "  <- LR1121 OK" : "");
    return b_is_ok;
}

/* With CS high the chip lets go of MISO, so a GP11->GP12 jumper makes the Pico
 * hear itself */
static bool loopback (void)
{
    const uint8_t pattern[4] = {0xA5, 0x5A, 0x0F, 0xF0};
    uint8_t       back[4]    = {0};
    spi_set_baudrate(SPI_PORT, 1000 * 1000);
    gpio_put(RADIO_CS, 1);
    spi_write_read_blocking(SPI_PORT, pattern, back, 4);
    const bool b_is_ok = (0xA5 == back[0]) && (0x5A == back[1]) &&
                         (0x0F == back[2]) && (0xF0 == back[3]);
    printf("4. Loopback (needs jumper GP11->GP12): sent A5 5A 0F F0, got %02X "
           "%02X %02X %02X%s\n",
           back[0], back[1], back[2], back[3],
           b_is_ok ? "  <- Pico SPI pins OK" : "");
    return b_is_ok;
}

int main (void)
{
    stdio_init_all();
    /* wait for the serial monitor so nothing is missed */
    while (!stdio_usb_connected())
    {
        sleep_ms(10);
    }
    sleep_ms(500);
    printf("\n=== Core1121 wiring diagnostic ===\n");
    printf("Pins: RESET GP%d, CLK GP%d, MOSI GP%d, MISO GP%d, CS GP%d, BUSY "
           "GP%d, DIO9 GP%d\n",
           RADIO_RESET, RADIO_CLK, RADIO_MOSI, RADIO_MISO, RADIO_CS, RADIO_BUSY,
           RADIO_IRQ);

    gpio_init(RADIO_RESET);
    gpio_set_dir(RADIO_RESET, GPIO_OUT);
    gpio_put(RADIO_RESET, 1);
    gpio_init(RADIO_CS);
    gpio_set_dir(RADIO_CS, GPIO_OUT);
    gpio_put(RADIO_CS, 1);
    gpio_init(RADIO_BUSY);
    gpio_set_dir(RADIO_BUSY, GPIO_IN);
    gpio_init(RADIO_IRQ);
    gpio_set_dir(RADIO_IRQ, GPIO_IN);

    for (int round = 1;; round++)
    {
        printf("\n--- Round %d ---\n", round);

        // 1. Reset and watch BUSY: it should go high during boot, then low when
        // ready
        gpio_put(RADIO_RESET, 0);
        sleep_ms(10);
        gpio_put(RADIO_RESET, 1);
        sleep_us(100);
        const int busy_after_reset = gpio_get(RADIO_BUSY);
        const int boot_ms          = wait_busy_low(2000);
        printf("1. Reset: BUSY right after reset = %d, ", busy_after_reset);
        if (boot_ms < 0)
        {
            printf("BUSY stuck HIGH for 2 s\n");
        }
        else
        {
            printf("went low after %d ms\n", boot_ms);
        }

        // 2. Is each input wire actually connected to something?
        const bool b_busy_driven = is_driven(RADIO_BUSY);
        gpio_init(RADIO_MISO); // plain input for this test
        gpio_put(RADIO_CS, 0); // chip drives MISO only while CS is low
        sleep_us(50);
        const bool b_miso_driven = is_driven(RADIO_MISO);
        gpio_put(RADIO_CS, 1);
        printf("2. Wire test: BUSY %s, MISO %s\n",
               b_busy_driven ? "connected" : "FLOATING",
               b_miso_driven ? "connected" : "FLOATING");

        // 3. Raw GetVersion over SPI, fast and slow, with MISO pulled both ways
        printf("3. GetVersion (the 3rd byte should be 03):\n");
        spi_init(SPI_PORT, 1000 * 1000);
        gpio_set_function(RADIO_MISO, GPIO_FUNC_SPI);
        gpio_set_function(RADIO_CLK, GPIO_FUNC_SPI);
        gpio_set_function(RADIO_MOSI, GPIO_FUNC_SPI);
        bool b_chip_ok = false;
        b_chip_ok |= get_version_raw(1000 * 1000, true);
        b_chip_ok |= get_version_raw(1000 * 1000, false);
        b_chip_ok |= get_version_raw(100 * 1000, true);
        b_chip_ok |= get_version_raw(100 * 1000, false);

        // 4. Pico-only SPI check
        const bool b_loop_ok = loopback();
        spi_deinit(SPI_PORT);

        // Plain-English verdict, most important finding first
        printf("VERDICT: ");
        if (b_chip_ok)
        {
            printf(
                "ALL GOOD - the LR1121 answered. Flash lora_mesh.uf2 now.\n");
        }
        else if (b_loop_ok)
        {
            printf("Jumper test PASSED - the Pico's SPI pins work. So the "
                   "fault is in the wires/module:\n"
                   "         remove the jumper, put module MISO back on GP12, "
                   "then re-check CLK (GP10) and MOSI (GP11).\n");
        }
        else if ((boot_ms < 0) || (!busy_after_reset))
        {
            printf("The module is not booting. Check 3V3 -> pin 36, GND, RESET "
                   "-> GP5, BUSY -> GP14.\n");
        }
        else if (!b_busy_driven)
        {
            printf("BUSY wire (GP14) is not connected to the module.\n");
        }
        else if (!b_miso_driven)
        {
            printf(
                "MISO wire (GP12) is not connected to the module's MISO pin\n"
                "         (or the jumper test is set up: then the Pico's SPI "
                "pins FAILED the loopback).\n");
        }
        else
        {
            printf("Module boots but ignores commands. Next: unplug USB, move "
                   "module MISO off GP12,\n"
                   "         add a jumper GP11->GP12, plug in, and read this "
                   "verdict again.\n");
        }

        // Always flag a module that did not boot, even when another verdict was
        // printed first
        if ((!b_chip_ok) && ((boot_ms < 0) || (!busy_after_reset)))
        {
            printf("WARNING: the module did NOT boot this round (BUSY never "
                   "went high after reset).\n"
                   "         Check 3V3 -> pin 36, GND, RESET -> GP5 before "
                   "anything else.\n");
        }

        sleep_ms(3000);
    }
}

/*** end of file ***/

