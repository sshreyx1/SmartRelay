/*
 * test_sound_image_pick.c - firmware/sensing/sound_image_pick.c: label -> group -> generic fallback,
 * round-robin within a prefix, gaps in the numbering, and names that must be ignored.
 */
#include <stdio.h>
#include <string.h>

#include "sound_classes.h"
#include "sound_image_pick.h"

static sip_table_t t;
static int fails;

static void expect(uint8_t label, const char* want)
{
    char name[32] = "";
    int nn = sip_pick(&t, label, name, sizeof(name));
    if ((want == NULL && nn != -1) || (want && (nn < 0 || strcmp(name, want) != 0)))
    {
        printf("FAIL: label %s -> '%s' (%d), want '%s'\n", sound_class_name(label), name, nn, want ? want : "(none)");
        fails++;
    }
}

int main(void)
{
    sip_reset(&t);
    expect(SC_DOG, NULL); /* empty card */

    const char* files[] = {"dog_00.jpg", "dog_02.JPG", "bird_00.jpg", "img_00.jpg", "img_01.jpg",
                           "notes.txt",  "dog_1.jpg",  "dog_00.png",  "_00.jpg",    "dog_00.jpg"};
    int accepted = 0;
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++)
    {
        accepted += sip_add_file(&t, files[i]);
    }
    if (accepted != 6 || sip_count(&t, "dog") != 2 || sip_count(&t, "img") != 2)
    {
        printf("FAIL: accepted %d, dog %u, img %u\n", accepted, sip_count(&t, "dog"), sip_count(&t, "img"));
        fails++;
    }

    expect(SC_DOG, "dog_00.jpg"); /* own label, round-robin over 00 and 02 (gap at 01) */
    expect(SC_DOG, "dog_02.jpg");
    expect(SC_DOG, "dog_00.jpg");
    expect(SC_ROOSTER, "bird_00.jpg"); /* no rooster images -> group BIRD */
    expect(SC_BIRD, "bird_00.jpg");
    expect(SC_CAT, "img_00.jpg"); /* no cat, no mammal -> generic */
    expect(SC_WEATHER, "img_01.jpg");
    expect(SC_UNKNOWN, "img_00.jpg"); /* capture failed -> generic */
    expect(200, "img_01.jpg");        /* invalid id -> generic */

    sip_reset(&t);
    for (int i = 0; i < SIP_MAX_PREFIXES + 5; i++)
    {
        char f[32];
        snprintf(f, sizeof(f), "p%d_00.jpg", i);
        sip_add_file(&t, f);
    }
    if (t.n != SIP_MAX_PREFIXES)
    {
        printf("FAIL: table holds %u prefixes, want %d\n", t.n, SIP_MAX_PREFIXES);
        fails++;
    }
    printf("sound_image_pick: sizeof(sip_table_t) = %u bytes, %s\n", (unsigned)sizeof(sip_table_t),
           fails ? "FAILED" : "all tests passed");
    return fails ? 1 : 0;
}
