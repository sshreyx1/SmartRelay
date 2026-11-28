/*
 * test_sound_image_pick.c - firmware/sensing/sound_image_pick.c: label -> group
 * -> generic fallback, round-robin within a prefix, gaps in the numbering, and
 * names that must be ignored.
 */
#include <stdio.h>
#include <string.h>

#include "sound_classes.h"
#include "sound_image_pick.h"

static sip_table_t table;
static int         fails;

static void expect (uint8_t label, const char* p_want)
{
    char name[32]  = "";
    int  image_num = sip_pick(&table, label, name, sizeof(name));
    if (((NULL == p_want) && (-1 != image_num)) ||
        (p_want && ((image_num < 0) || (0 != strcmp(name, p_want)))))
    {
        printf("FAIL: label %s -> '%s' (%d), want '%s'\n",
               sound_class_name(label), name, image_num,
               p_want ? p_want : "(none)");
        fails++;
    }
}

int main (void)
{
    sip_reset(&table);
    expect(SC_DOG, NULL); /* empty card */

    const char* files[] = {"dog_00.jpg", "dog_02.JPG", "bird_00.jpg",
                           "img_00.jpg", "img_01.jpg", "notes.txt", "dog_1.jpg",
                           "dog_00.png", "_00.jpg", "dog_00.jpg",
                           /* F1: dot-files ignored */
                           "._dog_00.jpg", "._img_05.jpg", ".hidden_00.jpg"};
    int         accepted = 0;
    for (unsigned idx = 0; idx < sizeof(files) / sizeof(files[0]); idx++)
    {
        accepted += sip_add_file(&table, files[idx]);
    }
    if ((6 != accepted) || (2 != sip_count(&table, "dog")) ||
        (2 != sip_count(&table, "img")))
    {
        printf("FAIL: accepted %d, dog %u, img %u\n", accepted,
               sip_count(&table, "dog"), sip_count(&table, "img"));
        fails++;
    }

    /* own label, round-robin over 00 and 02 (gap at 01) */
    expect(SC_DOG, "dog_00.jpg");
    expect(SC_DOG, "dog_02.jpg");
    expect(SC_DOG, "dog_00.jpg");
    expect(SC_ROOSTER, "bird_00.jpg"); /* no rooster images -> group BIRD */
    expect(SC_BIRD, "bird_00.jpg");
    expect(SC_CAT, "img_00.jpg"); /* no cat, no mammal -> generic */
    expect(SC_WEATHER, "img_01.jpg");
    expect(SC_UNKNOWN, "img_00.jpg"); /* capture failed -> generic */
    expect(200, "img_01.jpg");        /* invalid id -> generic */

    /* F6: a card with no img_NN.jpg (node 2's) still gives an unknown sound an
     * image, any group, in turn, instead of dropping the capture. */
    sip_reset(&table);
    sip_add_file(&table, "dog_00.jpg");
    sip_add_file(&table, "rooster_03.jpg");
    sip_add_file(&table, "._img_00.jpg");
    expect(SC_UNKNOWN, "dog_00.jpg");
    expect(SC_UNKNOWN, "rooster_03.jpg");
    expect(SC_UNKNOWN, "dog_00.jpg");
    expect(SC_CAT, "rooster_03.jpg");
    expect(SC_DOG, "dog_00.jpg"); /* own label still wins */

    sip_reset(&table);
    for (int idx = 0; idx < SIP_MAX_PREFIXES + 5; idx++)
    {
        char fname[32];
        snprintf(fname, sizeof(fname), "p%d_00.jpg", idx);
        sip_add_file(&table, fname);
    }
    if (SIP_MAX_PREFIXES != table.count)
    {
        printf("FAIL: table holds %u prefixes, want %d\n", table.count,
               SIP_MAX_PREFIXES);
        fails++;
    }
    printf("sound_image_pick: sizeof(sip_table_t) = %u bytes, %s\n",
           (unsigned)sizeof(sip_table_t),
           fails ? "FAILED" : "all tests passed");
    return fails ? 1 : 0;
}

/*** end of file ***/

