"""
End-to-end pipeline test and the dashboard's Tests page API.

The pipeline suite plays the simulator's lora, normal and loss scenarios through the same parser the
live serial port uses, then checks the database. This file runs it through the REST API exactly as
the Tests page does, and then checks that the dashboard's own endpoints show the result.

Run:      cd backend && pytest -q tests/test_pipeline_and_runner.py
"""
import importlib
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "backend"))
sys.path.insert(0, str(ROOT / "tools"))

from fastapi.testclient import TestClient  # noqa: E402


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("SMARTRELAY_DB", str(tmp_path / "t.db"))
    monkeypatch.setenv("SMARTRELAY_CAPTURES_DIR", str(tmp_path / "rx"))
    (tmp_path / "rx").mkdir()
    import app as app_module
    app_module = importlib.reload(app_module)
    app_module.BRIDGE_PORT = None  # never open a real serial port from a test
    with TestClient(app_module.app) as c:  # keeps the event loop alive for the background run
        yield c
    app_module.store.close()


def wait_for(client, run_id, timeout_s=60.0):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        view = client.get(f"/api/tests/runs/{run_id}").json()
        if view["status"] != "running":
            return view
        time.sleep(0.2)
    raise AssertionError("test run did not finish")


def test_suites_are_listed(client):
    suites = {s["id"]: s for s in client.get("/api/tests/suites").json()["suites"]}
    assert set(suites) == {"pipeline", "backend", "host", "style", "all"}
    assert suites["pipeline"]["available"] is True
    assert all(s["commands"] for s in suites.values())


def test_pipeline_end_to_end(client):
    run = client.post("/api/tests/run/pipeline").json()
    view = wait_for(client, run["id"])
    failed = [c for c in view["checks"] if not c["passed"]]
    assert view["status"] == "passed", failed or view["summary"]
    assert len(view["checks"]) == 8

    # what the dashboard shows: three simulated captures, none counted as real
    sim = client.get("/api/captures", params={"mode": "sim", "limit": 10}).json()
    assert sim["total"] == 3
    assert all(c["is_demo"] == 1 for c in sim["captures"])
    assert client.get("/api/captures", params={"mode": "real"}).json()["total"] == 0
    radios = sorted(t["radio"] for c in sim["captures"] for t in c["files"])
    assert radios == ["lora", "wifi", "wifi"]

    # the run is remembered as the suite's last result
    suites = {s["id"]: s for s in client.get("/api/tests/suites").json()["suites"]}
    assert suites["pipeline"]["last"]["status"] == "passed"


def test_one_run_at_a_time(client):
    first = client.post("/api/tests/run/pipeline")
    assert first.status_code == 200
    second = client.post("/api/tests/run/backend")
    assert second.status_code == 409
    wait_for(client, first.json()["id"])


def test_unknown_suite_is_refused(client):
    assert client.post("/api/tests/run/rm-rf").status_code == 404


def test_other_websites_cannot_start_a_run(client):
    r = client.post("/api/tests/run/pipeline", headers={"Origin": "http://example.com"})
    assert r.status_code == 403
    ok = client.post("/api/tests/run/pipeline", headers={"Origin": "http://localhost:5173"})
    assert ok.status_code == 200
    wait_for(client, ok.json()["id"])
