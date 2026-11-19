/*
 * sound_image_pick.h - choose the capture's stand-in "camera" image from what the node heard.
 *                      Owner: Rhaylene. Pure C, host-tested (host/tests/test_sound_image_pick.c).
 *
 * SD card layout: samples/<prefix>_NN.jpg, NN = 00-99, prefix from common/sound_classes.h, e.g.
 *   dog_00.jpg dog_01.jpg   rooster_00.jpg   bird_00.jpg (group fallback)   img_00.jpg (generic fallback)
 * At boot comms lists samples/ once and feeds every name to sip_add_file(). Per capture,
 * sip_pick() walks label -> its group -> "img" and takes the first prefix that has images,
 * round-robin within that prefix; if none of those has any, it takes any group that does, in
 * turn. Gaps in the numbering are fine. Hidden files (".*", incl. macOS "._*") are ignored.
 */
#ifndef SOUND_IMAGE_PICK_H
#define SOUND_IMAGE_PICK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIP_MAX_PREFIXES 48
#define SIP_PREFIX_MAX 15 /* characters */
#define SIP_MAX_INDEX 100 /* NN = 00..99 */
#define SIP_GENERIC "img"

typedef struct
{
    char prefix[SIP_PREFIX_MAX + 1];
    uint8_t present[(SIP_MAX_INDEX + 7) / 8]; /* bit NN set = <prefix>_NN.jpg exists */
    uint8_t count;
    uint8_t next; /* round-robin position: next NN to try */
} sip_prefix_t;

typedef struct
{
    sip_prefix_t p[SIP_MAX_PREFIXES];
    uint8_t n;
    uint8_t any_next; /* last-resort fallback: next group to take from */
} sip_table_t;

void sip_reset(sip_table_t* t);

/* Counts "<prefix>_NN.jpg" (case-insensitive .jpg). Other names, and any name starting with '.',
 * are ignored (returns false). */
bool sip_add_file(sip_table_t* t, const char* name);

/* Images known for a prefix (0 if none). */
unsigned sip_count(const sip_table_t* t, const char* prefix);

/* Picks the image for label_id (common/sound_classes.h; 0xFF = unknown -> generic).
 * Writes the file name ("dog_01.jpg", no folder) to name and returns NN, or -1 only if the card
 * has no usable image at all. */
int sip_pick(sip_table_t* t, uint8_t label_id, char* name, size_t cap);

#endif
