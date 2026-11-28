/*****************************************************************************
 * | File      	 :   gpio.c
 * | Author      :   Waveshare team
 * | Function    :   Hardware underlying interface
 * | Info        :
 *                   GPIO driver code
 *----------------
 * |This version :   V1.0
 * | Date        :   2024-11-19
 * | Info        :   Basic version
 *
 ******************************************************************************/
#include "gpio.h"

/**
 * @brief Configure a GPIO pin as input or output
 *
 * This function initializes a GPIO pin with the specified mode (input or output).
 * If set as input, it also enables the pull-up resistor by default.
 *
 * @param Pin GPIO pin number
 * @param Mode GPIO mode: 0 or GPIO_MODE_INPUT for input, others for output
 */
void DEV_GPIO_Mode(uint16_t pin, uint16_t mode)
{
    gpio_init(pin);
    if(mode == 0 || mode == GPIO_IN) {
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_up(pin);
    } else {
        gpio_set_dir(pin, GPIO_OUT);
    }

}

/**
 * @brief Configure a GPIO pin for interrupt handling
 *
 * This function sets up a GPIO pin to generate an interrupt on a negative edge
 * (falling edge) and registers the specified interrupt handler.
 *
 * @param Pin GPIO pin number
 * @param isr_handler Pointer to the interrupt handler function
 */
void DEV_GPIO_INT(int32_t pin, gpio_irq_callback_t p_isr_handler)
{
    gpio_set_irq_enabled_with_callback(pin,GPIO_IRQ_EDGE_RISE,true,p_isr_handler);
}

/**
 * @brief Set the logic level of a GPIO pin
 *
 * This function sets the logic level (high or low) of a GPIO pin.
 *
 * @param Pin GPIO pin number
 * @param Value Logic level: 0 for low, 1 for high
 */
void DEV_Digital_Write(uint16_t pin, uint8_t value)
{
    gpio_put(pin, value); // Set the GPIO pin level
}

/**
 * @brief Read the logic level of a GPIO pin
 *
 * This function reads and returns the current logic level of a GPIO pin.
 *
 * @param Pin GPIO pin number
 * @return uint8_t Logic level: 0 for low, 1 for high
 */
uint8_t DEV_Digital_Read(uint16_t pin)
{
    return gpio_get(pin); // Get the GPIO pin level
}
