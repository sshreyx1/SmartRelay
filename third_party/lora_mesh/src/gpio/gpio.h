/*****************************************************************************
 * | File         :   gpio.h
 * | Author       :   Waveshare team
 * | Function     :   Hardware underlying interface
 * | Info         :
 * |                 GPIO driver code for hardware-level operations.
 * ----------------
 * | This version :   V1.0
 * | Date         :   2024-11-19
 * | Info         :   Basic version
 *
 ******************************************************************************/

#ifndef __GPIO_H
#define __GPIO_H
#include "pico/stdlib.h"
#include "hardware/gpio.h"

/* Pin Definitions.
 * CSC09 integration (Shreya): wrapped in #ifndef so the combined firmware can move BUSY and IRQ
 * from CMake (-DRADIO_BUSY=6 -DRADIO_IRQ=7) without changing anything else. Defaults unchanged. */
#ifndef RADIO_RESET
#define RADIO_RESET 5
#endif
#ifndef RADIO_MOSI
#define RADIO_MOSI 11
#endif
#ifndef RADIO_MISO
#define RADIO_MISO 12
#endif
#ifndef RADIO_CLK
#define RADIO_CLK 10
#endif
#ifndef RADIO_CS
#define RADIO_CS 13
#endif
#ifndef RADIO_BUSY
#define RADIO_BUSY 14
#endif
#ifndef RADIO_IRQ
#define RADIO_IRQ 15
#endif
#ifndef RADIO_LED
#define RADIO_LED 25
#endif

/* Function Prototypes */

/**
 * @brief Configure a GPIO pin as input or output
 *
 * This function initializes a GPIO pin with the specified mode (input or output).
 * If set as input, it also enables the pull-up resistor by default.
 *
 * @param Pin GPIO pin number
 * @param Mode GPIO mode: 0 or GPIO_MODE_INPUT for input, others for output
 */
void DEV_GPIO_Mode(uint16_t Pin, uint16_t Mode);

/**
 * @brief Configure a GPIO pin for interrupt handling
 *
 * This function sets up a GPIO pin to generate an interrupt on a negative edge
 * (falling edge) and registers the specified interrupt handler.
 *
 * @param Pin GPIO pin number
 * @param isr_handler Pointer to the interrupt handler function
 */
void DEV_GPIO_INT(int32_t Pin, gpio_irq_callback_t isr_handler);

/**
 * @brief Set the logic level of a GPIO pin
 *
 * This function sets the logic level (high or low) of a GPIO pin.
 *
 * @param Pin GPIO pin number
 * @param Value Logic level: 0 for low, 1 for high
 */
void DEV_Digital_Write(uint16_t Pin, uint8_t Value);

/**
 * @brief Read the logic level of a GPIO pin
 *
 * This function reads and returns the current logic level of a GPIO pin.
 *
 * @param Pin GPIO pin number
 * @return uint8_t Logic level: 0 for low, 1 for high
 */
uint8_t DEV_Digital_Read(uint16_t Pin);

#endif
