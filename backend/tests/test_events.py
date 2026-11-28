"""
docs/PROTOCOLS.md: the bridge's "@EVT" lines -> transfers / fragments tables and the
WebSocket events the dashboard uses. Everything goes through the real parser (LiveBridge.feed_line),
exactly as the serial reader and /api/demo/ingest do.

Run:      cd backend && pytest -q
"""
import base64
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "backend"))
sys.path.insert(0, str(ROOT / "tools"))
from bridge_receiver import TELEMETRY_MARKER, decode_meta  # noqa: E402
from live_bridge import BridgeClock, LiveBridge, parse_event  # noqa: E402
from storage import Store  # noqa: E402
import transfers  # noqa: E402
from transfers import TransferTracker  # noqa: E402

V1_FMT = "<BBIIBhHHBBBBBHHB"


def meta_v1(node=5, cap=42, name=b"dog_01.jpg"):
    return struct.pack(V1_FMT, 1, node, cap, 1234, 3, 297, 815, 1000, 1, 0xFF, 0xFF, 20, 82, 345, 2100, 1) + name


def meta_v2(node=5, cap=42, queued=2600, name=b"dog_01.jpg"):
    rec = bytearray(meta_v1(node, cap, b""))
    rec[0] = 2
    return bytes(rec) + struct.pack("<I", queued) + name


class FakeClock:
    def __init__(self, t=1_000_000.0):
        self.t = t

    def __call__(self):
        return self.t


class Rig:
    """Store + tracker + LiveBridge wired like backend/app.py, recording what would go to the dashboards."""

    def __init__(self, tmp_path):
        self.store = Store(str(tmp_path / "t.db"), str(tmp_path / "rx"))
        self.now = FakeClock()
        self.events = []
        self.log = []
        self.tracker = TransferTracker(self.store, emit=lambda e, d: self.events.append((e, d)),
                                       log_line=self.log.append, now=self.now)
        self.saved = []

        def on_transfer(x):
            row_id = self.store.record_capture(node_id=x.src, session_id=x.session_id, radio=x.radio,
                                               frag_total=x.frag_total, size_bytes=x.size, crc32=x.crc32,
                                               image_path=None, meta=x.meta, is_demo=x.is_demo)
            self.saved.append(row_id)
            self.tracker.on_file_saved(x.hdr, x.is_demo, row_id)

        self.bridge = LiveBridge(on_transfer=on_transfer, on_line=self.log.append, on_event=self.tracker.on_event,
                                 on_upload=self.tracker.on_upload)

    def feed(self, *lines, dt=0.0):
        for line in lines:
            self.now.t += dt
            self.bridge.feed_line(line)

    def kinds(self):
        return [e for e, _ in self.events]


def file_lines(src, sid, data, meta, radio="wifi", extra=""):
    crc = zlib.crc32(data) & 0xFFFFFFFF
    out = [f"@ALFP FILE v1 src={src} sid={sid:05d} size={len(data)} crc={crc:08x} radio={radio} "
           f"frags={(len(data) + 237) // 238}{extra} meta={meta.hex()}"]
    out += ["@D " + base64.b64encode(data[i:i + 48]).decode() for i in range(0, len(data), 48)]
    out.append(f"@ALFP END crc={crc:08x}")
    return out


# ---- parsing --------------------------------------------------------------------------------------

def test_parse_every_kind():
    lines = {
        "RXS": "@EVT RXS src=5 sid=42 radio=wifi t=1234567",
        "RXM": "@EVT RXM src=5 sid=42 frags=40 fsz=238 size=9412 crc=1a2b3c4d meta=0205 t=1234570",
        "RXF": "@EVT RXF src=5 sid=42 seq=17 r=0 h=2 v=3 d=0 t=1234890",
        "NACK": "@EVT NACK src=5 sid=42 r=1 miss=3 t=1240100",
        "RXOK": "@EVT RXOK src=5 sid=42 first=1234567 done=1241670 nacks=1 dups=2 frags=40 t=1241670",
        "RXFAIL": "@EVT RXFAIL src=5 sid=42 why=expired t=1300000",
    }
    for kind, line in lines.items():
        k, f = parse_event(line)
        assert k == kind and f["src"] == 5 and f["sid"] == 42 and isinstance(f["t"], int)
    assert parse_event(lines["RXS"])[1]["radio"] == "wifi"
    rxm = parse_event(lines["RXM"])[1]
    assert rxm["crc"] == "1a2b3c4d" and rxm["meta"] == "0205" and rxm["frags"] == 40  # hex stays text
    rxf = parse_event(lines["RXF"])[1]
    assert (rxf["seq"], rxf["r"], rxf["h"], rxf["v"], rxf["d"]) == (17, 0, 2, 3, 0)
    assert parse_event(lines["RXFAIL"])[1]["why"] == "expired"
    assert parse_event("@EVT") is None and parse_event("rx from node 5") is None


