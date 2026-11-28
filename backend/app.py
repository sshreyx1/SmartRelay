"""
smartrelay backend.

Connects to the real bridge node (node 1) over USB serial and reads the
exact text protocol it already streams (common/uplink.h, tools/bridge_receiver.py):

    @ALFP FILE v1 src=3 sid=00042 size=8000 crc=1a2b3c4d radio=lora frags=34 meta=<hex>
    @D <base64>
    ...
    @ALFP END crc=1a2b3c4d

This is automatic: set SMARTRELAY_BRIDGE_PORT (or let it autodetect a Pico)
and every verified capture the bridge reassembles is stored and pushed to
the dashboard live, with no manual step. Nothing here invents sensor
readings, species, or node status beyond what the firmware actually reports
-- see storage.py's node_last_seen() for what "online" really means here
(time since the bridge last reassembled something from that node; the
firmware has no periodic heartbeat to the laptop, so that's the honest
signal, not a fabricated poll).

A SEPARATE, manual-only path exists for demos without hardware:
simulator/demo_feed.py. It is never started by this file. It talks to the
backend over /api/demo/ingest, which runs the exact same parser
(live_bridge.LiveBridge.feed_line) and tags every row is_demo=1, so the
dashboard can always tell real mesh data from a demo run and never present
one as the other.

Run:
  pip install -r requirements.txt
  SMARTRELAY_BRIDGE_PORT=/dev/ttyACM0 uvicorn app:app --host 0.0.0.0 --port 8000
  (omit the port env var to try autodetection; see /api/health for what it found)
"""
from __future__ import annotations

import asyncio
import csv
import io
import json
import logging
import os
import time
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any, Dict, List, Optional, Set

from fastapi import Depends, FastAPI, HTTPException, Query, Request, Response, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, HTMLResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from live_bridge import CompletedTransfer, LiveBridge, autodetect_port
from storage import Filters, Store
from suite_runner import make_runner
from transfers import TransferTracker, kind_of

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)-7s %(name)s: %(message)s")
log = logging.getLogger("smartrelay.api")

BASE_DIR = Path(__file__).parent
DASHBOARD_DIST = BASE_DIR.parent / "dashboard" / "dist"

# SMARTRELAY_DB / SMARTRELAY_CAPTURES_DIR let tests point the app at a throwaway database.
CAPTURES_DIR = Path(os.environ.get("SMARTRELAY_CAPTURES_DIR") or BASE_DIR / "received")
store = Store(db_path=os.environ.get("SMARTRELAY_DB") or str(BASE_DIR / "smartrelay.db"),
              captures_dir=str(CAPTURES_DIR))

BRIDGE_PORT = os.environ.get("SMARTRELAY_BRIDGE_PORT") or autodetect_port()
DEMO_MODE_REQUIRES_TOKEN = os.environ.get("SMARTRELAY_DEMO_TOKEN", "").strip()


# ---------------------------------------------------------------------------
# Live push to dashboards
# ---------------------------------------------------------------------------

class Hub:
    def __init__(self) -> None:
        self.clients: Set[WebSocket] = set()
        self._loop: Optional[asyncio.AbstractEventLoop] = None

    def bind_loop(self, loop: asyncio.AbstractEventLoop) -> None:
        self._loop = loop

    async def connect(self, ws: WebSocket) -> None:
        await ws.accept()
        self.clients.add(ws)

    def disconnect(self, ws: WebSocket) -> None:
        self.clients.discard(ws)

    async def broadcast(self, event: str, data: Any) -> None:
        if not self.clients:
            return
        message = json.dumps({"event": event, "data": data, "ts": time.time()})
        dead = []
        for ws in list(self.clients):
            try:
                await ws.send_text(message)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.disconnect(ws)

    def broadcast_soon(self, event: str, data: Any) -> None:
        if self._loop is None:
            return
        asyncio.run_coroutine_threadsafe(self.broadcast(event, data), self._loop)


hub = Hub()


# ---------------------------------------------------------------------------
# Bridge wiring: every verified transfer -> DB row + WS push
# ---------------------------------------------------------------------------

def with_image_url(row: Dict[str, Any]) -> Dict[str, Any]:
    """Every capture row sent to the dashboard (REST, live push and the WS snapshot) carries image_url."""
    row["image_url"] = "/captures/" + Path(row["image_path"]).name if row.get("image_path") else None
    return row


