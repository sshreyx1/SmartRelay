"""
Task E: tools/verify_received.py's logic, proven against a synthetic fixture (manifest + fake
received files + a Store with a couple of record_capture() calls) -- the only way to exercise it
in this environment, since there is no real two-node hardware data to run it against. This does
NOT prove the two-node hardware procedure (docs/TESTING.md) works; it only proves the
verifier script itself does the comparisons it claims to.

Run:      cd backend && pytest -q
"""
import csv
import hashlib
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "backend"))
sys.path.insert(0, str(ROOT / "tools"))
from bridge_receiver import TELEMETRY_MARKER, decode_meta  # noqa: E402
from storage import Store  # noqa: E402
from verify_received import check_capture_linkage, check_manifest  # noqa: E402


def meta(node=2, cap=1, name=b"dog_01.jpg"):
    return struct.pack("<BBIIBhHHBBBBBHHB", 1, node, cap, 1234, 3, 287, 815, 1000, 1, 0xFF, 0xFF, 20, 82, 345,
                       2100, 1) + name


def write_and_hash(path: Path, data: bytes) -> str:
    path.write_bytes(data)
    return hashlib.sha256(data).hexdigest()


def test_manifest_check_finds_present_mismatched_and_missing(tmp_path):
    received = tmp_path / "received"
    received.mkdir()

    good_data = b"A" * 1024
    good_hash = write_and_hash(received / "n2_00001.bin", good_data)

    manifest_rows = [
        {"filename": "p_0001024.jpg", "size_bytes": "1024", "sha256": good_hash},
        {"filename": "p_0004096.jpg", "size_bytes": "4096", "sha256": hashlib.sha256(b"wrong content").hexdigest()},
        {"filename": "p_0016384.jpg", "size_bytes": "16384", "sha256": hashlib.sha256(b"never arrived").hexdigest()},
    ]

    rows, any_fail = check_manifest(manifest_rows, str(received), conn=None)
    by_name = {r["filename"]: r for r in rows}
    assert by_name["p_0001024.jpg"]["present"] and by_name["p_0001024.jpg"]["size_match"]
    assert not by_name["p_0004096.jpg"]["present"]  # wrong content -> not found, correctly
    assert not by_name["p_0016384.jpg"]["present"]
    assert any_fail is True


def test_manifest_check_all_present_is_clean(tmp_path):
    received = tmp_path / "received"
    received.mkdir()
    data = b"B" * 2000
    h = write_and_hash(received / "n2_00002.bin", data)
    rows, any_fail = check_manifest([{"filename": "p_0002000.jpg", "size_bytes": "2000", "sha256": h}],
                                    str(received), conn=None)
    assert any_fail is False
    assert rows[0]["present"] and rows[0]["size_match"] and rows[0]["hash_match"]


def test_capture_linkage_flags_telemetry_only_row(tmp_path):
    s = Store(str(tmp_path / "t.db"), str(tmp_path / "rx"))
    m1 = decode_meta(meta(cap=1))
    s.record_capture(node_id=2, session_id=1, radio="wifi", frag_total=1, size_bytes=1024, crc32="00",
                     image_path=str(tmp_path / "n2_00001.bin"), meta=m1, is_demo=False)
    m2 = decode_meta(meta(cap=2, name=TELEMETRY_MARKER.encode()))
    m2 = {**m2, "original_name": None}
    s.record_capture(node_id=2, session_id=2, radio="lora", frag_total=1, size_bytes=27, crc32="00",
                     image_path=None, meta=m2, is_demo=False)

    rows = check_capture_linkage(s.conn)
    by_cap = {r["capture_id"]: r for r in rows}
    assert by_cap[1]["linked"] is True
    assert by_cap[2]["linked"] is False and by_cap[2]["has_telemetry"] and not by_cap[2]["has_image"]
