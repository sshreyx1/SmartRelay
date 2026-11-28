"""
Dashboard v2 API: filters, export, summary, insights and the per-file transfers table.

Every test runs the real app against a throwaway database (SMARTRELAY_DB) and feeds it the exact
serial text the bridge prints, through the same parser the live serial port uses.

Run:      cd backend && pytest -q
"""
import base64
import csv
import importlib
import io
import struct
import sys
import zlib
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "backend"))
sys.path.insert(0, str(ROOT / "tools"))

from fastapi.testclient import TestClient  # noqa: E402


@pytest.fixture()
def env(tmp_path, monkeypatch):
    monkeypatch.setenv("SMARTRELAY_DB", str(tmp_path / "t.db"))
    monkeypatch.setenv("SMARTRELAY_CAPTURES_DIR", str(tmp_path / "rx"))
    monkeypatch.setenv("SMARTRELAY_BRIDGE_PORT", "")
    (tmp_path / "rx").mkdir()
    import app as app_module
    app_module = importlib.reload(app_module)
    yield app_module, TestClient(app_module.app)
    app_module.store.close()


def meta(node, cap, trig=3, name=b"dog_01.jpg", sound=20):
    return struct.pack("<BBIIBhHHBBBBBHHB", 1, node, cap, 1234, trig, 297, 815, 1000, 1, 0xFF, 0xFF, sound, 82,
                       345, 2100, 1) + name


JPEG = b"\xff\xd8\xff" + bytes(range(256)) * 8


def file_lines(node, sid, data, m=b"", radio="wifi"):
    crc = zlib.crc32(data) & 0xFFFFFFFF
    lines = [f"@ALFP FILE v1 src={node} sid={sid:05d} size={len(data)} crc={crc:08x} radio={radio} "
             f"frags={max(1, len(data) // 238 + 1)} meta={m.hex()}"]
    lines += ["@D " + base64.b64encode(data[i:i + 48]).decode() for i in range(0, len(data), 48)]
    lines.append(f"@ALFP END crc={crc:08x}")
    return lines


def capture_lines(node, cap, sid, trig=3, radio="wifi"):
    """One capture the way APP_SPLIT_TELEMETRY=1 firmware sends it: telemetry over LoRa, then the image."""
    return (file_lines(node, sid, b"t" * 27, meta(node, cap, trig, b"TELEMETRY"), radio="lora")
            + file_lines(node, sid + 1, JPEG, meta(node, cap, trig), radio=radio))


def feed_real(app_module, lines):
    for line in lines:
        app_module.bridge.feed_line(line, is_demo=False)


def test_old_firmware_upload_makes_one_linked_verified_transfer(env):
    """No @EVT lines (bridge not reflashed): the @ALFP FILE upload alone still gives a verified transfer."""
    app_module, client = env
    feed_real(app_module, file_lines(5, 11, JPEG, meta(5, 42)))
    caps = client.get("/api/captures").json()["captures"]
    assert len(caps) == 1
    (f,) = caps[0]["files"]
    assert f["kind"] == "image" and f["status"] == "verified" and f["crc_laptop_ok"] == 1
    assert f["upload_s"] is not None and f["radio_s"] is None  # radio time needs RXOK


