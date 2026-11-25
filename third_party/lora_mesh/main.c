/*
 * main.c - LoRa mesh test program (CSC09, Member 1). Not the team's main loop:
 * it exists to run the contract's point-to-point and flooding tests.
 *
 * Every test packet is max_payload() bytes: 'T', a 32-bit sequence number, then a
 * pattern derived from the sequence number, so the receiver can prove byte-exact delivery.
 *
 * Serial keys (COM port, 115200):
 *   p  pause/resume sending          s  print stats now
 *   d  toggle drop_rate 0 <-> 0.3    b  burst 12 sends (queue-full test)
 *   h  toggle 5 s heartbeat (bridge)
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "pico/rand.h"
#include "lora_mesh.h"
#include "lora_radio.h"

/* ---- Per-board settings: change NODE_ID before flashing each board ---- */
#define NODE_ID           1               // 1-5 = A-E
#define TEST_DST          MESH_BROADCAST  // or a node ID, e.g. 3 for the A -> B -> C relay test
#define TEST_SEND_AT_BOOT ( NODE_ID == 1 )  // only node 1 sends by default, press p on others
#define TEST_INTERVAL_MS  5000            // plus up to 1 s random jitter
#define STATS_INTERVAL_MS 15000
#define USB_WAIT_MS       3000            // don't block forever: relay nodes may have no serial monitor

static const mesh_radio_t* mesh = &lora_mesh_radio;

static uint32_t tx_seq;
static uint32_t rx_ok;
static uint32_t rx_bad;

static uint32_t now_ms( void )
{
    return to_ms_since_boot( get_absolute_time( ) );
}

static uint16_t build_test_packet( uint8_t* buf, uint32_t seq )
{
    const uint16_t len = mesh->max_payload( );
    buf[0]             = 'T';
    buf[1]             = ( uint8_t ) ( seq >> 24 );
    buf[2]             = ( uint8_t ) ( seq >> 16 );
    buf[3]             = ( uint8_t ) ( seq >> 8 );
    buf[4]             = ( uint8_t ) seq;
    for( uint16_t i = 5; i < len; i++ )
    {
        buf[i] = ( uint8_t ) ( seq * 31u + i );
    }
    return len;
}

static void check_test_packet( uint8_t src, const uint8_t* buf, int len )
{
    uint8_t expected[256];
    if( len < 5 || buf[0] != 'T' )
    {
        printf( "RX from %u: %d bytes (not a test packet)\n", src, len );
        return;
    }
    const uint32_t seq = ( ( uint32_t ) buf[1] << 24 ) | ( ( uint32_t ) buf[2] << 16 ) | ( ( uint32_t ) buf[3] << 8 ) | buf[4];
    const uint16_t exp_len = build_test_packet( expected, seq );
    const bool     ok      = ( len == exp_len ) && memcmp( buf, expected, exp_len ) == 0;
    if( ok ) rx_ok++; else rx_bad++;

    int8_t rssi, snr;
    lora_mesh_last_link_quality( &rssi, &snr );
    printf( "RX from %u seq %lu, %d bytes, %s (last hop RSSI %d dBm, SNR %d dB) [ok %lu, bad %lu]\n", src,
            ( unsigned long ) seq, len, ok ? "byte-exact OK" : "MISMATCH", rssi, snr, ( unsigned long ) rx_ok,
            ( unsigned long ) rx_bad );
}

