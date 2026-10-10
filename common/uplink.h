/*
 * uplink.h - bridge node -> laptop over USB serial.   OWNER: Member 2
 *
 * When the bridge has reassembled and verified a file, it streams it to the laptop as text so it
 * survives any serial terminal and can be mixed with ordinary log lines:
 *
 *   @ALFP FILE v1 src=3 sid=00042 size=8000 crc=1a2b3c4d radio=lora frags=34 meta=01030000...
 *   @D <base64, 64 chars per line>
 *   ...
 *   @ALFP END crc=1a2b3c4d
 *
 * tools/bridge_receiver.py reads this, re-checks the CRC-32 and saves image + metadata JSON for the
 * backend. Portable C: the Pico passes a putchar-style function, the laptop tests pass a buffer.
 */
#ifndef UPLINK_H
#define UPLINK_H

#include <stdint.h>

#include "alfp.h"

typedef void (*uplink_write_fn)(const char* s, void* user);

/* Stream the finished file described by r. Returns 0 on success, -1 if the file cannot be read. */
int uplink_send_file(const alfp_storage_t* st, const alfp_rx_report_t* r, uplink_write_fn out, void* user);

#endif
