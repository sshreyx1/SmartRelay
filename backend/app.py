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
import json
import logging
import os
import time
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any, Dict, List, Optional, Set

from fastapi import FastAPI, HTTPException, Query, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, HTMLResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from live_bridge import CompletedTransfer, LiveBridge, autodetect_port
from storage import Store

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)-7s %(name)s: %(message)s")
log = logging.getLogger("smartrelay.api")

BASE_DIR = Path(__file__).parent
DASHBOARD_DIST = BASE_DIR.parent / "dashboard" / "dist"

store = Store(db_path=str(BASE_DIR / "smartrelay.db"), captures_dir=str(BASE_DIR / "received"))

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


TELEMETRY_MARKER = "TELEMETRY"  # kept in sync by hand with firmware/app/main.c's APP_TELEMETRY_MARKER


def on_transfer(xfer: CompletedTransfer) -> None:
    meta = xfer.meta
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


def on_line(line: str) -> None:
    store.log_line(line)
    hub.broadcast_soon("log", {"line": line})


bridge = LiveBridge(on_transfer=on_transfer, on_line=on_line)


@asynccontextmanager
async def lifespan(app: FastAPI):
    hub.bind_loop(asyncio.get_running_loop())
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
    bridge.stop()
    store.close()


app = FastAPI(title="smartrelay", version="1.0", lifespan=lifespan)
app.add_middleware(CORSMiddleware, allow_origins=["*"], allow_methods=["*"], allow_headers=["*"])


# ---------------------------------------------------------------------------
# REST
# ---------------------------------------------------------------------------

@app.get("/api/health")
async def api_health() -> Dict[str, Any]:
    return {
        "ok": True,
        "bridge_port": BRIDGE_PORT,
        "bridge_connected": bridge.connected,
        "dashboards_connected": len(hub.clients),
        "stats": store.stats(),
    }


@app.get("/api/nodes")
async def api_nodes() -> Dict[str, Any]:
    """Node status from the only honest signal available: time since the
    bridge last reassembled a real capture from that node. There is no
    periodic heartbeat in the firmware today, so we do not invent one."""
    last_seen = store.node_last_seen()
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
        })
    return {"nodes": nodes, "note": "status reflects real captures only; the firmware has no laptop-facing heartbeat yet"}


@app.get("/api/captures")
async def api_captures(limit: int = Query(50, le=500), node_id: Optional[int] = None,
                       include_demo: bool = True) -> Dict[str, Any]:
    rows = store.recent_captures(limit=limit, node_id=node_id, include_demo=include_demo)
    return {"captures": [with_image_url(r) for r in rows]}


@app.get("/api/captures/{capture_row_id}")
async def api_capture(capture_row_id: int) -> Dict[str, Any]:
    row = store.get_capture(capture_row_id)
    if not row:
        raise HTTPException(status_code=404, detail="capture not found")
    return with_image_url(row)


@app.get("/api/species")
async def api_species(include_demo: bool = False) -> Dict[str, Any]:
    """Tally of the sound labels the NODES reported (common/sound_classes.h), with last seen, nodes
    and mean confidence. The backend only counts stored labels; it never classifies anything."""
    return {"species": store.species_tally(include_demo=include_demo),
            "note": "labels are decided on the node (two-stage tree classifier); the backend only counts them"}


@app.get("/api/log")
async def api_log(limit: int = Query(200, le=1000)) -> Dict[str, Any]:
    return {"lines": store.recent_log(limit=limit)}


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
                "captures": [with_image_url(r) for r in store.recent_captures(limit=30)],
                "log": store.recent_log(limit=50),
                "stats": store.stats(),
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

app.mount("/captures", StaticFiles(directory=str(BASE_DIR / "received")), name="captures")

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
