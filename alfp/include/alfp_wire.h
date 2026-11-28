/*
 * alfp_wire.h - ALFP packet formats (what goes inside a mesh payload).
 *
 * Every packet:  [ver:4 | type:4] [session_id:2] ... type-specific ... [crc16:2]
 * All multi-byte fields are little-endian. crc16 covers every byte before it.
 *
 *   MANIFEST  sender -> bridge   announces a file: size, fragment count/size, CRC-32, metadata
 *   DATA      sender -> bridge   one fragment: seq, fragment count, payload
 *   POLL      sender -> bridge   "round finished, tell me what you are missing"
 *   NACK      bridge -> sender   exactly which fragments are missing (list or bitmap)
 *   ACK       bridge -> sender   file complete and CRC-32 verified (or a refusal)
 *
 * The mesh header (src, dst, pkt_id, hops) is added and removed by the mesh layer,
 * so it never appears here.
 */
#ifndef ALFP_WIRE_H
#define ALFP_WIRE_H

#include <stdbool.h>
#include <stdint.h>

#include "alfp_config.h"

#define ALFP_VERSION 1u

enum
{
    ALFP_T_MANIFEST = 1,
    ALFP_T_DATA = 2,
    ALFP_T_POLL = 3,
    ALFP_T_NACK = 4,
    ALFP_T_ACK = 5,
};

/* Fixed overheads, including the 2-byte CRC trailer. */
#define ALFP_DATA_OVERHEAD 9u      /* b0 sid2 seq2 count2 + crc2 */
#define ALFP_MANIFEST_OVERHEAD 19u /* b0 sid2 size4 count2 fsize2 crc4 radio1 metalen1 + crc2 */
#define ALFP_POLL_LEN 6u           /* b0 sid2 round1 + crc2 */
#define ALFP_ACK_LEN 6u            /* b0 sid2 status1 + crc2 */
#define ALFP_NACK_MIN 8u           /* b0 sid2 flags1 round1 fmt1 + crc2 (fmt ALL / NONE) */

/* NACK flags */
#define ALFP_NACK_NEED_MANIFEST 0x01u

/* NACK body formats: the bridge picks whichever encoding is shorter. */
enum
{
    ALFP_NACK_FMT_ALL = 0,    /* resend everything (bridge has no state for this session) */
    ALFP_NACK_FMT_LIST = 1,   /* n:1, then n x seq:2 */
    ALFP_NACK_FMT_BITMAP = 2, /* base:2 nbits:2, bitmap bytes; bit i set = seq base+i missing */
    ALFP_NACK_FMT_NONE = 3,   /* nothing missing from data; only flags matter (e.g. need manifest) */
};

/* ACK status */
enum
{
    ALFP_ACK_OK = 0,       /* file complete, CRC-32 matched */
    ALFP_ACK_CRC_FAIL = 1, /* all fragments arrived but CRC-32 did not match: resend all */
    ALFP_ACK_BUSY = 2,     /* bridge has no free session slot: try later */
    ALFP_ACK_STORAGE = 3,  /* bridge could not write to its SD card */
    ALFP_ACK_TOO_BIG = 4,  /* file exceeds ALFP_MAX_FRAGS or other limits */
};

/* Radio ids carried in the manifest, for logging and the dashboard. */
enum
{
    ALFP_RADIO_UNKNOWN = 0,
    ALFP_RADIO_LORA = 1,
    ALFP_RADIO_WIFI = 2,
};

typedef struct
{
    uint32_t file_size;
    uint16_t frag_count;
    uint16_t frag_size;
    uint32_t file_crc32;
    uint8_t radio_id;
    uint8_t meta_len;
    const uint8_t* p_meta; /* points into the packet buffer */
} alfp_manifest_t;

typedef struct
{
    uint16_t seq;
    uint16_t frag_count;
    uint16_t len;
    const uint8_t* p_payload; /* points into the packet buffer */
} alfp_data_t;

typedef struct
{
    uint8_t flags;
    uint8_t round;
    uint8_t fmt;
    /* LIST */
    uint8_t count;
    const uint8_t* p_list; /* n x little-endian uint16 */
    /* BITMAP */
    uint16_t base;
    uint16_t nbits;
    const uint8_t* p_bitmap;
} alfp_nack_t;

typedef struct
{
    uint8_t type;
    uint16_t sid;
    union
    {
        alfp_manifest_t manifest;
        alfp_data_t data;
        uint8_t round; /* POLL */
        alfp_nack_t nack;
        uint8_t status; /* ACK */
    } body;
} alfp_pkt_t;

typedef enum
{
    ALFP_DEC_OK = 0,
    ALFP_DEC_SHORT,     /* shorter than the minimum for its type */
    ALFP_DEC_CRC,       /* CRC-16 mismatch */
    ALFP_DEC_VERSION,   /* unknown protocol version */
    ALFP_DEC_TYPE,      /* unknown packet type */
    ALFP_DEC_BAD_FIELD, /* a length or field is inconsistent */
} alfp_dec_t;

/* Encoders write into out (capacity cap) and return the packet length, or 0 if it does not fit. */
uint16_t alfp_enc_manifest(uint8_t* p_out, uint16_t cap, uint16_t sid, const alfp_manifest_t* p_manifest);
uint16_t alfp_enc_data_header(uint8_t* p_out, uint16_t sid, uint16_t seq, uint16_t count);
uint16_t alfp_enc_data_finish(uint8_t* p_out, uint16_t payload_len); /* appends CRC, returns total */
uint16_t alfp_enc_poll(uint8_t* p_out, uint16_t cap, uint16_t sid, uint8_t round);
uint16_t alfp_enc_ack(uint8_t* p_out, uint16_t cap, uint16_t sid, uint8_t status);

/* NACK: pass the receiver's bitmap (bit set = RECEIVED) and let the encoder pick the shortest form. */
uint16_t alfp_enc_nack(uint8_t* p_out, uint16_t cap, uint16_t sid, uint8_t flags, uint8_t round,
                       const uint8_t* p_have_bitmap, uint16_t frag_count, bool b_all);

/* Decode and validate one packet. On success fills *p (pointers reference buf). */
alfp_dec_t alfp_decode(const uint8_t* p_buf, uint16_t len, alfp_pkt_t* p_pkt);

/* Helper: iterate the fragments a decoded NACK asks for. Calls fn(seq, user) for each. */
void alfp_nack_for_each(const alfp_nack_t* p_count, uint16_t frag_count, void (*p_callback)(uint16_t seq, void* p_user), void* p_user);

#endif
