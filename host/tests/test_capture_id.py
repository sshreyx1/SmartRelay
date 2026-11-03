#!/usr/bin/env python3
"""
test_capture_id.py - common/capture_id.py must produce and parse exactly what capture_id.h does
(the same cases as host/tests/test_capture_id.c). Run by ctest (capture_id_python) and pytest.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "common"))
import capture_id as cid  # noqa: E402


def check() -> list:
    errs = []

    def eq(got, want):
        if got != want:
            errs.append(f"got {got!r}, want {want!r}")

    eq(cid.display_id(2, 42), "N2-000042")
    eq(cid.file_stem(2, 42), "N2_000042")
    eq(cid.display_id(255, 4294967295), "N255-4294967295")
    eq(cid.display_id(5, 1234567), "N5-1234567")
    for i in (0, 1, 42, 999999, 1000000, 4294967295):
        for node in range(0, 256, 51):
            eq(cid.parse(cid.display_id(node, i)), (node, i))
            eq(cid.parse(cid.file_stem(node, i)), (node, i))
    eq(cid.parse("N2_000042_full.scl"), (2, 42))
    eq(cid.parse("n3-000007"), (3, 7))
    for bad in ("N2-42", "N256-000001", "img_00.jpg", "N-000001", ""):
        eq(cid.parse(bad), None)
    return errs


def test_capture_id_python():
    assert check() == []


if __name__ == "__main__":
    e = check()
    for x in e:
        print("FAIL:", x)
    print("capture_id.py:", "FAILED" if e else "all tests passed")
    sys.exit(1 if e else 0)
