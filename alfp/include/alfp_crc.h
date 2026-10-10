/*
 * alfp_crc.h - checksums used by ALFP.
 *
 * CRC-16/CCITT-FALSE protects every packet (header + payload).
 *   poly 0x1021, init 0xFFFF, no reflection. Check("123456789") = 0x29B1.
 *   Keeps Hamming distance 4 up to 32,751 bits, so a 250-byte packet (2,000 bits)
 *   has every 1, 2 and 3-bit error and every burst up to 16 bits detected.
 *
 * CRC-32 (IEEE, same as zlib/PNG) protects the whole file end to end.
 *   Check("123456789") = 0xCBF43926.
 *
 * Both are incremental: pass the previous value back in to continue over more bytes.
 */
#ifndef ALFP_CRC_H
#define ALFP_CRC_H

#include <stddef.h>
#include <stdint.h>

#define ALFP_CRC16_INIT 0xFFFFu
#define ALFP_CRC32_INIT 0x00000000u

uint16_t alfp_crc16(uint16_t crc, const uint8_t* data, size_t len);

/* Start with ALFP_CRC32_INIT; the result is the finished CRC-32 of all bytes so far. */
uint32_t alfp_crc32(uint32_t crc, const uint8_t* data, size_t len);

#endif