def with_files(rows: List[Dict[str, Any]], fragments: bool = False) -> List[Dict[str, Any]]:
    """Capture rows plus the transfers (files) that delivered them, as TransferTracker views: usually
    telemetry + image, each with its own radio time, upload time, CRC checks and relay counts."""
    ids = store.transfers_for(r["id"] for r in rows)
    for r in rows:
        with_image_url(r)
        r["files"] = [v for v in (tracker.transfer_view(t, with_fragments=fragments) for t in ids.get(r["id"], []))
                      if v is not None]
    return rows


TELEMETRY_MARKER = "TELEMETRY"  # kept in sync by hand with firmware/app/main.c's APP_TELEMETRY_MARKER


def on_transfer(xfer: CompletedTransfer) -> None:
    meta = xfer.meta
    if kind_of(meta) == "test":
        # A size-ladder test payload (no capture record): a real verified transfer, but not a capture.
        tracker.on_file_saved(xfer.hdr, xfer.is_demo, None)
        log.info("test payload verified: node %s session %s, %d bytes via %s", xfer.src, xfer.session_id,
                 xfer.size, xfer.radio)
        return
    # Task A.2: the node sends capture metadata and the image as two separate ALFP transfers, so
    # the size rule can route the tiny one over LoRa and the image over WiFi. The telemetry
    # transfer's original_name tail (tools/bridge_receiver.decode_meta) is the literal marker
    # above instead of an image file name; its payload is not an image and must not be saved or
    # linked as one. A transfer with no name tail at all (pre-existing, some callers never
    # appended one) still means "has an image", exactly as before this round.
    has_image = meta.get("original_name") != TELEMETRY_MARKER
    path = None
    if has_image:
        ext = ".jpg" if xfer.data[:3] == b"\xff\xd8\xff" else (".png" if xfer.data[:8] == b"\x89PNG\r\n\x1a\n" else ".bin")
        path = store.save_capture_image(xfer.src, xfer.session_id, xfer.data, ext=ext, is_demo=xfer.is_demo)
    else:
        # The marker itself is not a file name: record_capture()'s image_name column must stay
        # null, same as image_path, not literally "TELEMETRY".
        meta = {**meta, "original_name": None}
    row_id = store.record_capture(
        node_id=xfer.src, session_id=xfer.session_id, radio=xfer.radio, frag_total=xfer.frag_total,
        size_bytes=xfer.size, crc32=xfer.crc32, image_path=path, meta=meta, is_demo=xfer.is_demo,
    )
    row = with_image_url(store.get_capture(row_id))
    log.info("%scapture stored: node %s session %s, %d bytes via %s%s",
              "[DEMO] " if xfer.is_demo else "", xfer.src, xfer.session_id, xfer.size, xfer.radio,
              "" if has_image else " (telemetry only)")
    hub.broadcast_soon("capture", row)
    # the transfer that carried it: upload finished, laptop CRC ok, linked to this capture row
    tracker.on_file_saved(xfer.hdr, xfer.is_demo, row_id)


def on_line(line: str) -> None:
    store.log_line(line)
    hub.broadcast_soon("log", {"line": line})


# USB link history for the bridge page, newest last (since this backend started).
bridge_history: List[Dict[str, Any]] = []
BRIDGE_HISTORY_MAX = 50


def on_bridge_status(connected: bool, port: Optional[str]) -> None:
    log.info("bridge serial %s (%s)", "connected" if connected else "LOST", port)
    bridge_history.append({"connected": connected, "at": time.time()})
    del bridge_history[:-BRIDGE_HISTORY_MAX]
    hub.broadcast_soon("bridge_status", bridge_status())


def bridge_status() -> Dict[str, Any]:
    return {"connected": bridge.connected, "port": bridge.port or BRIDGE_PORT, "since": bridge.since,
            "history": list(reversed(bridge_history))}


# docs/PROTOCOLS.md: the bridge's @EVT lines -> transfers / fragments tables + live WS events
tracker = TransferTracker(store, emit=hub.broadcast_soon, log_line=on_line)

bridge = LiveBridge(on_transfer=on_transfer, on_line=on_line, on_event=tracker.on_event,
                    on_upload=tracker.on_upload, on_status=on_bridge_status)

SWEEP_EVERY_S = 15.0


async def sweep_transfers() -> None:
    """Marks transfers that went quiet for 3 minutes as failed / timeout (and tells the dashboards)."""
    while True:
        await asyncio.sleep(SWEEP_EVERY_S)
        try:
            await asyncio.to_thread(tracker.sweep)
        except Exception:
            log.exception("transfer sweep failed")


