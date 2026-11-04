"""
Task A.2: capture metadata and the image now travel as two separate ALFP transfers (so the size
rule can route the tiny one over LoRa and the image over WiFi), linked by capture_id. Either can
arrive first, or the image can be missing entirely (WiFi down). record_capture()'s upsert must
merge the two halves into one row without ever erasing an image (or its name) that a previous
transfer for the same capture_id already stored.

Run:      cd backend && pytest -q
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "backend"))
sys.path.insert(0, str(ROOT / "tools"))
from bridge_receiver import TELEMETRY_MARKER, decode_meta  # noqa: E402
from storage import Store  # noqa: E402


def meta(node=2, cap=42, temp=297, name=b"dog_01.jpg"):
    return struct.pack("<BBIIBhHHBBBBBHHB", 1, node, cap, 1234, 3, temp, 815, 1000, 1, 0xFF, 0xFF, 20, 82, 345,
                       2100, 1) + name


def telemetry_meta(**kw):
    return meta(name=TELEMETRY_MARKER.encode(), **kw)


def store(tmp_path):
    return Store(str(tmp_path / "t.db"), str(tmp_path / "rx"))


def record(s, m, image_path, sid, radio="lora"):
    """Mirrors backend/app.py's on_transfer(): a telemetry-only transfer's decoded
    original_name is the literal marker, not a real file name, so it must not reach storage.py
    as image_name -- on_transfer() clears it before calling record_capture(), same as here."""
    if image_path is None:
        m = {**m, "original_name": None}
    return s.record_capture(node_id=m["node_id"], session_id=sid, radio=radio, frag_total=1, size_bytes=27,
                            crc32="00", image_path=image_path, meta=m, is_demo=False)


def test_telemetry_marker_is_not_a_real_file_name():
    m = decode_meta(telemetry_meta())
    assert m["original_name"] == TELEMETRY_MARKER  # decode_meta just decodes; app.py tells them apart
    assert m["temperature_c"] == 29.7 and m["capture_id"] == 42


def test_telemetry_then_image_merge_into_one_row(tmp_path):
    s = store(tmp_path)
    tel_id = record(s, decode_meta(telemetry_meta()), image_path=None, sid=1, radio="lora")
    row = s.get_capture(tel_id)
    assert row["image_path"] is None and row["temperature_c"] == 29.7 and row["radio"] == "lora"

    img_id = record(s, decode_meta(meta()), image_path="rx/n2_00002.jpg", sid=2, radio="wifi")
    assert img_id == tel_id  # same (node_id, capture_id): one row, not two
    row = s.get_capture(img_id)
    assert row["image_path"] == "rx/n2_00002.jpg" and row["image_name"] == "dog_01.jpg"
    assert row["radio"] == "wifi"  # now reflects the most recently arrived transfer
    assert s.stats()["real_captures"] == 1


def test_image_then_telemetry_does_not_erase_the_image(tmp_path):
    """Out-of-order arrival: the image's transfer completes first, telemetry's duplicate copy of
    the same meta arrives after. The already-stored image must survive (image_path is COALESCEd,
    never blindly overwritten with the telemetry call's None)."""
    s = store(tmp_path)
    img_id = record(s, decode_meta(meta()), image_path="rx/n2_00001.jpg", sid=1, radio="wifi")
    tel_id = record(s, decode_meta(telemetry_meta()), image_path=None, sid=2, radio="lora")
    assert tel_id == img_id
    row = s.get_capture(img_id)
    assert row["image_path"] == "rx/n2_00001.jpg" and row["image_name"] == "dog_01.jpg"


def test_telemetry_only_never_gets_an_image(tmp_path):
    """WiFi down: telemetry arrives, the image never does. The row must stay queryable with a
    null image, never a fabricated path."""
    s = store(tmp_path)
    tel_id = record(s, decode_meta(telemetry_meta()), image_path=None, sid=1, radio="lora")
    row = s.get_capture(tel_id)
    assert row["image_path"] is None and row["capture_id"] == 42 and row["temperature_c"] == 29.7


def test_duplicate_telemetry_is_idempotent(tmp_path):
    s = store(tmp_path)
    a = record(s, decode_meta(telemetry_meta()), image_path=None, sid=1)
    b = record(s, decode_meta(telemetry_meta()), image_path=None, sid=2)  # resent after a reboot, say
    assert a == b
    assert s.stats()["real_captures"] == 1
