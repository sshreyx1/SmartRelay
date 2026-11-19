/*
 * sound_classes.h - the sound label table: ONE source of truth for the node, the bridge tools,
 * the backend and the dashboard.   Owner: Rhaylene.   Python mirror: common/sound_classes.py
 * (host/tests/test_sound_classes.py parses this file and checks the two agree).
 *
 * capture_meta sound_class carries a label id from this table. The group comes from the table,
 * so the 27-byte meta layout stays version 1.
 *
 *   0-6     group only (no finer label, or the finer label was rejected)
 *   16-47   Phase A animal sound types (ESC-50)
 *   64-127  Phase B Singapore species (B1)
 *   0xFF    unknown / capture failed (CM_UNKNOWN_U8)
 *
 * Ids are stable forever and never reused. Old values 1 LOUD_ANIMAL_CALL / 2 HIGH_FREQ_RUSTLE
 * (mic.c thresholds) are retired: 1 and 2 now mean BIRD and MAMMAL, so all nodes are reflashed together.
 *
 * Columns: id, NAME, group id, kind, image prefix (SD card samples/<prefix>_NN.jpg).
 */
#ifndef SOUND_CLASSES_H
#define SOUND_CLASSES_H

#include <stdbool.h>
#include <stdint.h>

#define SC_KIND_GROUP 0u
#define SC_KIND_ANIMAL 1u
#define SC_KIND_SPECIES 2u

#define SC_UNKNOWN 0xFFu

/* clang-format off */
#define SOUND_CLASS_LIST(X) \
    X(0,  AMBIENT,     0, SC_KIND_GROUP,  "ambient")     \
    X(1,  BIRD,        1, SC_KIND_GROUP,  "bird")        \
    X(2,  MAMMAL,      2, SC_KIND_GROUP,  "mammal")      \
    X(3,  INSECT_FROG, 3, SC_KIND_GROUP,  "insect_frog") \
    X(4,  HUMAN,       4, SC_KIND_GROUP,  "human")       \
    X(5,  VEHICLE,     5, SC_KIND_GROUP,  "vehicle")     \
    X(6,  WEATHER,     6, SC_KIND_GROUP,  "weather")     \
    X(16, ROOSTER,     1, SC_KIND_ANIMAL, "rooster")     \
    X(17, HEN,         1, SC_KIND_ANIMAL, "hen")         \
    X(18, CROW,        1, SC_KIND_ANIMAL, "crow")        \
    X(19, SONGBIRD,    1, SC_KIND_ANIMAL, "songbird")    \
    X(20, DOG,         2, SC_KIND_ANIMAL, "dog")         \
    X(21, CAT,         2, SC_KIND_ANIMAL, "cat")         \
    X(22, COW,         2, SC_KIND_ANIMAL, "cow")         \
    X(23, PIG,         2, SC_KIND_ANIMAL, "pig")         \
    X(24, SHEEP,       2, SC_KIND_ANIMAL, "sheep")       \
    X(25, FROG,        3, SC_KIND_ANIMAL, "frog")        \
    X(26, CRICKET,     3, SC_KIND_ANIMAL, "cricket")     \
    X(27, INSECT_BUZZ, 3, SC_KIND_ANIMAL, "insect_buzz")
/* clang-format on */

enum
{
#define SC_ENUM(id, name, group, kind, prefix) SC_##name = id,
    SOUND_CLASS_LIST(SC_ENUM)
#undef SC_ENUM
};

static inline bool sound_class_valid(uint8_t id)
{
    switch (id)
    {
#define SC_CASE(id, name, group, kind, prefix) case id:
        SOUND_CLASS_LIST(SC_CASE)
#undef SC_CASE
        return true;
        default:
            return false;
    }
}

/* "DOG"; "UNKNOWN" for 0xFF; "INVALID" for an id not in the table */
static inline const char* sound_class_name(uint8_t id)
{
    switch (id)
    {
#define SC_CASE(id, name, group, kind, prefix)                                                                         \
    case id:                                                                                                           \
        return #name;
        SOUND_CLASS_LIST(SC_CASE)
#undef SC_CASE
        default:
            return id == SC_UNKNOWN ? "UNKNOWN" : "INVALID";
    }
}

/* group id (0-6) of a label; SC_UNKNOWN for unknown / invalid ids */
static inline uint8_t sound_class_group(uint8_t id)
{
    switch (id)
    {
#define SC_CASE(id, name, group, kind, prefix)                                                                         \
    case id:                                                                                                           \
        return group;
        SOUND_CLASS_LIST(SC_CASE)
#undef SC_CASE
        default:
            return SC_UNKNOWN;
    }
}

static inline uint8_t sound_class_kind(uint8_t id)
{
    switch (id)
    {
#define SC_CASE(id, name, group, kind, prefix)                                                                         \
    case id:                                                                                                           \
        return kind;
        SOUND_CLASS_LIST(SC_CASE)
#undef SC_CASE
        default:
            return SC_UNKNOWN;
    }
}

/* image prefix ("dog"); NULL for unknown / invalid ids */
static inline const char* sound_class_prefix(uint8_t id)
{
    switch (id)
    {
#define SC_CASE(id, name, group, kind, prefix)                                                                         \
    case id:                                                                                                           \
        return prefix;
        SOUND_CLASS_LIST(SC_CASE)
#undef SC_CASE
        default:
            return 0;
    }
}

#endif
