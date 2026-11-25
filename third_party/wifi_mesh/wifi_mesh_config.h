/*
 * wifi_mesh_config.h - settings for the WiFi flooding mesh.
 * Edit this file when WSTs are swapped or the demo layout changes.
 */
#ifndef WIFI_MESH_CONFIG_H
#define WIFI_MESH_CONFIG_H

#include <stdint.h>

/* ---------- Which WST belongs to which node ----------
 * Each node works out its own ID by looking up its WST's MAC here,
 * so the same firmware runs on every node.
 * Index = node ID (1-5 = A-E). Index 0 is unused.                        */
/* DEVIATION (BARR-C 4.2.c.ii, storage allocated in a header file): this table
 * and the neighbour table below are small static const lookup tables (42 bytes)
 * that each includer gets its own copy of. They are kept here so the demo layout
 * is edited in one place; moving them to a .c would change the FreeRTOS, SMP and
 * host build file lists.
 * Approved by: ____________________ (project manager), date: __________ */
static const uint8_t WIFI_MESH_MACS[6][6] = {
    {0, 0, 0, 0, 0, 0},                       /* 0: unused          */
    {0xAC, 0x0B, 0xFB, 0xC7, 0x7F, 0xE9},     /* 1: A  <- WST-1     */
    {0xAC, 0x0B, 0xFB, 0xC8, 0x19, 0x96},     /* 2: B  <- WST-2     */
    {0xAC, 0x0B, 0xFB, 0xC7, 0xF8, 0x91},     /* 3: C  <- WST-3     */
    {0xAC, 0x0B, 0xFB, 0xC7, 0xE2, 0xFD},     /* 4: D  <- WST-4     */
    {0xAC, 0x0B, 0xFB, 0xC8, 0x19, 0xC1},     /* 5: E  <- WST-5     */
};

/* ---------- Demo layout (who can "hear" whom) ----------
 * In a lab every node hears every other node, so there would never be
 * more than one hop. When the topology filter is ON, a node ignores
 * packets from WSTs that are not its neighbour, recreating the trail
 * layout from the proposal diagram:
 *
 *        B ----- C
 *        | \   / | \
 *        |  \ /  |  E
 *        |  / \  | /
 *        A ----- D
 *
 * Bit n set = node n is a neighbour.                                     */
#define NB(n) (1u << (n))
static const uint8_t WIFI_MESH_NEIGHBOURS[6] = {
    0,
    NB(2) | NB(3) | NB(4),            /* A: B, C, D    */
    NB(1) | NB(3) | NB(4),            /* B: A, C, D    */
    NB(1) | NB(2) | NB(4) | NB(5),    /* C: A, B, D, E */
    NB(1) | NB(2) | NB(3) | NB(5),    /* D: A, B, C, E */
    NB(3) | NB(4),                    /* E: C, D       */
};
#undef NB

/* ---------- Agreed numbers (Phase 2) ---------- */
#define WIFI_MESH_BRIDGE_NODE      1       /* node A is plugged into the laptop */
#define WIFI_MESH_HOP_LIMIT        4       /* relays allowed per packet         */
#define WIFI_MESH_DUP_WINDOW_MS    30000   /* remember packet IDs for 30 s      */
#define WIFI_MESH_HEARTBEAT_MS     3000    /* bridge heartbeat period           */
#define WIFI_MESH_RELAY_DELAY_MIN  5       /* random wait before relaying (ms)  */
#define WIFI_MESH_RELAY_DELAY_MAX  30
#define WIFI_MESH_SEND_QUEUE       8
#define WIFI_MESH_RELAY_QUEUE      8
#define WIFI_MESH_RECV_QUEUE       8
#define WIFI_MESH_DUP_CACHE        1024    /* packet IDs remembered             */

#endif