def test_event_lines_do_not_reach_the_log(tmp_path):
    rig = Rig(tmp_path)
    rig.feed("@EVT RXS src=5 sid=1 radio=wifi t=10", "@EVT RXF src=5 sid=1 seq=0 r=0 h=1 v=5 d=0 t=12",
             "ordinary log line")
    assert rig.log == ["ordinary log line"]


def test_event_between_data_lines_keeps_the_file_intact(tmp_path):
    rig = Rig(tmp_path)
    data = bytes(range(256)) * 3
    lines = file_lines(5, 9, data, meta_v2())
    lines.insert(3, "@EVT RXS src=4 sid=77 radio=lora t=500")  # another transfer starts mid-upload
    rig.feed(*lines)
    assert len(rig.saved) == 1
    assert "transfer_start" in rig.kinds()


# ---- clock ------------------------------------------------------------------------------------------

def test_clock_uses_the_smallest_offset_and_resets_on_reboot():
    c = BridgeClock()
    c.update(10_000, 1000.050)   # 50 ms USB delay
    c.update(11_000, 1001.010)   # 10 ms: better
    c.update(12_000, 1002.200)   # 200 ms: ignored
    assert abs(c.offset - (1001.010 - 11.0)) < 1e-9
    assert abs(c.to_laptop(20_000) - (20.0 + 990.010)) < 1e-9
    assert c.update(11_500, 1003.0) is False  # slightly out of order: not a reboot
    assert c.update(300, 2000.020) is True    # clock went back: the bridge rebooted
    assert abs(c.offset - (2000.020 - 0.3)) < 1e-9 and c.reboots == 1
    assert BridgeClock().to_laptop(5) is None


# ---- a full transfer --------------------------------------------------------------------------------

def test_full_transfer_with_nack(tmp_path):
    rig = Rig(tmp_path)
    m = meta_v2(queued=2600)
    data = bytes((i * 7) % 256 for i in range(4 * 238))
    rig.feed("@EVT RXS src=5 sid=42 radio=wifi t=100000",
             f"@EVT RXM src=5 sid=42 frags=4 fsz=238 size={len(data)} crc=00000000 meta={m.hex()} t=100010",
             "@EVT RXF src=5 sid=42 seq=0 r=0 h=2 v=3 d=0 t=100020",
             "@EVT RXF src=5 sid=42 seq=1 r=0 h=2 v=4 d=0 t=100030",
             "@EVT RXF src=5 sid=42 seq=3 r=0 h=2 v=3 d=0 t=100040",
             "@EVT NACK src=5 sid=42 r=1 miss=1 t=100300",
             "@EVT RXF src=5 sid=42 seq=2 r=1 h=2 v=4 d=0 t=100400",
             "@EVT RXF src=5 sid=42 seq=2 r=1 h=2 v=3 d=1 t=100410",
             "@EVT RXOK src=5 sid=42 first=100000 done=100500 nacks=1 dups=1 frags=4 t=100500", dt=0.01)
    t = rig.store.find_transfer(5, 42, False)
    assert t["status"] == "verified" and t["crc_bridge_ok"] == 1
    assert (t["radio"], t["frag_count"], t["frag_size"], t["size_bytes"]) == ("wifi", 4, 238, len(data))
    assert (t["kind"], t["capture_id"], t["queued_ms"]) == ("image", 42, 2600)
    assert (t["nacks"], t["dups"], t["rounds"], t["hops_min"], t["hops_max"]) == (1, 1, 2, 2, 2)
    assert t["first_ms"] == 100000 and t["done_ms"] == 100500
    assert t["first_at"] is not None and abs(t["verified_at"] - t["first_at"] - 0.5) < 1e-6
    frags = {f["seq"]: f for f in rig.store.fragments_for(t["id"])}
    assert frags[2]["round"] == 1 and frags[2]["via"] == 4 and frags[2]["dup_count"] == 1  # first copy kept
    assert rig.store.fragment_summary(t["id"])["via_counts"] == {"3": 2, "4": 2}
    assert rig.kinds()[0] == "transfer_start" and rig.kinds()[-1] == "transfer_done"
    assert any("NACK 1" in line for line in rig.log)

    # then the upload to the laptop: links the capture and records the laptop's CRC check
    rig.feed(*file_lines(5, 42, data, m, extra=" first=100000 done=100500 nacks=1 dups=1"), dt=0.05)
    t = rig.tracker.transfer_view(t["id"], with_fragments=True)
    assert t["crc_laptop_ok"] == 1 and t["capture_row_id"] == rig.saved[0]
    assert t["upload_start_at"] is not None and t["upload_s"] > 0 and t["radio_s"] == 0.5
    assert [f["t_rel_s"] for f in t["fragments"]] == [0.02, 0.03, 0.4, 0.04]
    assert rig.store.transfers_for_capture(rig.saved[0])[0]["id"] == t["id"]
    assert rig.store.list_transfers(mode="real")[0]["id"] == t["id"]
    assert rig.store.list_transfers(mode="sim") == []