@asynccontextmanager
async def lifespan(app: FastAPI):
    hub.bind_loop(asyncio.get_running_loop())
    sweeper = asyncio.create_task(sweep_transfers())
    if BRIDGE_PORT:
        log.info("bridge: connecting to %s automatically", BRIDGE_PORT)
        bridge.start_serial_background(BRIDGE_PORT)
    else:
        log.warning(
            "bridge: no serial port found or configured. Set SMARTRELAY_BRIDGE_PORT=/dev/ttyACM0 "
            "(or COM5 on Windows) to connect to the bridge node automatically. "
            "The dashboard will show no real data until then -- "
            "use simulator/demo_feed.py manually if you just want to look at the UI."
        )
    yield
    sweeper.cancel()
    bridge.stop()
    store.close()


app = FastAPI(title="smartrelay", version="2.0", lifespan=lifespan)
app.add_middleware(CORSMiddleware, allow_origins=["*"], allow_methods=["*"], allow_headers=["*"],
                   expose_headers=["Content-Disposition"])


# ---------------------------------------------------------------------------
# Filters (shared by every list, KPI, chart, insight and export)
# ---------------------------------------------------------------------------

def get_filters(
    node_id: Optional[int] = None,
    radio: Optional[str] = Query(None, pattern="^(lora|wifi|unknown)$"),
    trigger: Optional[str] = Query(None, pattern="^(dual|pir|sound|manual)$"),
    since: Optional[float] = None,
    until: Optional[float] = None,
    mode: Optional[str] = Query(None, pattern="^(real|sim|both)$"),
    include_demo: Optional[bool] = None,
    q: Optional[str] = None,
) -> Filters:
    """mode replaces include_demo; the old parameter still works (true -> both, false -> real)."""
    if mode is None:
        mode = "real" if include_demo is None else ("both" if include_demo else "real")
    return Filters(node_id=node_id, radio=radio, trigger=trigger, since=since, until=until, mode=mode,
                   q=(q or None))


# ---------------------------------------------------------------------------
# REST
# ---------------------------------------------------------------------------

@app.get("/api/health")
async def api_health() -> Dict[str, Any]:
    return {
        "ok": True,
        "bridge_port": BRIDGE_PORT,
        "bridge_connected": bridge.connected,
        "bridge": bridge_status(),
        "dashboards_connected": len(hub.clients),
        "stats": store.stats(),
    }


@app.get("/api/nodes")
async def api_nodes() -> Dict[str, Any]:
    """Node status from the only honest signal available: time since the
    bridge last reassembled a real capture from that node. There is no
    periodic heartbeat in the firmware today, so we do not invent one."""
    last_seen = store.node_last_seen()
    arriving = {t["node_id"] for t in tracker.in_flight()}
    now = time.time()
    nodes = []
    for node_id in range(1, 6):
        info = last_seen.get(node_id)
        age = (now - info["last_seen"]) if info else None
        nodes.append({
            "node_id": node_id,
            "role": "bridge" if node_id == 1 else "field",
            "last_seen": info["last_seen"] if info else None,
            "seconds_since": round(age) if age is not None else None,
            "last_radio": info["radio"] if info else None,
            "status": "never seen" if age is None else ("recent" if age < 600 else "stale"),
            "arriving": node_id in arriving,
        })
    return {"nodes": nodes, "note": "status reflects the last real capture received; the firmware has no "
                                    "laptop-facing heartbeat"}


@app.get("/api/nodes/{node_id}")
async def api_node(node_id: int, f: Filters = Depends(get_filters)) -> Dict[str, Any]:
    if not 1 <= node_id <= 5:
        raise HTTPException(status_code=404, detail="no such node")
    if node_id == 1:
        return {"node_id": 1, "role": "bridge", "bridge": bridge_status(), **store.bridge_detail(),
                "in_flight": tracker.in_flight(), "log": store.log_page(limit=20)}
    d = store.node_detail(node_id, f)
    with_files(d["captures"])
    return d


@app.get("/api/summary")
async def api_summary(f: Filters = Depends(get_filters)) -> Dict[str, Any]:
    return store.summary(f)


@app.get("/api/captures")
async def api_captures(f: Filters = Depends(get_filters), limit: int = Query(20, ge=1, le=500),
                       before_id: Optional[int] = None, has_image: bool = False) -> Dict[str, Any]:
    f.has_image = has_image
    rows = store.filtered_captures(f, limit=limit, before_id=before_id)
    return {"captures": with_files(rows), "total": store.count_captures(f)}


