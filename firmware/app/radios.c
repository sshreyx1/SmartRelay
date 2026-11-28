/*
 * radios.c - see radios.h.
 */
#include "float_types.h"
#include "radios.h"

#include <stdio.h>

/* ---- offline stand-in, used until the real mesh sources are linked ---------------------- */

static bool off_send(uint8_t dst, const uint8_t* p_buf, uint16_t len)
{
    (void)dst;
    (void)p_buf;
    (void)len;
    return false;
}
static int off_recv(uint8_t* p_src, uint8_t* p_buf, uint16_t cap)
{
    (void)p_src;
    (void)p_buf;
    (void)cap;
    return MESH_RECV_NONE;
}
static void off_poll(void)
{
}
static uint16_t off_maxp_lora(void)
{
    return 250;
}
static __attribute__((unused)) uint16_t off_maxp_wifi(void)
{
    return 245;
}
static bool off_heard(uint8_t node, uint32_t* p_msec)
{
    (void)node;
    (void)p_msec;
    return false;
}
static bool off_busy(void)
{
    return false;
}
static void off_stats(mesh_stats_t* p_out)
{
    *p_out = (mesh_stats_t) { 0 };
}
static void off_drop(float32_t rate)
{
    (void)rate;
}

/* used when LoRa is not linked OR its init failed: a dead LR1121 must never be polled, because
 * the driver waits on its BUSY pin */
static const mesh_radio_t lora_offline
    = { "lora", off_send, off_recv, off_poll, off_maxp_lora, off_heard, off_busy, off_stats, off_drop };

#ifdef HAVE_LORA_MESH
#include "lora_mesh.h" /* Mridula: exports lora_mesh_radio, lora_mesh_init, lora_mesh_set_heartbeat */
#define lora_mesh g_lora_mesh_radio
#else
#define lora_mesh lora_offline
#endif

#ifdef HAVE_WIFI_MESH
#include "wifi_mesh.h" /* Goutham: exports wifi_mesh, wifi_mesh_init, wifi_mesh_set_topology_filter */
#include "wifi_mesh_config.h"
#include "wst_link.h"
#else
static const mesh_radio_t wifi_mesh
    = { "wifi", off_send, off_recv, off_poll, off_maxp_wifi, off_heard, off_busy, off_stats, off_drop };
#endif

#include "board.h"
#include "pico/stdlib.h"

static bool b_s_lora_ok;
static bool b_s_wifi_ok;
static bool b_s_wst_seen;
static int s_wst_owner = -1; /* radios_wst_owner() */
static uint8_t s_wst_mac[6];

#ifdef HAVE_WIFI_MESH
/* The ESP-01 (WST) in this Pico's socket must be the one wifi_mesh_config.h lists for this node,
 * because Goutham's topology filter works out who sent a packet from the WST's MAC. */
