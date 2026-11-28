/*
 * lora_radio.c - Non-blocking LR1121 packet radio (Waveshare Core1121-HF).
 *
 * Setup follows the Waveshare demo (examples/lr1121_config.c) that was verified
 * on this hardware, with three changes: 923 MHz + matching image calibration,
 * LoRa hardware CRC on, and the IRQ line polled instead of blocking calls.
 */
#include <inttypes.h>
#include <stdio.h>
#include "wavesahre_lora_1121.h"
#include "lora_radio.h"

#define RX_CONTINUOUS 0xFFFFFF
/* read IRQ status over SPI even if the DIO9 pin looks idle */
#define IRQ_FALLBACK_CHECK_MS 10
/* give up on a TX this long after its expected end */
#define TX_WATCHDOG_MARGIN_MS 1000

#define LORA_IRQ_MASK                                                          \
    (LR11XX_SYSTEM_IRQ_TX_DONE | LR11XX_SYSTEM_IRQ_RX_DONE |                   \
     LR11XX_SYSTEM_IRQ_TIMEOUT | LR11XX_SYSTEM_IRQ_HEADER_ERROR |              \
     LR11XX_SYSTEM_IRQ_CRC_ERROR)

static lr1121_t radio;

static lr11xx_radio_mod_params_lora_t mod_params = {
    .sf   = LORA_SF,
    .bw   = LORA_BW,
    .cr   = LORA_CR,
    .ldro = 0, // computed in init
};

static lr11xx_radio_pkt_params_lora_t pkt_params = {
    .preamble_len_in_symb = LORA_PREAMBLE_SYMB,
    .header_type          = LR11XX_RADIO_LORA_PKT_EXPLICIT,
    .pld_len_in_bytes     = LORA_MAX_PACKET_LEN,
    /* contract rule 4: corrupt packets are dropped */
    .crc = LR11XX_RADIO_LORA_CRC_ON,
    .iq  = LR11XX_RADIO_LORA_IQ_STANDARD,
};

static bool     b_tx_active;
static uint32_t tx_deadline_ms;
static uint32_t last_irq_check_ms;

static uint32_t now_ms (void)
{
    return to_ms_since_boot(get_absolute_time());
}

static bool start_rx (void)
{
    pkt_params.pld_len_in_bytes = LORA_MAX_PACKET_LEN;
    if (LR11XX_STATUS_OK !=
        lr11xx_radio_set_lora_pkt_params(&radio, &pkt_params))
    {
        return false;
    }
    if (LR11XX_STATUS_OK !=
        lr11xx_radio_set_rx_with_timeout_in_rtc_step(&radio, RX_CONTINUOUS))
    {
        return false;
    }
    return true;
}

static bool system_init (void)
{
    lr11xx_system_reset(&radio);
    lr11xx_hal_wakeup(&radio);

    if (LR11XX_STATUS_OK != lr11xx_system_enable_spi_crc(&radio, false))
    {
        return false;
    }
    if (LR11XX_STATUS_OK !=
        lr11xx_system_set_standby(&radio, LR11XX_SYSTEM_STANDBY_CFG_XOSC))
    {
        return false;
    }
    /* 902-928 MHz */
    if (LR11XX_STATUS_OK != lr11xx_system_calibrate_image(&radio, 0xE1, 0xE9))
    {
        return false;
    }
    if (LR11XX_STATUS_OK !=
        lr11xx_system_set_reg_mode(&radio,
                                   smtc_shield_lr11xx_common_get_reg_mode()))
    {
        return false;
    }
    if (LR11XX_STATUS_OK !=
        lr11xx_system_set_dio_as_rf_switch(
            &radio, smtc_shield_lr11xx_common_get_rf_switch_cfg()))
    {
        return false;
    }
    if (LR11XX_STATUS_OK !=
        lr11xx_system_set_tcxo_mode(&radio, LR11XX_SYSTEM_TCXO_CTRL_3_0V, 300))
    {
        return false;
    }
    if (LR11XX_STATUS_OK !=
        lr11xx_system_cfg_lfclk(&radio, LR11XX_SYSTEM_LFCLK_XTAL, true))
    {
        return false;
    }
    if (LR11XX_STATUS_OK != lr11xx_system_clear_errors(&radio))
    {
        return false;
    }
    if (LR11XX_STATUS_OK != lr11xx_system_calibrate(&radio, 0x3F))
    {
        return false;
    }

    uint16_t errors = 0;
    if (LR11XX_STATUS_OK != lr11xx_system_get_errors(&radio, &errors))
    {
        return false;
    }
    if (errors & LR11XX_SYSTEM_ERRORS_IMG_CALIB_MASK)
    {
        printf("lora_radio: image calibration error\n");
    }
    if (LR11XX_STATUS_OK != lr11xx_system_clear_errors(&radio))
    {
        return false;
    }
    if (LR11XX_STATUS_OK !=
        lr11xx_system_clear_irq_status(&radio, LR11XX_SYSTEM_IRQ_ALL_MASK))
    {
        return false;
    }

    lr11xx_system_version_t ver;
    if (LR11XX_STATUS_OK != lr11xx_system_get_version(&radio, &ver))
    {
        return false;
    }
    if (LR11XX_SYSTEM_VERSION_TYPE_LR1121 != ver.type)
    {
        printf("lora_radio: unexpected chip type 0x%02X (SPI wiring?)\n",
               ver.type);
        return false;
    }
    printf("lora_radio: LR1121 firmware 0x%04X, hardware 0x%02X\n", ver.fw,
           ver.hw);
    return true;
}

