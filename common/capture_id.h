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
static inline int capture_id_format(char* p_out, size_t cap, uint8_t node, uint32_t cap_id)
{
    return snprintf(p_out, cap, "N%u-%06lu", (unsigned)node, (unsigned long)cap_id);
}

/* "N2_000042" */
static inline int capture_id_file_stem(char* p_out, size_t cap, uint8_t node, uint32_t cap_id)
{
    return snprintf(p_out, cap, "N%u_%06lu", (unsigned)node, (unsigned long)cap_id);
}

/* Parses "N2-000042" or "N2_000042", optionally followed by more text (e.g. "_full.scl").
 * Returns false if s does not start with a capture id. */
static inline bool capture_id_parse(const char* p_str, uint8_t* p_node, uint32_t* p_cap_id)
{
    if (!p_str || (p_str[0] != 'N' && p_str[0] != 'n') || p_str[1] < '0' || p_str[1] > '9')
    {
        return false;
    }
    char* p_end;
    unsigned long count = strtoul(p_str + 1, &p_end, 10);
    if (count > 255 || (*p_end != '-' && *p_end != '_') || p_end[1] < '0' || p_end[1] > '9')
    {
        return false;
    }
    const char* p_digits = p_end + 1;
    unsigned long value = strtoul(p_digits, &p_end, 10);
    if (p_end - p_digits < 6 || value > 0xFFFFFFFFul)
    {
        return false;
    }
    *p_node = (uint8_t)count;
    *p_cap_id = (uint32_t)value;
    return true;
}

#endif
