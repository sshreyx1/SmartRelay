/*
 * sound_image_pick.c - see sound_image_pick.h.
 */
#include "sound_image_pick.h"

#include <stdio.h>
#include <string.h>

#include "sound_classes.h"

void sip_reset(sip_table_t* t)
{
    memset(t, 0, sizeof(*t));
}

static sip_prefix_t* find(sip_table_t* t, const char* prefix, size_t len)
{
    for (uint32_t i = 0; i < t->n; i++)
    {
        if (strlen(t->p[i].prefix) == len && strncmp(t->p[i].prefix, prefix, len) == 0)
        {
            return &t->p[i];
        }
    }
    return NULL;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

bool sip_add_file(sip_table_t* t, const char* name)
{
    /* Hidden files are never images: macOS writes "._<name>" AppleDouble files next to every file
     * it copies onto a FAT card (follow-up F1), and those would otherwise count as their own
     * "._dog" group and could be picked and sent as if they were JPEGs. */
    if (name[0] == '.')
    {
        return false;
    }
    /* <prefix>_NN.jpg: the last '_' must be followed by exactly two digits and ".jpg" */
    const char* us = strrchr(name, '_');
    if (!us || us == name || (size_t)(us - name) > SIP_PREFIX_MAX)
    {
        return false;
    }
    if (us[1] < '0' || us[1] > '9' || us[2] < '0' || us[2] > '9' || us[3] != '.' || lower(us[4]) != 'j'
        || lower(us[5]) != 'p' || lower(us[6]) != 'g' || us[7] != '\0')
    {
        return false;
    }
    const unsigned nn = (unsigned)(us[1] - '0') * 10u + (unsigned)(us[2] - '0');
    const size_t len = (size_t)(us - name);
    sip_prefix_t* p = find(t, name, len);
    if (!p)
    {
        if (t->n >= SIP_MAX_PREFIXES)
        {
            return false;
        }
        p = &t->p[t->n++];
        memcpy(p->prefix, name, len);
        p->prefix[len] = '\0';
    }
    if (!(p->present[nn / 8u] & (1u << (nn % 8u))))
    {
        p->present[nn / 8u] = (uint8_t)(p->present[nn / 8u] | (1u << (nn % 8u)));
        p->count++;
    }
    return true;
}

unsigned sip_count(const sip_table_t* t, const char* prefix)
{
    for (uint32_t i = 0; i < t->n; i++)
    {
        if (strcmp(t->p[i].prefix, prefix) == 0)
        {
            return t->p[i].count;
        }
    }
    return 0;
}

static int take(sip_prefix_t* p, char* name, size_t cap)
{
    for (unsigned k = 0; k < SIP_MAX_INDEX; k++)
    {
        const unsigned nn = (p->next + k) % SIP_MAX_INDEX;
        if (p->present[nn / 8u] & (1u << (nn % 8u)))
        {
            p->next = (uint8_t)((nn + 1u) % SIP_MAX_INDEX);
            snprintf(name, cap, "%s_%02u.jpg", p->prefix, nn);
            return (int)nn;
        }
    }
    return -1;
}

int sip_pick(sip_table_t* t, uint8_t label_id, char* name, size_t cap)
{
    const char* chain[3]
        = { sound_class_prefix(label_id), sound_class_prefix(sound_class_group(label_id)), SIP_GENERIC };
    for (int i = 0; i < 3; i++)
    {
        if (!chain[i])
        {
            continue;
        }
        sip_prefix_t* p = find(t, chain[i], strlen(chain[i]));
        if (p && p->count)
        {
            return take(p, name, cap);
        }
    }
    /* Follow-up F6: nothing for this label, its group or "img" (e.g. an unknown sound on a card
     * with no img_NN.jpg). Any image beats dropping the capture: take the groups in turn. */
    for (uint32_t k = 0; k < t->n; k++)
    {
        sip_prefix_t* p = &t->p[(t->any_next + k) % t->n];
        if (p->count)
        {
            t->any_next = (uint8_t)((t->any_next + k + 1u) % t->n);
            return take(p, name, cap);
        }
    }
    return -1;
}