EXPORT_FIELDS = [
    "id", "display_id", "node_id", "capture_id", "received_at", "received_iso", "is_demo", "trigger_pir",
    "trigger_sound", "temperature_c", "humidity_pct", "light_raw", "weather_name", "sound_label", "sound_group",
    "sound_conf", "sound_rms", "image_name", "crc32",
]
FILE_FIELDS = ["radio", "size_bytes", "frag_count", "nacks", "dups", "rounds", "hops_max", "radio_s", "upload_s",
               "crc_laptop_ok", "sid"]


@app.get("/api/captures/export")
async def api_export(f: Filters = Depends(get_filters),
                     format: str = Query("csv", pattern="^(csv|json)$")) -> Response:
    rows = with_files(store.filtered_captures(f, limit=100_000))
    stamp = time.strftime("%Y%m%d-%H%M%S")
    if format == "json":
        body = json.dumps({"filters": f.__dict__, "captures": rows}, indent=1, default=str)
        return Response(body, media_type="application/json",
                        headers={"Content-Disposition": f'attachment; filename="smartrelay-captures-{stamp}.json"'})
    buf = io.StringIO()
    w = csv.writer(buf)
    w.writerow(EXPORT_FIELDS + [f"telemetry_{k}" for k in FILE_FIELDS] + [f"image_{k}" for k in FILE_FIELDS])
    for r in rows:
        r["received_iso"] = time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(r["received_at"]))
        by_kind = {t.get("kind"): t for t in r["files"]}
        line = [r.get(k) for k in EXPORT_FIELDS]
        for kind in ("telemetry", "image"):
            t = by_kind.get(kind) or {}
            line += [t.get(k) for k in FILE_FIELDS]
        w.writerow(line)
    return Response(buf.getvalue(), media_type="text/csv",
                    headers={"Content-Disposition": f'attachment; filename="smartrelay-captures-{stamp}.csv"'})


@app.get("/api/captures/{capture_row_id}")
async def api_capture(capture_row_id: int) -> Dict[str, Any]:
    row = store.get_capture(capture_row_id)
    if not row:
        raise HTTPException(status_code=404, detail="capture not found")
    # both transfers of this capture (telemetry + image), each with its own timing, checks and fragments
    row = with_files([row], fragments=True)[0]
    row["transfers"] = row["files"]  # the name the transfer-visibility branch used
    return row


@app.get("/api/transfers")
async def api_transfers(limit: int = Query(20, le=500), node_id: Optional[int] = None, radio: Optional[str] = None,
                        status: Optional[str] = None, mode: str = Query("both", pattern="^(real|sim|both)$")
                        ) -> Dict[str, Any]:
    rows = store.list_transfers(limit=limit, node_id=node_id, radio=radio, status=status, mode=mode)
    return {"transfers": [tracker.transfer_view(r["id"]) for r in rows]}


@app.get("/api/transfers/in_flight")
async def api_transfers_in_flight() -> Dict[str, Any]:
    return {"transfers": tracker.in_flight()}


@app.get("/api/transfers/{transfer_id}")
async def api_transfer(transfer_id: int) -> Dict[str, Any]:
    view = tracker.transfer_view(transfer_id, with_fragments=True)
    if view is None:
        raise HTTPException(status_code=404, detail="transfer not found")
    return view


@app.get("/api/species")
async def api_species(include_demo: bool = False) -> Dict[str, Any]:
    """Tally of the sound labels the NODES reported (common/sound_classes.h), with last seen, nodes
    and mean confidence. The backend only counts stored labels; it never classifies anything."""
    return {"species": store.species_tally(include_demo=include_demo),
            "note": "labels are decided on the node (two-stage tree classifier); the backend only counts them"}


@app.get("/api/sound_classes")
async def api_sound_classes(f: Filters = Depends(get_filters)) -> Dict[str, Any]:
    return {"classes": store.sound_classes(f),
            "note": "sound classes from each node's on-board classifier, not confirmed species"}


@app.get("/api/log")
async def api_log(limit: int = Query(20, ge=1, le=1000), before_id: Optional[int] = None,
                  kind: str = Query("all", pattern="^(all|errors|nack|fail)$"),
                  show_evt: bool = False) -> Dict[str, Any]:
    """Newest first. 'Load older' passes the smallest id it has as before_id."""
    return {"lines": store.log_page(limit=limit, before_id=before_id, kind=kind, show_evt=show_evt)}


# ---------------------------------------------------------------------------
# Demo ingestion -- manual only. Never called automatically by this process.
# ---------------------------------------------------------------------------

class DemoIngest(BaseModel):
    lines: List[str]


