"""
sound_classes.py - Python mirror of common/sound_classes.h (the sound label table).

host/tests/test_sound_classes.py parses the C header and checks it matches CLASSES exactly,
so edit both files together. Used by ml/, tools/bridge_receiver.py and backend/.

    import sound_classes as sc
    sc.name(20)            -> "DOG"
    sc.group(20)           -> 2
    sc.group_name(20)      -> "MAMMAL"
    sc.by_name("DOG").id   -> 20
"""
from typing import NamedTuple, Optional

KIND_GROUP, KIND_ANIMAL, KIND_SPECIES = 0, 1, 2
KIND_NAMES = {KIND_GROUP: "group", KIND_ANIMAL: "animal", KIND_SPECIES: "species"}
UNKNOWN = 0xFF


class SoundClass(NamedTuple):
    id: int
    name: str
    group: int
    kind: int
    prefix: str


CLASSES = [
    SoundClass(0, "AMBIENT", 0, KIND_GROUP, "ambient"),
    SoundClass(1, "BIRD", 1, KIND_GROUP, "bird"),
    SoundClass(2, "MAMMAL", 2, KIND_GROUP, "mammal"),
    SoundClass(3, "INSECT_FROG", 3, KIND_GROUP, "insect_frog"),
    SoundClass(4, "HUMAN", 4, KIND_GROUP, "human"),
    SoundClass(5, "VEHICLE", 5, KIND_GROUP, "vehicle"),
    SoundClass(6, "WEATHER", 6, KIND_GROUP, "weather"),
    SoundClass(16, "ROOSTER", 1, KIND_ANIMAL, "rooster"),
    SoundClass(17, "HEN", 1, KIND_ANIMAL, "hen"),
    SoundClass(18, "CROW", 1, KIND_ANIMAL, "crow"),
    SoundClass(19, "SONGBIRD", 1, KIND_ANIMAL, "songbird"),
    SoundClass(20, "DOG", 2, KIND_ANIMAL, "dog"),
    SoundClass(21, "CAT", 2, KIND_ANIMAL, "cat"),
    SoundClass(22, "COW", 2, KIND_ANIMAL, "cow"),
    SoundClass(23, "PIG", 2, KIND_ANIMAL, "pig"),
    SoundClass(24, "SHEEP", 2, KIND_ANIMAL, "sheep"),
    SoundClass(25, "FROG", 3, KIND_ANIMAL, "frog"),
    SoundClass(26, "CRICKET", 3, KIND_ANIMAL, "cricket"),
    SoundClass(27, "INSECT_BUZZ", 3, KIND_ANIMAL, "insect_buzz"),
]

BY_ID = {c.id: c for c in CLASSES}
BY_NAME = {c.name: c for c in CLASSES}
GROUP_IDS = [c.id for c in CLASSES if c.kind == KIND_GROUP]
ANIMAL_GROUPS = [1, 2, 3]  # groups that have a stage-2 model


def get(label_id: int) -> Optional[SoundClass]:
    return BY_ID.get(label_id)


def by_name(name: str) -> SoundClass:
    return BY_NAME[name]


def name(label_id: Optional[int]) -> str:
    if label_id is None or label_id == UNKNOWN:
        return "UNKNOWN"
    c = BY_ID.get(label_id)
    return c.name if c else "INVALID"


def group(label_id: Optional[int]) -> Optional[int]:
    c = BY_ID.get(label_id) if label_id is not None else None
    return c.group if c else None


def group_name(label_id: Optional[int]) -> Optional[str]:
    g = group(label_id)
    return None if g is None else BY_ID[g].name


def kind_name(label_id: Optional[int]) -> Optional[str]:
    c = BY_ID.get(label_id) if label_id is not None else None
    return KIND_NAMES[c.kind] if c else None


def labels_in_group(group_id: int) -> list:
    """Finer (non-group) labels of a group, in id order."""
    return [c for c in CLASSES if c.group == group_id and c.kind != KIND_GROUP]
