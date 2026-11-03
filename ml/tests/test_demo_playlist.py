"""pick() in demo_playlist.py: exact labels first, then confidence; every take must be right; group-only kept as fallback."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from demo_playlist import pick  # noqa: E402
from mlcommon import sc  # noqa: E402

DOG, CAT, MAMMAL, RAIN = (sc.by_name(n).id for n in ("DOG", "CAT", "MAMMAL", "WEATHER"))


def test_exact_beats_group_and_confidence_orders():
    preds = [("dog_a", DOG, MAMMAL, 95),           # group only, high confidence
             ("dog_b", DOG, DOG, 60),
             ("cat_a", CAT, CAT, 80)]
    got = pick(preds)[MAMMAL]
    assert [c["clip_id"] for c in got] == ["cat_a", "dog_b"]
    assert all(c["match"] == "exact" for c in got)


def test_every_take_must_be_right_and_min_conf_counts():
    preds = [("dog_a", DOG, DOG, 90), ("dog_a", DOG, CAT, 90),     # one take wrong -> out
             ("dog_b", DOG, DOG, 90), ("dog_b", DOG, DOG, 40),     # conf = lowest take
             ("rain", RAIN, RAIN, 70)]
    got = pick(preds)
    assert [c["clip_id"] for c in got[MAMMAL]] == ["dog_b"] and got[MAMMAL][0]["conf"] == 40
    assert got[MAMMAL][0]["takes"] == 2 and got[RAIN][0]["match"] == "exact"


def test_group_only_is_the_fallback():
    got = pick([("dog_a", DOG, MAMMAL, 70)])[MAMMAL]
    assert got[0]["match"] == "group" and got[0]["pred_id"] == MAMMAL