static bool modem_init (void)
{
    const smtc_shield_lr11xx_pa_pwr_cfg_t* p_path_a =
        smtc_shield_lr1121mb1gis_get_pa_pwr_cfg(LORA_FREQ_HZ,
                                                LORA_TX_POWER_DBM);
    if (NULL == p_path_a)
    {
        printf("lora_radio: invalid frequency/power combination\n");
        return false;
    }

    if (LR11XX_STATUS_OK !=
        lr11xx_radio_set_pkt_type(&radio, LR11XX_RADIO_PKT_TYPE_LORA))
    {
        return false;
    }
    lr11xx_radio_pkt_type_t pkt_type;
    if (LR11XX_STATUS_OK != lr11xx_radio_get_pkt_type(&radio, &pkt_type))
    {
        return false;
    }
    if (LR11XX_RADIO_PKT_TYPE_LORA != pkt_type)
    {
        printf("lora_radio: packet type read-back failed (SPI wiring?)\n");
        return false;
    }

    if (LR11XX_STATUS_OK != lr11xx_radio_set_rf_freq(&radio, LORA_FREQ_HZ))
    {
        return false;
    }
    if (LR11XX_STATUS_OK !=
        lr11xx_radio_set_rssi_calibration(
            &radio,
            smtc_shield_lr11xx_get_rssi_calibration_table(LORA_FREQ_HZ)))
    {
        return false;
    }
    if (LR11XX_STATUS_OK !=
        lr11xx_radio_set_pa_cfg(&radio, &p_path_a->pa_config))
    {
        return false;
    }
    if (LR11XX_STATUS_OK != lr11xx_radio_set_tx_params(&radio, p_path_a->power,
                                                       LR11XX_RADIO_RAMP_48_US))
    {
        return false;
    }
    // Stay on the TCXO between TX and RX for fast turnaround (power is from USB
    // anyway)
    if (LR11XX_STATUS_OK != lr11xx_radio_set_rx_tx_fallback_mode(
                                &radio, LR11XX_RADIO_FALLBACK_STDBY_XOSC))
    {
        return false;
    }
    if (LR11XX_STATUS_OK != lr11xx_radio_cfg_rx_boosted(&radio, false))
    {
        return false;
    }

    mod_params.ldro =
        smtc_shield_lr11xx_common_compute_lora_ldro(LORA_SF, LORA_BW);
    if (LR11XX_STATUS_OK !=
        lr11xx_radio_set_lora_mod_params(&radio, &mod_params))
    {
        return false;
    }
    if (LR11XX_STATUS_OK !=
        lr11xx_radio_set_lora_pkt_params(&radio, &pkt_params))
    {
        return false;
    }
    if (LR11XX_STATUS_OK !=
        lr11xx_radio_set_lora_sync_word(&radio, LORA_SYNC_WORD))
    {
        return false;
    }

    // Route our IRQs to DIO9 (the RADIO_IRQ pin)
    if (LR11XX_STATUS_OK !=
        lr11xx_system_set_dio_irq_params(&radio, LORA_IRQ_MASK, 0))
    {
        return false;
    }
    if (LR11XX_STATUS_OK !=
        lr11xx_system_clear_irq_status(&radio, LR11XX_SYSTEM_IRQ_ALL_MASK))
    {
        return false;
    }
    return true;
}

bool lora_radio_init (void)
{
    lora_init_io_context(&radio);
    lora_init_io(&radio);
    lora_spi_init(&radio);

    if ((!system_init()) || (!modem_init()))
    {
        return false;
    }

    b_tx_active       = false;
    last_irq_check_ms = now_ms();
    printf("lora_radio: %" PRIu32 " Hz, SF%d BW125 CR4/5, %d dBm, CRC on\n",
           (uint32_t)LORA_FREQ_HZ, (int)LORA_SF, LORA_TX_POWER_DBM);
    return start_rx();
}