def evt_transfer(node, sid, data, m, radio="wifi", relays=(3, 4), lost=(2,), t0=1000):
    """The bridge's @EVT lines for one file, a NACK round for `lost`, then the upload."""
    crc = zlib.crc32(data) & 0xFFFFFFFF
    n = max(1, len(data) // 236 + 1)
    lines = [f"@EVT RXS src={node} sid={sid} radio={radio} t={t0}",
             f"@EVT RXM src={node} sid={sid} frags={n} fsz=236 size={len(data)} crc={crc:08x} meta={m.hex()} t={t0 + 5}"]
    t = t0 + 10
    for seq in range(n):
        if seq in lost:
            continue
        lines.append(f"@EVT RXF src={node} sid={sid} seq={seq} r=0 h=2 v={relays[seq % len(relays)]} d=0 t={t}")
        t += 50
    lines.append(f"@EVT NACK src={node} sid={sid} r=1 miss={len(lost)} t={t}")
    for seq in lost:
        t += 50
        lines.append(f"@EVT RXF src={node} sid={sid} seq={seq} r=1 h=2 v={relays[0]} d=0 t={t}")
    lines.append(f"@EVT RXOK src={node} sid={sid} first={t0} done={t} nacks=1 dups=0 frags={n} t={t}")
    return lines + file_lines(node, sid, data, m, radio=radio)


def test_evt_lines_reach_every_dashboard_view(env):
    app_module, client = env
    feed_real(app_module, evt_transfer(5, 50, JPEG, meta(5, 7)))
    (c,) = client.get("/api/captures").json()["captures"]
    (f,) = c["files"]
    assert f["radio_s"] is not None and f["nacks"] == 1 and f["rounds"] == 2 and f["via_counts"]
    detail = client.get(f"/api/captures/{c['id']}").json()
    frags = detail["files"][0]["fragments"]
    assert len(frags) == f["frag_count"] and {x["round"] for x in frags} == {0, 1}
    assert client.get("/api/summary").json()["median_transfer_s"]["wifi"] == f["radio_s"]
    node = client.get("/api/nodes/5").json()
    assert node["median_image_transfer_s"] == f["radio_s"]
    assert node["fragments_resent"]["resent"] == 1
    assert set(node["relay_counts"]["wifi"]) <= {"3", "4"}


def test_both_files_of_a_capture_link_to_one_row(env):
    app_module, client = env
    feed_real(app_module, capture_lines(3, 7, 100))
    caps = client.get("/api/captures").json()["captures"]
    assert len(caps) == 1
    assert sorted(f["kind"] for f in caps[0]["files"]) == ["image", "telemetry"]
    s = client.get("/api/summary").json()
    assert s["real_captures"] == 1 and s["crc_verified_transfers"] == 2
    assert s["by_radio"] == {"lora": 1, "wifi": 1, "unknown": 0}


def test_size_ladder_payload_is_a_transfer_not_a_capture(env):
    app_module, client = env
    feed_real(app_module, file_lines(2, 5, b"x" * 500, m=b""))
    assert client.get("/api/captures").json()["captures"] == []
    assert client.get("/api/summary").json()["crc_verified_transfers"] == 1


def test_laptop_crc_failure_is_recorded_not_stored(env):
    app_module, client = env
    lines = file_lines(4, 9, JPEG, meta(4, 1))
    lines[1] = "@D " + base64.b64encode(b"\x00" * 48).decode()  # corrupt one line
    feed_real(app_module, lines)
    assert client.get("/api/captures").json()["captures"] == []
    s = client.get("/api/summary").json()
    assert s["laptop_crc_failed"] == 1


def test_filters_mode_node_radio_trigger(env):
    app_module, client = env
    feed_real(app_module, capture_lines(2, 1, 10, trig=3))
    feed_real(app_module, capture_lines(3, 1, 20, trig=0))
    feed_real(app_module, capture_lines(4, 1, 30, trig=1, radio="lora"))
    client.post("/api/demo/ingest", json={"lines": file_lines(5, 40, JPEG, meta(5, 1))})

    def n(qs):
        return len(client.get("/api/captures?" + qs).json()["captures"])

    assert n("") == 3                      # default mode=real
    assert n("mode=sim") == 1
    assert n("mode=both") == 4
    assert n("include_demo=true") == 4     # old parameter still works
    assert n("node_id=3") == 1
    assert n("trigger=dual") == 1
    assert n("trigger=manual") == 1
    assert n("trigger=pir") == 1
    assert n("radio=lora") == 3            # every real capture sent its telemetry over LoRa
    assert n("radio=wifi") == 2
    assert n("q=N3-") == 1
    assert n("since=9999999999") == 0


def test_limit_defaults_to_newest_20(env):
    app_module, client = env
    for i in range(25):
        feed_real(app_module, file_lines(2, 100 + i, JPEG, meta(2, i)))
    body = client.get("/api/captures").json()
    assert len(body["captures"]) == 20 and body["total"] == 25
    assert body["captures"][0]["capture_id"] == 24


def test_export_csv_header_and_rows(env):
    app_module, client = env
    feed_real(app_module, capture_lines(2, 1, 10))
    feed_real(app_module, capture_lines(3, 2, 20))
    r = client.get("/api/captures/export?format=csv")
    assert r.status_code == 200
    assert "attachment" in r.headers["content-disposition"]
    rows = list(csv.reader(io.StringIO(r.text)))
    header = rows[0]
    assert header[0] == "id" and "telemetry_radio" in header and "image_radio" in header
    assert len(rows) == 1 + 2
    row = dict(zip(header, rows[1]))
    assert row["telemetry_radio"] == "lora" and row["image_radio"] == "wifi"
    j = client.get("/api/captures/export?format=json&node_id=2").json()
    assert len(j["captures"]) == 1


def test_summary_sim_mode_has_no_reporting_status(env):
    app_module, client = env
    client.post("/api/demo/ingest", json={"lines": file_lines(5, 40, JPEG, meta(5, 1))})
    s = client.get("/api/summary?mode=sim").json()
    assert s["field_nodes_reporting"] is None
    assert s["sim_captures"] == 1 and s["real_captures"] == 0
    real = client.get("/api/summary").json()["field_nodes_reporting"]
    assert real["count"] == 0 and real["of"] == 4  # demo rows never make a node "reporting"


def test_insights_need_ten_captures(env):
    app_module, client = env
    for i in range(3):
        feed_real(app_module, file_lines(2, 10 + i, JPEG, meta(2, i)))
    ins = client.get("/api/summary").json()["insights"]
    assert len(ins) == 1 and ins[0]["text"].startswith("Not enough data")
    for i in range(3, 12):
        feed_real(app_module, file_lines(3 if i % 3 else 2, 10 + i, JPEG, meta(3 if i % 3 else 2, i)))
    ins = client.get("/api/summary").json()["insights"]
    assert 1 <= len(ins) <= 3
    assert all(i["evidence"] for i in ins)
    assert "busiest field node" in ins[0]["text"]


def test_backfill_links_only_exact_matches(tmp_path):
    from storage import Store
    s = Store(str(tmp_path / "b.db"), str(tmp_path / "rx"))
    s.conn.execute("DROP TABLE transfers")
    s.conn.commit()
    s.log_line("rx from node 5: rcv/55/n5_00011.bin, 27 bytes via lora, 0 dup")
    s.log_line("rx from node 5: rcv/55/n5_00012.bin, 9000 bytes via wifi, 2 dup")
    s.log_line("[ENV] DHT22: no reading")
    s.record_capture(node_id=5, session_id=12, radio="wifi", frag_total=38, size_bytes=9000, crc32="0",
                     image_path=str(tmp_path / "rx" / "n5_00012.jpg"), meta={"capture_id": 1})
    s.close()
    s = Store(str(tmp_path / "b.db"), str(tmp_path / "rx"))
    rows = [dict(r) for r in s.conn.execute("SELECT * FROM transfers ORDER BY sid")]
    assert [r["sid"] for r in rows] == [11, 12]
    assert rows[0]["kind"] is None and rows[0]["capture_row_id"] is None   # no exact link: not guessed
    assert rows[1]["kind"] == "image" and rows[1]["dups"] == 2 and rows[1]["capture_row_id"] is not None
    assert all(r["status"] == "verified" and r["crc_bridge_ok"] == 1 for r in rows)


def test_node_and_bridge_pages(env):
    app_module, client = env
    feed_real(app_module, capture_lines(3, 1, 10))
    d = client.get("/api/nodes/3").json()
    assert d["captures_count"] == 1 and d["files"]["files"] == 2
    assert d["median_image_transfer_s"] is None and d["relay_counts"] is None  # not on the wire
    b = client.get("/api/nodes/1").json()
    assert b["role"] == "bridge" and b["transfers"]["verified"] == 2
    assert client.get("/api/nodes/9").status_code == 404


def test_log_newest_first_with_load_older_and_filters(env):
    app_module, client = env
    for i in range(30):
        app_module.on_line(f"line {i}")
    app_module.on_line("uplink: queue full, failed")
    app_module.on_line("@EVT transfer something")
    page = client.get("/api/log").json()["lines"]
    assert len(page) == 20 and page[0]["line"] == "uplink: queue full, failed"
    older = client.get(f"/api/log?before_id={page[-1]['id']}").json()["lines"]
    assert older[0]["id"] < page[-1]["id"]
    assert [x["line"] for x in client.get("/api/log?kind=fail").json()["lines"]] == ["uplink: queue full, failed"]
    assert client.get("/api/log?show_evt=true").json()["lines"][0]["line"].startswith("@EVT")