static void print_stats( void )
{
    mesh_stats_t s;
    mesh->get_stats( &s );
    printf( "STATS node %d: tx %lu rx %lu delivered %lu relayed %lu dup_dropped %lu hop_dropped %lu tx_fail %lu "
            "rx_error %lu rx_overflow %lu relay_overflow %lu drop_injected %lu\n",
            NODE_ID, ( unsigned long ) s.tx, ( unsigned long ) s.rx, ( unsigned long ) s.delivered,
            ( unsigned long ) s.relayed, ( unsigned long ) s.dup_dropped, ( unsigned long ) s.hop_dropped,
            ( unsigned long ) s.tx_fail, ( unsigned long ) s.rx_error, ( unsigned long ) s.rx_overflow,
            ( unsigned long ) s.relay_overflow, ( unsigned long ) s.drop_injected );
    for( uint8_t id = 1; id <= 5; id++ )
    {
        uint32_t t;
        if( id != NODE_ID && mesh->last_heard( id, &t ) )
        {
            printf( "  last heard node %u: %lu ms ago\n", id, ( unsigned long ) ( now_ms( ) - t ) );
        }
    }
}

int main( void )
{
    stdio_init_all( );
    const uint32_t usb_start = now_ms( );
    while( !stdio_usb_connected( ) && ( now_ms( ) - usb_start ) < USB_WAIT_MS )
    {
        sleep_ms( 10 );
    }

    printf( "\n=== LoRa mesh test, node %d ===\n", NODE_ID );
    while( !lora_mesh_init( NODE_ID ) )
    {
        printf( "LoRa init failed - check wiring/power. Retrying in 2 s\n" );
        sleep_ms( 2000 );
    }
    printf( "max_payload = %u bytes, 250-byte packet time on air = %lu ms\n", mesh->max_payload( ),
            ( unsigned long ) lora_radio_time_on_air_ms( 255 ) );
    printf( "Keys: p pause/resume, s stats, d drop 30%%, b burst, h heartbeat\n\n" );

    bool     sending      = TEST_SEND_AT_BOOT;
    bool     dropping     = false;
    bool     heartbeat_on = false;
    uint32_t next_send    = now_ms( ) + 1000;
    uint32_t next_stats   = now_ms( ) + STATS_INTERVAL_MS;
    uint8_t  buf[256];

    for( ;; )
    {
        mesh->poll( );

        uint8_t src;
        int     len;
        while( ( len = mesh->recv( &src, buf, sizeof( buf ) ) ) >= 0 )
        {
            check_test_packet( src, buf, len );
        }

        const uint32_t now = now_ms( );
        if( sending && ( int32_t ) ( now - next_send ) >= 0 )
        {
            next_send = now + TEST_INTERVAL_MS + get_rand_32( ) % 1000;
            if( mesh->send( TEST_DST, buf, build_test_packet( buf, tx_seq ) ) )
            {
                printf( "TX seq %lu to %u\n", ( unsigned long ) tx_seq, TEST_DST );
                tx_seq++;
            }
            else
            {
                printf( "TX seq %lu: send queue full\n", ( unsigned long ) tx_seq );
            }
        }

        if( ( int32_t ) ( now - next_stats ) >= 0 )
        {
            next_stats = now + STATS_INTERVAL_MS;
            print_stats( );
        }

        const int c = getchar_timeout_us( 0 );
        switch( c )
        {
        case 'p':
            sending = !sending;
            printf( "Sending %s\n", sending ? "ON" : "OFF" );
            break;
        case 's':
            print_stats( );
            break;
        case 'd':
            dropping = !dropping;
            mesh->set_drop_rate( dropping ? 0.3f : 0.0f );
            printf( "drop_rate = %s\n", dropping ? "0.3" : "0.0" );
            break;
        case 'b':
        {
            int accepted = 0;
            for( int i = 0; i < 12; i++ )
            {
                if( mesh->send( TEST_DST, buf, build_test_packet( buf, tx_seq ) ) )
                {
                    accepted++;
                    tx_seq++;
                }
            }
            printf( "Burst: %d of 12 accepted, rest refused (queue full) - expected\n", accepted );
            break;
        }
        case 'h':
            heartbeat_on = !heartbeat_on;
            lora_mesh_set_heartbeat( heartbeat_on ? 5000 : 0 );
            printf( "Heartbeat %s\n", heartbeat_on ? "ON (5 s)" : "OFF" );
            break;
        default:
            break;
        }

        sleep_ms( 1 );
    }
}
