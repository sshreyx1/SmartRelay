#!/usr/bin/env python3
"""
test_sound_classes.py - common/sound_classes.h and common/sound_classes.py must list exactly the same
labels (id, name, group, kind, image prefix). Run by ctest (sound_classes) and by pytest.

Run:      python host/tests/test_sound_classes.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "common"))
import sound_classes as sc  # noqa: E402

ROW = re.compile(r'X\(\s*(\d+),\s*(\w+),\s*(\d+),\s*SC_KIND_(\w+),\s*"([^"]*)"\s*\)')
KINDS = {"GROUP": sc.KIND_GROUP, "ANIMAL": sc.KIND_ANIMAL, "SPECIES": sc.KIND_SPECIES}


def header_rows():
    text = (ROOT / "common" / "sound_classes.h").read_text(encoding="utf-8")
    body = text[text.index("#define SOUND_CLASS_LIST"):text.index("/* clang-format on */")]
    return [sc.SoundClass(int(i), n, int(g), KINDS[k], p) for i, n, g, k, p in ROW.findall(body)]


def check() -> list:
    errors = []
    rows = header_rows()
    if rows != sc.CLASSES:
        h, p = set(rows), set(sc.CLASSES)
        errors += [f"only in sound_classes.h: {r}" for r in sorted(h - p)]
        errors += [f"only in sound_classes.py: {r}" for r in sorted(p - h)]
        if not errors:
            errors.append("same rows, different order")
    ids = [r.id for r in rows]
    if len(ids) != len(set(ids)):
        errors.append("duplicate ids")
    if sc.UNKNOWN in ids:
        errors.append("0xFF is reserved for unknown")
    for r in rows:
        if r.kind == sc.KIND_GROUP and r.group != r.id:
            errors.append(f"group row {r.name} must be its own group")
        if sc.BY_ID.get(r.group) is None or sc.BY_ID[r.group].kind != sc.KIND_GROUP:
            errors.append(f"{r.name}: group {r.group} is not a group row")
    return errors


def test_header_matches_python():
    assert check() == []


if __name__ == "__main__":
    errs = check()
    for e in errs:
        print("FAIL:", e)
    print(f"sound_classes: {len(sc.CLASSES)} labels, {'MISMATCH' if errs else 'header and python agree'}")
    sys.exit(1 if errs else 0)
