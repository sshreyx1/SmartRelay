/*
 * radios.c - see radios.h.
 */
#include "radios.h"

#include <stdio.h>

/* ---- offline stand-in, used until the real mesh sources are linked ---------------------- */

static bool off_send(uint8_t d, const uint8_t* p, uint16_t l)
{
    (void)d;
    (void)p;
    (void)l;
    return false;
}
static int off_recv(uint8_t* s, uint8_t* b, uint16_t c)
{
    (void)s;
    (void)b;
    (void)c;
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
static bool off_heard(uint8_t n, uint32_t* ms)
{
    (void)n;
    (void)ms;
    return false;
}
static bool off_busy(void)
{
    return false;
}
static void off_stats(mesh_stats_t* o)
{
    *o = (mesh_stats_t) { 0 };
}
static void off_drop(float r)
{
    (void)r;
}

/* used when LoRa is not linked OR its init failed: a dead LR1121 must never be polled, because
 * the driver waits on its BUSY pin */
static const mesh_radio_t lora_offline
    = { "lora", off_send, off_recv, off_poll, off_maxp_lora, off_heard, off_busy, off_stats, off_drop };

#ifdef HAVE_LORA_MESH
#include "lora_mesh.h" /* Mridula: exports lora_mesh_radio, lora_mesh_init, lora_mesh_set_heartbeat */
#define lora_mesh lora_mesh_radio
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

static bool s_lora_ok, s_wifi_ok, s_wst_seen;
static int s_wst_owner = -1; /* radios_wst_owner() */
static uint8_t s_wst_mac[6];

#ifdef HAVE_WIFI_MESH
/* The ESP-01 (WST) in this Pico's socket must be the one wifi_mesh_config.h lists for this node,
 * because Goutham's topology filter works out who sent a packet from the WST's MAC. */
static void check_wst(uint8_t node_id)
{
    uint8_t mac[6];
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while (!wst_link_get_mac(mac))
    {
        wifi_mesh.poll();
        if (to_ms_since_boot(get_absolute_time()) - t0 > 2000)
        {
            printf("wifi: the ESP-01 did not answer (seated in the ESP-01 socket? GP16/GP17). LoRa will be used.\n");
            return;
        }
        sleep_ms(5);
    }
    s_wst_seen = true;
    int owner = 0;
    for (int n = 1; n <= 5; n++)
    {
        bool same = true;
        for (int i = 0; i < 6; i++)
        {
            same = same && WIFI_MESH_MACS[n][i] == mac[i];
        }
        if (same)
        {
            owner = n;
        }
    }
    s_wst_owner = owner;
    for (int i = 0; i < 6; i++)
    {
        s_wst_mac[i] = mac[i];
    }
    printf("wifi: ESP-01 MAC %02X:%02X:%02X:%02X:%02X:%02X = WST-%d\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           owner);
    if (owner == 0)
    {
        printf("wifi: WARNING this ESP-01 is not in wifi_mesh_config.h, other nodes' topology filter will drop it\n");
    }
    else if (owner != node_id)
    {
        printf("wifi: WARNING this firmware is node %u but the ESP-01 is WST-%d. Flash csc09_node_%d.uf2 "
               "or swap the ESP-01, otherwise WiFi routing is wrong.\n",
               node_id, owner, owner);
    }
}
#endif

bool radios_init(uint8_t node_id)
{
#ifdef HAVE_LORA_MESH
    s_lora_ok = lora_mesh_init(node_id);
    board_spi1_after_lora(); /* her driver set SPI1 to 1 MHz; put it back to the shared speed */
    if (!s_lora_ok)
    {
        printf("lora: init FAILED (check the Core1121 wiring and power, BUSY GP%d, DIO9 GP%d). "
               "Continuing with WiFi only.\n",
               RADIO_BUSY, RADIO_IRQ);
    }
    else if (node_id == RADIOS_BRIDGE_ID)
    {
        /* The bridge floods an empty broadcast every 3 s. Field nodes see it through
         * last_heard(), which is how alfp_select_radio() knows LoRa can reach the bridge. */
        lora_mesh_set_heartbeat(3000);
    }
#else
    printf("radios: LoRa mesh not linked (offline stub)\n");
#endif
#ifdef HAVE_WIFI_MESH
    /* node 1 sends its own 3 s WiFi heartbeat inside wifi_mesh.c */
    s_wifi_ok = wifi_mesh_init(node_id);
    if (s_wifi_ok)
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
    return s_lora_ok || s_wifi_ok;
}

bool radios_lora_ok(void)
{
    return s_lora_ok;
}

bool radios_wifi_ok(void)
{
    /* the mesh keeps running either way, so a late ESP-01 still works; this is for the status line */
    return s_wifi_ok && s_wst_seen;
}

void radios_set_wifi_topology_filter(bool on)
{
#ifdef HAVE_WIFI_MESH
    wifi_mesh_set_topology_filter(on);
#else
    (void)on;
#endif
}

const mesh_radio_t* radio_lora(void)
{
    return s_lora_ok ? &lora_mesh : &lora_offline;
}

const mesh_radio_t* radio_wifi(void)
{
    return &wifi_mesh;
}

bool radios_last_recv_info(const mesh_radio_t* r, uint8_t* hops, uint8_t* prev)
{
    *hops = 0;
    *prev = 0;
#ifdef HAVE_LORA_MESH
    if (s_lora_ok && r == &lora_mesh)
    {
        return lora_mesh_last_recv_info(hops, prev);
    }
#endif
#ifdef HAVE_WIFI_MESH
    if (r == &wifi_mesh)
    {
        return wifi_mesh_last_recv_info(hops, prev);
    }
#endif
    (void)r;
    return false;
}

uint16_t radios_relayed_from(const mesh_radio_t* r, uint8_t src)
{
#ifdef HAVE_LORA_MESH
    if (s_lora_ok && r == &lora_mesh)
    {
        return lora_mesh_relayed_from(src);
    }
#endif
#ifdef HAVE_WIFI_MESH
    if (r == &wifi_mesh)
    {
        return wifi_mesh_relayed_from(src);
    }
#endif
    (void)r;
    (void)src;
    return 0;
}

int radios_wst_owner(uint8_t mac[6])
{
    if (mac != NULL && s_wst_owner >= 0)
    {
        for (int i = 0; i < 6; i++)
        {
            mac[i] = s_wst_mac[i];
        }
    }
    return s_wst_owner;
}
