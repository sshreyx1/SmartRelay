/*
 * capture_id.h - the one human-readable capture name, used everywhere a person sees a capture:
 * serial logs, SD file names, the backend's display_id, the dashboard badge, CSV exports.
 * Python mirror: common/capture_id.py.   Owner: Rhaylene (shared).
 *
 *   display:  N<node>-<id, 6 digits>   e.g. N2-000042
 *   file:     N<node>_<id, 6 digits>   e.g. N2_000042  (no '-' in SD file names)
 * Ids above 999999 simply print more digits. The wire field capture_id (u32) is unchanged; the
 * node keeps it on the SD card (meta/next_id) so it never restarts after a reboot.
 *
 * Pure C, no Pico headers.
 */
#ifndef CAPTURE_ID_H
#define CAPTURE_ID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CAPTURE_ID_STR_MAX 20 /* "N255-4294967295" + NUL fits */

/* "N2-000042"; returns the length snprintf would write */
static inline int capture_id_format(char* out, size_t cap, uint8_t node, uint32_t id)
{
    return snprintf(out, cap, "N%u-%06lu", (unsigned)node, (unsigned long)id);
}

/* "N2_000042" */
static inline int capture_id_file_stem(char* out, size_t cap, uint8_t node, uint32_t id)
{
    return snprintf(out, cap, "N%u_%06lu", (unsigned)node, (unsigned long)id);
}

/* Parses "N2-000042" or "N2_000042", optionally followed by more text (e.g. "_full.scl").
 * Returns false if s does not start with a capture id. */
static inline bool capture_id_parse(const char* s, uint8_t* node, uint32_t* id)
{
    if (!s || (s[0] != 'N' && s[0] != 'n') || s[1] < '0' || s[1] > '9')
    {
        return false;
    }
    char* end;
    unsigned long n = strtoul(s + 1, &end, 10);
    if (n > 255 || (*end != '-' && *end != '_') || end[1] < '0' || end[1] > '9')
    {
        return false;
    }
    const char* digits = end + 1;
    unsigned long v = strtoul(digits, &end, 10);
    if (end - digits < 6 || v > 0xFFFFFFFFul)
    {
        return false;
    }
    *node = (uint8_t)n;
    *id = (uint32_t)v;
    return true;
}

#endif