def test_progress_is_throttled(tmp_path):
    rig = Rig(tmp_path)
    rig.feed("@EVT RXS src=5 sid=7 radio=wifi t=1000",
             "@EVT RXM src=5 sid=7 frags=40 fsz=238 size=9520 crc=0 meta= t=1001")
    for seq in range(39):
        rig.feed(f"@EVT RXF src=5 sid=7 seq={seq} r=0 h=2 v=3 d=0 t={1010 + seq}", dt=0.02)  # 50 per second
    progress = [d for e, d in rig.events if e == "transfer_progress"]
    assert 3 <= len(progress) <= 6  # ~0.8 s of fragments at <= 4 per second (+ the RXM one)
    rig.feed("@EVT RXF src=5 sid=7 seq=39 r=0 h=2 v=4 d=0 t=1050", dt=0.001)
    last = [d for e, d in rig.events if e == "transfer_progress"][-1]
    assert last["have"] == 40 and last["frag_count"] == 40 and last["last_via"] == 4  # the last one always shows
    assert last["via_counts"] == {"3": 39, "4": 1} and last["kind"] == "test"


def test_fragments_before_manifest(tmp_path):
    rig = Rig(tmp_path)
    m = meta_v2()
    rig.feed("@EVT RXS src=5 sid=8 radio=wifi t=1000",
             "@EVT RXF src=5 sid=8 seq=1 r=0 h=2 v=3 d=0 t=1010",
             "@EVT RXF src=5 sid=8 seq=2 r=0 h=2 v=3 d=0 t=1020",
             f"@EVT RXM src=5 sid=8 frags=3 fsz=238 size=700 crc=0 meta={m.hex()} t=1030",
             "@EVT RXF src=5 sid=8 seq=0 r=0 h=2 v=3 d=0 t=1040")
    rows = rig.store.list_transfers()
    assert len(rows) == 1 and rows[0]["frag_count"] == 3 and rows[0]["capture_id"] == 42
    assert rig.store.fragment_summary(rows[0]["id"])["have"] == 3


def test_file_without_rxs_still_creates_a_transfer(tmp_path):
    rig = Rig(tmp_path)  # old firmware: no @EVT at all, no first= / done= in the header
    data = b"x" * 500
    rig.feed(*file_lines(3, 11, data, meta_v1(node=3, cap=9)))
    rows = rig.store.list_transfers()
    assert len(rows) == 1
    t = rig.tracker.transfer_view(rows[0]["id"])
    assert t["status"] == "verified" and t["crc_laptop_ok"] == 1 and t["capture_row_id"] == rig.saved[0]
    assert t["first_ms"] is None and t["first_at"] is None and t["radio_s"] is None  # shown as unavailable
    assert t["queued_ms"] is None and t["capture_id"] == 9 and t["kind"] == "image"


