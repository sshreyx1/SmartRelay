/*
 * sound_image_pick.c - see sound_image_pick.h.
 */
#include "sound_image_pick.h"

#include <stdio.h>
#include <string.h>

#include "sound_classes.h"

void sip_reset(sip_table_t* p_table)
{
    memset(p_table, 0, sizeof(*p_table));
}

static sip_prefix_t* find(sip_table_t* p_table, const char* p_prefix, size_t len)
{
    for (uint32_t idx = 0; idx < p_table->count; idx++)
    {
        if ((strlen(p_table->prefixes[idx].prefix) == len) && (0 == strncmp(p_table->prefixes[idx].prefix, p_prefix, len)))
        {
            return &p_table->prefixes[idx];
        }
    }
    return NULL;
}

static int lower(int chr)
{
    return (chr >= 'A') && (chr <= 'Z') ? chr + 32 : chr;
}

bool sip_add_file(sip_table_t* p_table, const char* p_name)
{
    /* Hidden files are never images: macOS writes "._<name>" AppleDouble files next to every file
     * it copies onto a FAT card (follow-up F1), and those would otherwise count as their own
     * "._dog" group and could be picked and sent as if they were JPEGs. */
    if ('.' == p_name[0])
    {
        return false;
    }
    /* <prefix>_NN.jpg: the last '_' must be followed by exactly two digits and ".jpg" */
    const char* p_uscore = strrchr(p_name, '_');
    if ((!p_uscore) || (p_uscore == p_name) || ((size_t)(p_uscore - p_name) > SIP_PREFIX_MAX))
    {
        return false;
    }
    if ((p_uscore[1] < '0') || (p_uscore[1] > '9') || (p_uscore[2] < '0') || (p_uscore[2] > '9') || ('.' != p_uscore[3]) || ('j' != lower(p_uscore[4]))
        || ('p' != lower(p_uscore[5])) || ('g' != lower(p_uscore[6])) || ('\0' != p_uscore[7]))
    {
        return false;
    }
    const unsigned image_num = (unsigned)(p_uscore[1] - '0') * 10u + (unsigned)(p_uscore[2] - '0');
    const size_t len = (size_t)(p_uscore - p_name);
    sip_prefix_t* p_prefix_ent = find(p_table, p_name, len);
    if (!p_prefix_ent)
    {
        if (p_table->count >= SIP_MAX_PREFIXES)
        {
            return false;
        }
        p_prefix_ent = &p_table->prefixes[p_table->count++];
        memcpy(p_prefix_ent->prefix, p_name, len);
        p_prefix_ent->prefix[len] = '\0';
    }
    if (!(p_prefix_ent->present[image_num / 8u] & (1u << (image_num % 8u))))
    {
        p_prefix_ent->present[image_num / 8u] = (uint8_t)(p_prefix_ent->present[image_num / 8u] | (1u << (image_num % 8u)));
        p_prefix_ent->count++;
    }
    return true;
}

unsigned sip_count(const sip_table_t* p_table, const char* p_prefix)
{
    for (uint32_t idx = 0; idx < p_table->count; idx++)
    {
        if (0 == strcmp(p_table->prefixes[idx].prefix, p_prefix))
        {
            return p_table->prefixes[idx].count;
        }
    }
    return 0;
}

static int take(sip_prefix_t* p_prefix_ent, char* p_name, size_t cap)
{
    for (unsigned kdx = 0; kdx < SIP_MAX_INDEX; kdx++)
    {
        const unsigned image_num = (p_prefix_ent->next + kdx) % SIP_MAX_INDEX;
        if (p_prefix_ent->present[image_num / 8u] & (1u << (image_num % 8u)))
        {
            p_prefix_ent->next = (uint8_t)((image_num + 1u) % SIP_MAX_INDEX);
            snprintf(p_name, cap, "%s_%02u.jpg", p_prefix_ent->prefix, image_num);
            return (int)image_num;
        }
    }
    return -1;
}

int sip_pick(sip_table_t* p_table, uint8_t label_id, char* p_name, size_t cap)
{
    const char* chain[3]
        = { sound_class_prefix(label_id), sound_class_prefix(sound_class_group(label_id)), SIP_GENERIC };
    for (int idx = 0; idx < 3; idx++)
    {
        if (!chain[idx])
        {
            continue;
        }
        sip_prefix_t* p_prefix_ent = find(p_table, chain[idx], strlen(chain[idx]));
        if (p_prefix_ent && (p_prefix_ent->count))
        {
            return take(p_prefix_ent, p_name, cap);
        }
    }
    /* Follow-up F6: nothing for this label, its group or "img" (e.g. an unknown sound on a card
     * with no img_NN.jpg). Any image beats dropping the capture: take the groups in turn. */
    for (uint32_t kdx = 0; kdx < p_table->count; kdx++)
    {
        sip_prefix_t* p_prefix_ent = &p_table->prefixes[(p_table->any_next + kdx) % p_table->count];
        if (p_prefix_ent->count)
        {
            p_table->any_next = (uint8_t)((p_table->any_next + kdx + 1u) % p_table->count);
            return take(p_prefix_ent, p_name, cap);
        }
    }
    return -1;
}