bool lora_radio_start_tx (const uint8_t* p_buf, uint8_t len)
{
    if (b_tx_active || (0 == len))
    {
        return false;
    }

    // Leave RX, load the packet, then transmit. On any failure go back to
    // listening.
    const uint32_t toa_ms       = lora_radio_time_on_air_ms(len);
    pkt_params.pld_len_in_bytes = len;
    if ((LR11XX_STATUS_OK !=
         lr11xx_system_set_standby(&radio, LR11XX_SYSTEM_STANDBY_CFG_XOSC)) ||
        (LR11XX_STATUS_OK !=
         lr11xx_radio_set_lora_pkt_params(&radio, &pkt_params)) ||
        (LR11XX_STATUS_OK != lr11xx_regmem_write_buffer8(&radio, p_buf, len)) ||
        (LR11XX_STATUS_OK !=
         lr11xx_radio_set_tx(&radio, toa_ms + TX_WATCHDOG_MARGIN_MS)))
    {
        start_rx();
        return false;
    }

    b_tx_active    = true;
    tx_deadline_ms = now_ms() + toa_ms + TX_WATCHDOG_MARGIN_MS + 100;
    return true;
}

bool lora_radio_tx_active (void)
{
    return b_tx_active;
}

lora_radio_event_t lora_radio_service (uint8_t* p_buf, uint8_t* p_len,
                                       int8_t* p_rssi_dbm, int8_t* p_snr_db)
{
    const uint32_t now = now_ms();

    // Software watchdog in case the TX-done IRQ never arrives
    if (b_tx_active && ((int32_t)(now - tx_deadline_ms) > 0))
    {
        b_tx_active = false;
        start_rx();
        return LORA_EVT_TX_FAIL;
    }

    // Cheap path: DIO9 low and a recent SPI check means nothing to do
    if ((!gpio_get(radio.irq)) &&
        ((now - last_irq_check_ms) < IRQ_FALLBACK_CHECK_MS))
    {
        return LORA_EVT_NONE;
    }
    last_irq_check_ms = now;

    lr11xx_system_irq_mask_t irq = 0;
    if (LR11XX_STATUS_OK !=
        lr11xx_system_get_and_clear_irq_status(&radio, &irq))
    {
        return LORA_EVT_NONE;
    }
    irq &= LORA_IRQ_MASK;
    if (0 == irq)
    {
        return LORA_EVT_NONE;
    }

    if (irq & LR11XX_SYSTEM_IRQ_TX_DONE)
    {
        b_tx_active = false;
        start_rx();
        return LORA_EVT_TX_DONE;
    }

    if (irq & LR11XX_SYSTEM_IRQ_TIMEOUT)
    {
        start_rx();
        if (b_tx_active)
        {
            b_tx_active = false;
            return LORA_EVT_TX_FAIL;
        }
        return LORA_EVT_NONE;
    }

    if (irq & (LR11XX_SYSTEM_IRQ_CRC_ERROR | LR11XX_SYSTEM_IRQ_HEADER_ERROR))
    {
        return LORA_EVT_RX_ERROR; // continuous RX keeps listening
    }

    if (irq & LR11XX_SYSTEM_IRQ_RX_DONE)
    {
        lr11xx_radio_rx_buffer_status_t status;
        lr11xx_radio_pkt_status_lora_t  pkt_status;
        if ((LR11XX_STATUS_OK !=
             lr11xx_radio_get_rx_buffer_status(&radio, &status)) ||
            (0 == status.pld_len_in_bytes) ||
            (LR11XX_STATUS_OK != lr11xx_regmem_read_buffer8(
                                     &radio, p_buf, status.buffer_start_pointer,
                                     status.pld_len_in_bytes)))
        {
            return LORA_EVT_RX_ERROR;
        }
        *p_len = status.pld_len_in_bytes;
        if (LR11XX_STATUS_OK ==
            lr11xx_radio_get_lora_pkt_status(&radio, &pkt_status))
        {
            *p_rssi_dbm = pkt_status.rssi_pkt_in_dbm;
            *p_snr_db   = pkt_status.snr_pkt_in_db;
        }
        else
        {
            *p_rssi_dbm = 0;
            *p_snr_db   = 0;
        }
        return LORA_EVT_RX_DONE;
    }

    return LORA_EVT_NONE;
}

uint32_t lora_radio_time_on_air_ms (uint8_t len)
{
    lr11xx_radio_pkt_params_lora_t pkt_cfg = pkt_params;
    pkt_cfg.pld_len_in_bytes               = len;
    return lr11xx_radio_get_lora_time_on_air_in_ms(&pkt_cfg, &mod_params);
}

/*** end of file ***/

