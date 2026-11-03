"""
Backend side of the on-node sound labels: decode_meta names the label the node sent (it never
classifies), the store keeps one row per real capture, and /api/species is a plain tally.

Run:      cd backend && pytest -q
"""
import sqlite3
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "backend"))
sys.path.insert(0, str(ROOT / "tools"))
from bridge_receiver import decode_meta  # noqa: E402
from storage import Store  # noqa: E402


def meta(node=2, cap=42, sound_class=20, sound_conf=82, name=b"dog_01.jpg"):
    return struct.pack("<BBIIBhHHBBBBBHHB", 1, node, cap, 1234, 3, 287, 815, 1000, 1, 0xFF, 0xFF,
                       sound_class, sound_conf, 345, 2100, 1) + name


def test_decode_names_the_nodes_label():
    m = decode_meta(meta())
    assert m["sound_class"] == 20 and m["sound_label"] == "DOG" and m["sound_name"] == "DOG"
    assert m["sound_group"] == "MAMMAL" and m["sound_kind"] == "animal" and m["sound_confidence"] == 82
    assert m["display_id"] == "N2-000042" and m["original_name"] == "dog_01.jpg"


def test_decode_group_only_and_unknown():
    g = decode_meta(meta(sound_class=1, sound_conf=55, name=b""))
    assert g["sound_label"] == "BIRD" and g["sound_kind"] == "group" and g["original_name"] is None
    u = decode_meta(meta(sound_class=0xFF, sound_conf=0xFF))
    assert u["sound_class"] is None and u["sound_label"] is None and u["sound_group"] is None
    assert u["sound_confidence"] is None  # unknown stays unknown, never a fake 0


def store(tmp_path):
    return Store(str(tmp_path / "t.db"), str(tmp_path / "rx"))


def record(s, m, demo=False, sid=1):
    return s.record_capture(node_id=m["node_id"], session_id=sid, radio="wifi", frag_total=3, size_bytes=900,
                            crc32="00", image_path="x.jpg", meta=m, is_demo=demo)


def test_store_columns_and_one_row_per_real_capture(tmp_path):
    s = store(tmp_path)
    a = record(s, decode_meta(meta()))
    b = record(s, decode_meta(meta()), sid=2)  # same node + capture id delivered again
    assert a == b
    row = s.get_capture(a)
    assert row["display_id"] == "N2-000042" and row["sound_label"] == "DOG" and row["sound_conf"] == 82
    assert row["sound_group"] == "MAMMAL" and row["image_name"] == "dog_01.jpg" and row["session_id"] == 2
    record(s, decode_meta(meta()), demo=True)
    record(s, decode_meta(meta()), demo=True)  # demo runs restart their counters: both kept
    assert s.stats() == {"real_captures": 1, "demo_captures": 2, "by_radio": {"wifi": 1}}


def test_species_tally(tmp_path):
    s = store(tmp_path)
    record(s, decode_meta(meta(cap=1, sound_class=20, sound_conf=80)))
    record(s, decode_meta(meta(cap=2, sound_class=20, sound_conf=60)))
    record(s, decode_meta(meta(node=3, cap=1, sound_class=1, sound_conf=50)))
    record(s, decode_meta(meta(cap=3, sound_class=0xFF, sound_conf=0xFF)))  # unknown: not tallied
    record(s, decode_meta(meta(cap=4, sound_class=25)), demo=True)
    t = s.species_tally()
    assert [(r["sound_label"], r["count"]) for r in t] == [("DOG", 2), ("BIRD", 1)]
    assert t[0]["sound_group"] == "MAMMAL" and t[0]["mean_conf"] == 70.0 and t[0]["nodes"] == [2]
    assert "FROG" in [r["sound_label"] for r in s.species_tally(include_demo=True)]


def test_old_database_is_migrated(tmp_path):
    db = tmp_path / "t.db"
    con = sqlite3.connect(db)
    con.execute("CREATE TABLE captures (id INTEGER PRIMARY KEY AUTOINCREMENT, node_id INTEGER NOT NULL, "
                "capture_id INTEGER, session_id INTEGER NOT NULL, radio TEXT, frag_total INTEGER, "
                "size_bytes INTEGER, crc32 TEXT, image_path TEXT, received_at REAL NOT NULL, uptime_ms INTEGER, "
                "trigger_pir INTEGER, trigger_sound INTEGER, temperature_c REAL, humidity_pct REAL, "
                "light_raw INTEGER, weather_label INTEGER, weather_name TEXT, trigger_trust INTEGER, "
                "sound_class INTEGER, sound_name TEXT, sound_rms INTEGER, sound_zcr INTEGER, "
                "image_index INTEGER, is_demo INTEGER NOT NULL DEFAULT 0)")
    con.execute("INSERT INTO captures (node_id, capture_id, session_id, received_at) VALUES (2, 5, 1, 0)")
    con.execute("INSERT INTO captures (node_id, capture_id, session_id, received_at) VALUES (2, 5, 2, 0)")
    con.commit()
    con.close()
    s = Store(str(db), str(tmp_path / "rx"))  # duplicates from before: warns, still works
    assert record(s, decode_meta(meta(cap=6))) > 0
    assert s.get_capture(3)["display_id"] == "N2-000006"
