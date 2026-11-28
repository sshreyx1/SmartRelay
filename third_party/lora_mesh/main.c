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

static const mesh_radio_t* p_mesh = &g_lora_mesh_radio;

static uint32_t tx_seq;
static uint32_t rx_ok;
static uint32_t rx_bad;

static uint32_t now_ms( void )
{
    return to_ms_since_boot( get_absolute_time( ) );
}

static uint16_t build_test_packet( uint8_t* p_buf, uint32_t seq )
{
    const uint16_t len = p_mesh->p_max_payload( );
    p_buf[0]             = 'T';
    p_buf[1]             = ( uint8_t ) ( seq >> 24 );
    p_buf[2]             = ( uint8_t ) ( seq >> 16 );
    p_buf[3]             = ( uint8_t ) ( seq >> 8 );
    p_buf[4]             = ( uint8_t ) seq;
    for( uint16_t idx = 5; idx < len; idx++ )
    {
        p_buf[idx] = ( uint8_t ) ( seq * 31u + idx );
    }
    return len;
}

static void check_test_packet( uint8_t src, const uint8_t* p_buf, int len )
{
    uint8_t expected[256];
    if( (len < 5) || ('T' != p_buf[0]) )
    {
        printf( "RX from %u: %d bytes (not a test packet)\n", src, len );
        return;
    }
    const uint32_t seq = ( ( uint32_t ) p_buf[1] << 24 ) | ( ( uint32_t ) p_buf[2] << 16 ) | ( ( uint32_t ) p_buf[3] << 8 ) | p_buf[4];
    const uint16_t exp_len = build_test_packet( expected, seq );
    const bool     b_is_ok      = ( len == exp_len ) && (0 == memcmp( p_buf, expected, exp_len ));
    if( b_is_ok ) rx_ok++; else rx_bad++;

    int8_t rssi, snr;
    lora_mesh_last_link_quality( &rssi, &snr );
    printf( "RX from %u seq %lu, %d bytes, %s (last hop RSSI %d dBm, SNR %d dB) [ok %lu, bad %lu]\n", src,
            ( unsigned long ) seq, len, b_is_ok ? "byte-exact OK" : "MISMATCH", rssi, snr, ( unsigned long ) rx_ok,
            ( unsigned long ) rx_bad );
}

static void print_stats( void )
{
    mesh_stats_t stats;
    p_mesh->p_get_stats( &stats );
    printf( "STATS node %d: tx %lu rx %lu delivered %lu relayed %lu dup_dropped %lu hop_dropped %lu tx_fail %lu "
            "rx_error %lu rx_overflow %lu relay_overflow %lu drop_injected %lu\n",
            NODE_ID, ( unsigned long ) stats.tx_count, ( unsigned long ) stats.rx_count, ( unsigned long ) stats.delivered,
            ( unsigned long ) stats.relayed, ( unsigned long ) stats.dup_dropped, ( unsigned long ) stats.hop_dropped,
            ( unsigned long ) stats.tx_fail, ( unsigned long ) stats.rx_error, ( unsigned long ) stats.rx_overflow,
            ( unsigned long ) stats.relay_overflow, ( unsigned long ) stats.drop_injected );
    for( uint8_t node_id = 1; node_id <= 5; node_id++ )
    {
        uint32_t now_ms;
        if( (NODE_ID != node_id) && (p_mesh->p_last_heard( node_id, &now_ms )) )
        {
            printf( "  last heard node %u: %lu ms ago\n", node_id, ( unsigned long ) ( now_ms( ) - now_ms ) );
        }
    }
}

int main( void )
{
    stdio_init_all( );
    const uint32_t usb_start = now_ms( );
    while( (!stdio_usb_connected( )) && (( now_ms( ) - usb_start ) < USB_WAIT_MS) )
    {
        sleep_ms( 10 );
    }

    printf( "\n=== LoRa mesh test, node %d ===\n", NODE_ID );
    while( !lora_mesh_init( NODE_ID ) )
    {
        printf( "LoRa init failed - check wiring/power. Retrying in 2 s\n" );
        sleep_ms( 2000 );
    }
    printf( "max_payload = %u bytes, 250-byte packet time on air = %lu ms\n", p_mesh->p_max_payload( ),
            ( unsigned long ) lora_radio_time_on_air_ms( 255 ) );
    printf( "Keys: p pause/resume, s stats, d drop 30%%, b burst, h heartbeat\n\n" );

    bool     b_sending      = TEST_SEND_AT_BOOT;
    bool     b_dropping     = false;
    bool     b_heartbeat_on = false;
    uint32_t next_send    = now_ms( ) + 1000;
    uint32_t next_stats   = now_ms( ) + STATS_INTERVAL_MS;
    uint8_t  buf[256];

    for( ;; )
    {
        p_mesh->p_poll( );

        uint8_t src;
        int     len;
        while( ( len = p_mesh->p_recv( &src, buf, sizeof( buf ) ) ) >= 0 )
        {
            check_test_packet( src, buf, len );
        }

        const uint32_t now = now_ms( );
        if( b_sending && (( int32_t ) ( now - next_send ) >= 0) )
        {
            next_send = now + TEST_INTERVAL_MS + get_rand_32( ) % 1000;
            if( p_mesh->p_send( TEST_DST, buf, build_test_packet( buf, tx_seq ) ) )
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

        const int chr = getchar_timeout_us( 0 );
        switch( chr )
        {
        case 'p':
            b_sending = !b_sending;
            printf( "Sending %s\n", b_sending ? "ON" : "OFF" );
            break;
        case 's':
            print_stats( );
            break;
        case 'd':
            b_dropping = !b_dropping;
            p_mesh->p_set_drop_rate( b_dropping ? 0.3f : 0.0f );
            printf( "drop_rate = %s\n", b_dropping ? "0.3" : "0.0" );
            break;
        case 'b':
        {
            int accepted = 0;
            for( int idx = 0; idx < 12; idx++ )
            {
                if( p_mesh->p_send( TEST_DST, buf, build_test_packet( buf, tx_seq ) ) )
                {
                    accepted++;
                    tx_seq++;
                }
            }
            printf( "Burst: %d of 12 accepted, rest refused (queue full) - expected\n", accepted );
            break;
        }
        case 'h':
            b_heartbeat_on = !b_heartbeat_on;
            lora_mesh_set_heartbeat( b_heartbeat_on ? 5000 : 0 );
            printf( "Heartbeat %s\n", b_heartbeat_on ? "ON (5 s)" : "OFF" );
            break;
        default:
            break;
        }

        sleep_ms( 1 );
    }
}
