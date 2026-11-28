/*
 * lora_radio.h - Thin, non-blocking wrapper around the Semtech LR1121 driver
 * (Waveshare Core1121-HF). Sends and receives single raw LoRa packets; knows
 * nothing about the mesh.
 */
#ifndef LORA_RADIO_H
#define LORA_RADIO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Modem settings - fixed constants, shared with the fragmentation layer because
 * they set time on air. Every node must use identical values.
 */
/* AS923, Singapore (920-925 MHz). Trying 924.2 to avoid other groups; was
 * 923000000 */
#define LORA_FREQ_HZ 924200000UL
/* -9..22; lower it to force multi-hop on a desk */
#define LORA_TX_POWER_DBM   14
#define LORA_SF             LR11XX_RADIO_LORA_SF7
#define LORA_BW             LR11XX_RADIO_LORA_BW_125
#define LORA_CR             LR11XX_RADIO_LORA_CR_4_5
#define LORA_PREAMBLE_SYMB  8
#define LORA_SYNC_WORD      0x12 // private network
#define LORA_MAX_PACKET_LEN 255

typedef enum
{
    LORA_EVT_NONE = 0,
    LORA_EVT_RX_DONE,  // valid packet copied out (CRC passed)
    LORA_EVT_RX_ERROR, // CRC or header error, packet discarded
    LORA_EVT_TX_DONE,
    LORA_EVT_TX_FAIL, // TX timed out or could not be started
} lora_radio_event_t;

// Reset and configure the LR1121, then enter continuous RX. Returns false if
// the chip does not answer.
bool lora_radio_init(void);

// Start transmitting one packet (1..255 bytes). Returns false if busy or the
// radio rejected it.
bool lora_radio_start_tx(const uint8_t* p_buf, uint8_t len);

// True between lora_radio_start_tx() and TX_DONE/TX_FAIL.
bool lora_radio_tx_active(void);

// Check the radio IRQ and handle at most one event. For LORA_EVT_RX_DONE, buf
// (>= 255 bytes)
// receives the packet and *len/*rssi_dbm/*snr_db are filled. Returns
// LORA_EVT_NONE quickly when idle.
lora_radio_event_t lora_radio_service(uint8_t* p_buf, uint8_t* p_len,
                                      int8_t* p_rssi_dbm, int8_t* p_snr_db);

// Time on air for a packet of len bytes with the settings above.
uint32_t lora_radio_time_on_air_ms(uint8_t len);

#endif // LORA_RADIO_H

/*** end of file ***/

