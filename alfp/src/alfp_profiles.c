/*
 * alfp_profiles.c - per-radio timing, and radio selection.
 *
 * Every number here was checked in the mesh simulator (host/experiments, results in
 * docs/results/). Summary of why each value is what it is:
 *
 * LoRa (Mridula: SF7, 125 kHz, CR 4/5, 8-symbol preamble, CRC on)
 *   Airtime: Semtech AN1200.13 gives 399.9 ms for a 255-byte frame and ~41 ms for a POLL.
 *   gap 500 ms: a relay waits 10-100 ms and then transmits for ~400 ms. LoRa is half-duplex, so
 *     sending the next fragment before a 1-hop neighbour finishes relaying the last one means that
 *     neighbour cannot hear it. 500 ms clears one relay stage: at 2 hops it cuts transmissions per
 *     fragment from 4.0 (no gap) to 1.8. With the bridge 3+ hops away, relays two stages apart also
 *     collide (hidden terminals); 1000 ms then uses ~14% less airtime for about the same time, so
 *     build with -DALFP_LORA_GAP_MS=1000 for that layout (docs/results/e1_lora_gap.csv).
 *   Adaptive pacing (gap_max_ms > gap_ms) was implemented and tested; random link loss makes it
 *     slow down for no benefit (median 88 s vs 65 s at 2 hops, 10% loss), so it is off.
 *   poll timeout 1500 ms: a POLL/NACK round trip over 2 hops is ~0.6 s. Retrying sooner recovers
 *     from a lost POLL or NACK faster than the first design's 3 s did.
 *   max_polls 30: give up only after ~45 s of total silence. Counting a handful of unanswered polls
 *     gave up on links that were lossy but alive.
 *   tail_dup 4: when 4 or fewer fragments are missing, send each twice; saves about one round.
 *
 * WiFi (Goutham: ESP-NOW bridge firmware, 245-byte payload)
 *   ESP-NOW frame ~2 ms; the Pico-to-ESP8266 UART at 115200 baud adds ~22 ms for a full frame and
 *   tx_busy() covers that. Relays wait 5-30 ms, and ESP-NOW listens before talking, so a 10 ms gap
 *   is enough. Poll timeout 200 ms, give up after ~8 s of silence.
 */
#include <string.h>

#include "alfp.h"

#ifndef ALFP_LORA_GAP_MS
#define ALFP_LORA_GAP_MS 500u
#endif
#ifndef ALFP_WIFI_GAP_MS
#define ALFP_WIFI_GAP_MS 10u
#endif

static const alfp_profile_t k_profiles[] = {
    /* name   gap               gap_max           poll_to expiry  rounds stalled polls pcopies taildup id */
    { "lora", ALFP_LORA_GAP_MS, ALFP_LORA_GAP_MS, 1500u, 60000u, 40u, 10u, 30u, 1u, 4u, ALFP_RADIO_LORA },
    { "wifi", ALFP_WIFI_GAP_MS, ALFP_WIFI_GAP_MS, 200u, 15000u, 40u, 10u, 40u, 1u, 4u, ALFP_RADIO_WIFI },
};

/* Unknown radio: be conservative (LoRa-like timing). */
static const alfp_profile_t k_default
    = { "other", 500u, 500u, 1500u, 60000u, 40u, 10u, 30u, 1u, 4u, ALFP_RADIO_UNKNOWN };

const alfp_profile_t* alfp_profile_for(const alfp_ctx_t* p_ctx, const mesh_radio_t* p_radio)
{
    if (p_ctx && (p_ctx->cfg.p_profiles) && p_radio && (p_radio->p_name))
    {
        for (uint8_t idx = 0; idx < p_ctx->cfg.n_profiles; idx++)
        {
            if (0 == strcmp(p_radio->p_name, p_ctx->cfg.p_profiles[idx].p_radio_name))
            {
                return &p_ctx->cfg.p_profiles[idx];
            }
        }
    }
    return alfp_default_profile(p_radio);
}