static void check_wst(uint8_t node_id)
{
    uint8_t mac[6];
    uint32_t start_t = to_ms_since_boot(get_absolute_time());
    while (!wst_link_get_mac(mac))
    {
        g_wifi_mesh.p_poll();
        if (to_ms_since_boot(get_absolute_time()) - start_t > 2000)
        {
            printf("wifi: the ESP-01 did not answer (seated in the ESP-01 socket? GP16/GP17). LoRa will be used.\n");
            return;
        }
        sleep_ms(5);
    }
    b_s_wst_seen = true;
    int owner = 0;
    for (int node = 1; node <= 5; node++)
    {
        bool b_same = true;
        for (int idx = 0; idx < 6; idx++)
        {
            b_same = b_same && (wifi_mesh_macs[node][idx] == mac[idx]);
        }
        if (b_same)
        {
            owner = node;
        }
    }
    s_wst_owner = owner;
    for (int idx = 0; idx < 6; idx++)
    {
        s_wst_mac[idx] = mac[idx];
    }
    printf("wifi: ESP-01 MAC %02X:%02X:%02X:%02X:%02X:%02X = WST-%d\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           owner);
    if (0 == owner)
    {
        printf("wifi: WARNING this ESP-01 is not in wifi_mesh_config.h, other nodes' topology filter will drop it\n");
    }
    else if (owner != node_id)
    {
        printf("wifi: WARNING this firmware is node %u but the ESP-01 is WST-%d. Flash csc09_node_%d.uf2 "
               "or swap the ESP-01, otherwise WiFi routing is wrong.\n",
               node_id, owner, owner);
    }
    else
    {
        /* the ESP-01 belongs to this node: nothing to report */
    }
}
#endif

bool radios_init(uint8_t node_id)
{
#ifdef HAVE_LORA_MESH
    b_s_lora_ok = lora_mesh_init(node_id);
    board_spi1_after_lora(); /* her driver set SPI1 to 1 MHz; put it back to the shared speed */
    if (!b_s_lora_ok)
    {
        printf("lora: init FAILED (check the Core1121 wiring and power, BUSY GP%d, DIO9 GP%d). "
               "Continuing with WiFi only.\n",
               RADIO_BUSY, RADIO_IRQ);
    }
    else if (RADIOS_BRIDGE_ID == node_id)
    {
        /* The bridge floods an empty broadcast every 3 s. Field nodes see it through
         * last_heard(), which is how alfp_select_radio() knows LoRa can reach the bridge. */
        lora_mesh_set_heartbeat(3000);
    }
    else
    {
        /* field node: LoRa is up, no heartbeat to send */
    }
#else
    printf("radios: LoRa mesh not linked (offline stub)\n");
#endif
#ifdef HAVE_WIFI_MESH
    /* node 1 sends its own 3 s WiFi heartbeat inside wifi_mesh.c */
    b_s_wifi_ok = wifi_mesh_init(node_id);
    if (b_s_wifi_ok)
    {
        check_wst(node_id);
    }
    else
    {
        printf("wifi: init FAILED (node id %u out of range?)\n", node_id);
    }
#else
    printf("radios: WiFi mesh not linked (offline stub)\n");
#endif
    (void)node_id;
    return b_s_lora_ok || b_s_wifi_ok;
}

bool radios_lora_ok(void)
{
    return b_s_lora_ok;
}

bool radios_wifi_ok(void)
{
    /* the mesh keeps running either way, so a late ESP-01 still works; this is for the status line */
    return b_s_wifi_ok && b_s_wst_seen;
}

void radios_set_wifi_topology_filter(bool b_enable)
{
#ifdef HAVE_WIFI_MESH
    wifi_mesh_set_topology_filter(b_enable);
#else
    (void)on;
#endif
}

const mesh_radio_t* radio_lora(void)
{
    return b_s_lora_ok ? &lora_mesh : &lora_offline;
}

const mesh_radio_t* radio_wifi(void)
{
    return &g_wifi_mesh;
}

bool radios_last_recv_info(const mesh_radio_t* p_radio, uint8_t* p_hops, uint8_t* p_prev)
{
    *p_hops = 0;
    *p_prev = 0;
#ifdef HAVE_LORA_MESH
    if (b_s_lora_ok && (p_radio == &lora_mesh))
    {
        return lora_mesh_last_recv_info(p_hops, p_prev);
    }
#endif
#ifdef HAVE_WIFI_MESH
    if (p_radio == &g_wifi_mesh)
    {
        return wifi_mesh_last_recv_info(p_hops, p_prev);
    }
#endif
    (void)p_radio;
    return false;
}

uint16_t radios_relayed_from(const mesh_radio_t* p_radio, uint8_t src)
{
#ifdef HAVE_LORA_MESH
    if (b_s_lora_ok && (p_radio == &lora_mesh))
    {
        return lora_mesh_relayed_from(src);
    }
#endif
#ifdef HAVE_WIFI_MESH
    if (p_radio == &g_wifi_mesh)
    {
        return wifi_mesh_relayed_from(src);
    }
#endif
    (void)p_radio;
    (void)src;
    return 0;
}

int radios_wst_owner(uint8_t mac[6])
{
    if ((NULL != mac) && (s_wst_owner >= 0))
    {
        for (int idx = 0; idx < 6; idx++)
        {
            mac[idx] = s_wst_mac[idx];
        }
    }
    return s_wst_owner;
}