@app.post("/api/demo/ingest")
async def demo_ingest(body: DemoIngest) -> Dict[str, Any]:
    """Feeds text through the SAME real parser (live_bridge.LiveBridge) used
    for the actual bridge, but tagged is_demo=1 end to end. This exists so
    simulator/demo_feed.py (run by hand) can exercise the dashboard without
    hardware -- it is not wired to start on its own, and nothing here is
    shown to the dashboard as if it were a real mesh capture."""
    for line in body.lines:
        bridge.feed_line(line, is_demo=True)
    return {"ok": True, "lines_fed": len(body.lines)}


# ---------------------------------------------------------------------------
# Tests page: runs the project's fixed test suites (suite_runner.py). This machine only.
# ---------------------------------------------------------------------------

runner = make_runner(store, lambda line, demo: bridge.feed_line(line, is_demo=demo))
LOCAL_HOSTS = {"127.0.0.1", "::1", "localhost", "testclient"}


def require_local(request: Request) -> None:
    """The suites run programs on this laptop, so only a page served from this laptop may start them:
    the caller must be on loopback and a browser Origin, if sent, must be localhost."""
    host = request.client.host if request.client else ""
    origin = request.headers.get("origin", "")
    origin_host = origin.split("://", 1)[-1].split(":", 1)[0].strip("[]") if origin else ""
    if host not in LOCAL_HOSTS or (origin and origin_host not in LOCAL_HOSTS):
        raise HTTPException(status_code=403, detail="test runs are only allowed from this laptop")


@app.get("/api/tests/suites", dependencies=[Depends(require_local)])
async def api_test_suites() -> Dict[str, Any]:
    cur = runner.current
    return {"suites": runner.suites(), "current": cur.view(len(cur.lines)) if cur else None}


@app.post("/api/tests/run/{suite_id}", dependencies=[Depends(require_local)])
async def api_test_run(suite_id: str) -> Dict[str, Any]:
    try:
        run = runner.start(suite_id)
    except KeyError:
        raise HTTPException(status_code=404, detail="no such suite")
    except RuntimeError as exc:
        raise HTTPException(status_code=409, detail=str(exc))
    return run.view()


@app.get("/api/tests/runs/{run_id}", dependencies=[Depends(require_local)])
async def api_test_run_status(run_id: int, after: int = Query(0, ge=0)) -> Dict[str, Any]:
    run = runner.runs.get(run_id)
    if run is None:
        raise HTTPException(status_code=404, detail="no such run")
    return run.view(after)


@app.post("/api/tests/cancel", dependencies=[Depends(require_local)])
async def api_test_cancel() -> Dict[str, Any]:
    return {"cancelled": runner.cancel()}


# ---------------------------------------------------------------------------
# WebSocket
# ---------------------------------------------------------------------------

@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket) -> None:
    await hub.connect(ws)
    try:
        await ws.send_text(json.dumps({
            "event": "snapshot",
            "data": {
                "nodes": (await api_nodes())["nodes"],
                "captures": with_files(store.filtered_captures(Filters(mode="both"), limit=20)),
                "log": store.recent_log(limit=20),
                "stats": store.stats(),
                "in_flight": tracker.in_flight(),
                "bridge": bridge_status(),
            },
            "ts": time.time(),
        }))
        while True:
            await ws.receive_text()
    except WebSocketDisconnect:
        hub.disconnect(ws)
    except Exception:
        hub.disconnect(ws)


# ---------------------------------------------------------------------------
# Static
# ---------------------------------------------------------------------------

app.mount("/captures", StaticFiles(directory=str(CAPTURES_DIR)), name="captures")

if DASHBOARD_DIST.is_dir():
    assets_dir = DASHBOARD_DIST / "assets"
    if assets_dir.is_dir():
        app.mount("/assets", StaticFiles(directory=str(assets_dir)), name="dashboard-assets")
    _index_path = DASHBOARD_DIST / "index.html"

    @app.get("/{full_path:path}", response_class=FileResponse)
    async def spa_catch_all(full_path: str) -> FileResponse:
        if not _index_path.is_file():
            raise HTTPException(status_code=404, detail="dashboard not built")
        return FileResponse(str(_index_path))
else:
    @app.get("/", response_class=HTMLResponse)
    async def dashboard_missing() -> HTMLResponse:
        return HTMLResponse(
            "<h1>Dashboard not built</h1>"
            "<p>Run <code>cd dashboard && npm install && npm run build</code>, "
            "or <code>npm run dev</code> for the dev server on port 5173.</p>"
        )