const alfp_profile_t* alfp_default_profile(const mesh_radio_t* p_radio)
{
    if (p_radio && (p_radio->p_name))
    {
        for (unsigned idx = 0; idx < sizeof(k_profiles) / sizeof(k_profiles[0]); idx++)
        {
            if (0 == strcmp(p_radio->p_name, k_profiles[idx].p_radio_name))
            {
                return &k_profiles[idx];
            }
        }
    }
    return &k_default;
}

const mesh_radio_t* alfp_select_radio(const mesh_radio_t* const* pp_by_preference, uint8_t count, uint8_t bridge_id,
                                      uint32_t now_ms, uint32_t fresh_ms)
{
    for (uint8_t idx = 0; idx < count; idx++)
    {
        const mesh_radio_t* p_radio = pp_by_preference[idx];
        uint32_t total_ms = 0;
        if (p_radio && (p_radio->p_last_heard) && (p_radio->p_last_heard(bridge_id, &total_ms)))
        {
            if ((uint32_t)(now_ms - total_ms) <= fresh_ms)
            {
                return p_radio;
            }
        }
    }
    return NULL;
}

static bool radio_is_fresh(const mesh_radio_t* p_radio, uint8_t bridge_id, uint32_t now_ms, uint32_t fresh_ms)
{
    uint32_t total_ms = 0;
    return (NULL != p_radio) && (NULL != p_radio->p_last_heard) && (p_radio->p_last_heard(bridge_id, &total_ms)) && ((uint32_t)(now_ms - total_ms) <= fresh_ms);
}

static const mesh_radio_t* radio_named(const mesh_radio_t* const* pp_by_preference, uint8_t count, const char* p_name)
{
    for (uint8_t idx = 0; idx < count; idx++)
    {
        const mesh_radio_t* p_radio = pp_by_preference[idx];
        if ((NULL != p_radio) && (NULL != p_radio->p_name) && (0 == strcmp(p_radio->p_name, p_name)))
        {
            return p_radio;
        }
    }
    return NULL;
}

const mesh_radio_t* alfp_select_radio_for_size(const mesh_radio_t* const* pp_by_preference, uint8_t count, uint8_t bridge_id,
                                               uint32_t now_ms, uint32_t fresh_ms, uint32_t size_bytes,
                                               uint32_t size_threshold)
{
    const mesh_radio_t* p_lora = radio_named(pp_by_preference, count, "lora");
    const mesh_radio_t* p_wifi = radio_named(pp_by_preference, count, "wifi");
    const mesh_radio_t* p_preferred = (size_bytes < size_threshold) ? p_lora : p_wifi;
    const mesh_radio_t* p_fallback = (size_bytes < size_threshold) ? p_wifi : p_lora;
    if (radio_is_fresh(p_preferred, bridge_id, now_ms, fresh_ms))
    {
        return p_preferred;
    }
    if (radio_is_fresh(p_fallback, bridge_id, now_ms, fresh_ms))
    {
        return p_fallback;
    }
    return NULL;
}

bool alfp_radio_was_size_preferred(const mesh_radio_t* const* pp_by_preference, uint8_t count, uint32_t size_bytes,
                                   uint32_t size_threshold, const mesh_radio_t* p_chosen)
{
    const mesh_radio_t* p_preferred
        = (size_bytes < size_threshold) ? radio_named(pp_by_preference, count, "lora") : radio_named(pp_by_preference, count, "wifi");
    return (NULL != p_chosen) && (NULL != p_preferred) && (p_chosen == p_preferred);
}

const char* alfp_tx_result_str(alfp_tx_result_t result)
{
    switch (result)
    {
        case ALFP_TX_DONE:
            return "done";
        case ALFP_TX_GAVE_UP:
            return "gave_up";
        case ALFP_TX_REFUSED:
            return "refused";
        case ALFP_TX_IO_ERROR:
            return "io_error";
        default:
            /* not a valid alfp_tx_result_t: fall through to "?" below */
            break;
    }
    return "?";
}