def test_lora_relay_unknown_and_telemetry_kind(tmp_path):
    rig = Rig(tmp_path)
    m = meta_v2(name=TELEMETRY_MARKER.encode())
    rig.feed("@EVT RXS src=5 sid=3 radio=lora t=1000",
             f"@EVT RXM src=5 sid=3 frags=1 fsz=241 size=31 crc=0 meta={m.hex()} t=1001",
             "@EVT RXF src=5 sid=3 seq=0 r=0 h=3 v=0 d=0 t=1300")
    t = rig.store.find_transfer(5, 3, False)
    assert t["kind"] == "telemetry" and t["hops_max"] == 3
    assert rig.store.fragments_for(t["id"])[0]["via"] is None  # unknown, never guessed
    assert rig.store.fragment_summary(t["id"])["via_counts"] == {"0": 1}


# ---- failures ---------------------------------------------------------------------------------------

def test_rxfail_marks_failed_and_crc_is_not_final(tmp_path):
    rig = Rig(tmp_path)
    rig.feed("@EVT RXS src=5 sid=4 radio=wifi t=1000", "@EVT RXF src=5 sid=4 seq=0 r=0 h=1 v=5 d=0 t=1001",
             "@EVT RXFAIL src=5 sid=4 why=crc t=1100")
    t = rig.store.find_transfer(5, 4, False)
    assert t["status"] == "in_progress" and rig.store.fragments_for(t["id"]) == []
    rig.feed("@EVT RXFAIL src=5 sid=4 why=expired t=70000")
    t = rig.store.get_transfer(t["id"])
    assert t["status"] == "failed" and t["fail_reason"] == "expired"
    assert ("transfer_failed", {"transfer_id": t["id"], "reason": "expired", "node_id": 5, "sid": 4,
                                "is_demo": False}) in rig.events
    assert any("expired" in line for line in rig.log)


def test_refusals_without_a_session_make_one_row(tmp_path):
    rig = Rig(tmp_path)
    for i in range(5):  # the sender retries every poll; the bridge refuses every time
        rig.feed(f"@EVT RXFAIL src=4 sid=9 why=busy t={1000 + i}", dt=1.0)
    rows = rig.store.list_transfers()
    assert len(rows) == 1 and rows[0]["status"] == "failed" and rows[0]["fail_reason"] == "busy"


def test_quiet_transfer_times_out(tmp_path):
    rig = Rig(tmp_path)
    rig.feed("@EVT RXS src=5 sid=5 radio=wifi t=1000")
    assert rig.tracker.sweep() == 0
    rig.now.t += transfers.TIMEOUT_S + 1
    assert rig.tracker.sweep() == 1
    t = rig.store.find_transfer(5, 5, False)
    assert t["status"] == "failed" and t["fail_reason"] == "timeout"
    assert rig.tracker.in_flight() == []


def test_rxs_again_closes_the_old_session(tmp_path):
    rig = Rig(tmp_path)
    rig.feed("@EVT RXS src=5 sid=6 radio=wifi t=1000", "@EVT RXS src=5 sid=6 radio=wifi t=90000")
    rows = rig.store.list_transfers()
    assert [r["status"] for r in rows] == ["in_progress", "failed"] and rows[1]["fail_reason"] == "restarted"


def test_demo_rows_are_tagged(tmp_path):
    rig = Rig(tmp_path)
    rig.bridge.feed_line("@EVT RXS src=5 sid=1 radio=wifi t=10", is_demo=True)
    assert rig.store.list_transfers(mode="sim")[0]["is_demo"] == 1
    assert rig.store.list_transfers(mode="real") == []
    assert rig.events[0][1]["is_demo"] is True


# ---- capture metadata v2 ------------------------------------------------------------------------------

def test_decode_meta_v1_and_v2():
    v2 = decode_meta(meta_v2(queued=2600))
    assert v2["version"] == 2 and v2["queued_ms"] == 2600 and v2["original_name"] == "dog_01.jpg"
    assert v2["capture_id"] == 42 and v2["temperature_c"] == 29.7
    missing = decode_meta(meta_v2(queued=0xFFFFFFFF, name=b""))
    assert missing["queued_ms"] is None and missing["original_name"] is None
    v1 = decode_meta(meta_v1())
    assert v1["version"] == 1 and v1["queued_ms"] is None and v1["original_name"] == "dog_01.jpg"
    assert decode_meta(meta_v2()[:30]) == {"raw_hex": meta_v2()[:30].hex()}  # v2 cut short
    assert "raw_hex" in decode_meta(b"\x03" + bytes(40))  # unknown version
