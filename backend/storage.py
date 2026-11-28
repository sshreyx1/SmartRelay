"""
storage.py - SQLite storage for smartrelay's backend.

Only fields that actually exist in capture_meta.h / alfp_rx_report_t are
stored. A missing sensor reading (capture_meta.h's MISSING_* sentinels,
already turned into None by tools/bridge_receiver.py's decode_meta) is
stored as NULL and shown as "-" by the dashboard -- never a fabricated 0.

Every row carries is_demo: real captures (0) and simulator captures (1,
see simulator/demo_feed.py) share the same schema so the dashboard can
reuse one code path, but the demo rows are always visibly tagged and are
never mixed into totals presented as real mesh activity.

Sound: sound_class is the label id the NODE decided (common/sound_classes.h); sound_label /
sound_group are just its name and group from that table, filled in by bridge_receiver.decode_meta.
Nothing in the backend classifies sounds (docs/SENSORS.md): species_tally() only counts.
"""
from __future__ import annotations

import logging
import re
import sqlite3
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

SCHEMA = """
CREATE TABLE IF NOT EXISTS captures (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id         INTEGER NOT NULL,
    capture_id      INTEGER,
    session_id      INTEGER NOT NULL,
    radio           TEXT,
    frag_total      INTEGER,
    size_bytes      INTEGER,
    crc32           TEXT,
    image_path      TEXT,
    received_at     REAL NOT NULL,

    uptime_ms       INTEGER,
    trigger_pir     INTEGER,
    trigger_sound   INTEGER,
    temperature_c   REAL,
    humidity_pct    REAL,
    light_raw       INTEGER,
    weather_label   INTEGER,
    weather_name    TEXT,
    trigger_trust   INTEGER,
    sound_class     INTEGER,
    sound_name      TEXT,
    sound_rms       INTEGER,
    sound_zcr       INTEGER,
    image_index     INTEGER,

    is_demo         INTEGER NOT NULL DEFAULT 0
);
-- (sound_conf, sound_label, sound_group, image_name, display_id: see MIGRATIONS)

CREATE TABLE IF NOT EXISTS mesh_log (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    line        TEXT NOT NULL,
    received_at REAL NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_captures_node ON captures(node_id);
CREATE INDEX IF NOT EXISTS idx_captures_received ON captures(received_at);

-- One row per ALFP transfer as the bridge saw it (docs/PROTOCOLS.md). A capture is usually
-- two transfers (telemetry + image); captures keeps one merged row, this keeps both transfers' details.
-- Bridge-clock fields are *_ms (ms since the bridge booted); laptop-clock fields are *_at (unix s).
-- NULL = not reported (e.g. old firmware, or the backend started mid-transfer): shown as unavailable.
CREATE TABLE IF NOT EXISTS transfers (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id         INTEGER NOT NULL,
    sid             INTEGER NOT NULL,
    capture_row_id  INTEGER,
    capture_id      INTEGER,
    kind            TEXT,
    radio           TEXT,
    size_bytes      INTEGER,
    frag_count      INTEGER,
    frag_size       INTEGER,
    crc_sender      TEXT,
    crc_bridge_ok   INTEGER,
    crc_laptop_ok   INTEGER,
    status          TEXT,
    fail_reason     TEXT,
    first_ms        INTEGER,
    done_ms         INTEGER,
    first_at        REAL,
    verified_at     REAL,
    upload_start_at REAL,
    upload_end_at   REAL,
    queued_ms       INTEGER,
    nacks           INTEGER,
    dups            INTEGER,
    rounds          INTEGER,
    hops_min        INTEGER,
    hops_max        INTEGER,
    last_at         REAL,
    is_demo         INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS fragments (
    transfer_id INTEGER NOT NULL,
    seq         INTEGER NOT NULL,
    round       INTEGER,
    hops        INTEGER,
    via         INTEGER,
    bridge_ms   INTEGER,
    dup_count   INTEGER DEFAULT 0,
    PRIMARY KEY (transfer_id, seq)
);
CREATE INDEX IF NOT EXISTS idx_transfers_key ON transfers(node_id, sid, is_demo);
CREATE INDEX IF NOT EXISTS idx_transfers_status ON transfers(status);
CREATE INDEX IF NOT EXISTS idx_transfers_capture ON transfers(capture_row_id);
"""

# The bridge's own per-file report, printed by every firmware version (firmware/app/main.c on_rx_done).
# Used once, when the transfers table is first created, to rebuild history from the stored log.
TRANSFER_TIME = "COALESCE(t.verified_at, t.last_at)"  # transfers have no received_at column

RX_LINE = re.compile(r"^rx from node (\d+): (\S+), (\d+) bytes via (\w+), (\d+) dup")
SID_IN_PATH = re.compile(r"n\d+_(\d+)\.bin$")

FIELD_NODES = (2, 3, 4, 5)
REPORTING_WINDOW_S = 600


def parse_rx_line(line: str) -> Optional[Dict[str, Any]]:
    """The bridge's own report of one verified file, or None if the line is something else."""
    m = RX_LINE.match(line.strip())
    if not m:
        return None
    sid = SID_IN_PATH.search(m.group(2))
    return {"node_id": int(m.group(1)), "path": m.group(2), "size_bytes": int(m.group(3)),
            "radio": m.group(4), "duplicates": int(m.group(5)),
            "session_id": int(sid.group(1)) if sid else None}


@dataclass
class Filters:
    """The dashboard's filter bar. Every list, KPI and insight is computed through these."""
    node_id: Optional[int] = None
    radio: Optional[str] = None      # lora | wifi | unknown
    trigger: Optional[str] = None    # dual | pir | sound | manual
    since: Optional[float] = None
    until: Optional[float] = None
    mode: str = "real"               # real | sim | both
    q: Optional[str] = None
    has_image: bool = False          # image gallery: only captures whose image file arrived

    def capture_where(self, alias: str = "c") -> Tuple[str, List[Any]]:
        a = alias + "."
        where: List[str] = []
        args: List[Any] = []
        if self.mode == "real":
            where.append(f"{a}is_demo = 0")
        elif self.mode == "sim":
            where.append(f"{a}is_demo = 1")
        if self.node_id is not None:
            where.append(f"{a}node_id = ?")
            args.append(self.node_id)
        if self.since is not None:
            where.append(f"{a}received_at >= ?")
            args.append(self.since)
        if self.until is not None:
            where.append(f"{a}received_at <= ?")
            args.append(self.until)
        if self.radio in ("lora", "wifi"):
            where.append(f"({a}radio = ? OR EXISTS (SELECT 1 FROM transfers t2 WHERE t2.capture_row_id = {a}id "
                         f"AND t2.radio = ?))")
            args += [self.radio, self.radio]
        elif self.radio == "unknown":
            where.append(f"({a}radio IS NULL OR {a}radio NOT IN ('lora', 'wifi'))")
        if self.trigger == "dual":
            where.append(f"{a}trigger_pir = 1 AND {a}trigger_sound = 1")
        elif self.trigger == "pir":
            where.append(f"{a}trigger_pir = 1 AND {a}trigger_sound = 0")
        elif self.trigger == "sound":
            where.append(f"{a}trigger_pir = 0 AND {a}trigger_sound = 1")
        elif self.trigger == "manual":
            where.append(f"{a}trigger_pir = 0 AND {a}trigger_sound = 0")
        if self.has_image:
            where.append(f"{a}image_path IS NOT NULL")
        if self.q:
            like = f"%{self.q.strip()}%"
            where.append(f"({a}sound_label LIKE ? OR {a}display_id LIKE ? OR CAST({a}capture_id AS TEXT) LIKE ?)")
            args += [like, like, like]
        return (" WHERE " + " AND ".join(where)) if where else "", args

    def transfer_where(self, alias: str = "t") -> Tuple[str, List[Any]]:
        a = alias + "."
        where: List[str] = []
        args: List[Any] = []
        if self.mode == "real":
            where.append(f"{a}is_demo = 0")
        elif self.mode == "sim":
            where.append(f"{a}is_demo = 1")
        if self.node_id is not None:
            where.append(f"{a}node_id = ?")
            args.append(self.node_id)
        if self.since is not None:
            where.append(f"COALESCE({a}verified_at, {a}last_at) >= ?")
            args.append(self.since)
        if self.until is not None:
            where.append(f"COALESCE({a}verified_at, {a}last_at) <= ?")
            args.append(self.until)
        if self.radio in ("lora", "wifi"):
            where.append(f"{a}radio = ?")
            args.append(self.radio)
        elif self.radio == "unknown":
            where.append(f"({a}radio IS NULL OR {a}radio NOT IN ('lora', 'wifi'))")
        if self.trigger or self.q:
            sub = Filters(trigger=self.trigger, q=self.q, mode="both")
            cw, cargs = sub.capture_where("c3")
            where.append(f"{a}capture_row_id IN (SELECT c3.id FROM captures c3{cw})")
            args += cargs
        return (" WHERE " + " AND ".join(where)) if where else "", args


def _median(values: List[float]) -> Optional[float]:
    v = sorted(x for x in values if x is not None)
    if not v:
        return None
    mid = len(v) // 2
    return v[mid] if len(v) % 2 else (v[mid - 1] + v[mid]) / 2


def _hhmm(ts: Optional[float]) -> str:
    return time.strftime("%H:%M", time.localtime(ts)) if ts else "?"


def _span(a: Optional[float], b: Optional[float]) -> str:
    """'09:12–12:27', or with dates when the span crosses midnight."""
    if not a or not b:
        return "?"
    if time.strftime("%Y%m%d", time.localtime(a)) == time.strftime("%Y%m%d", time.localtime(b)):
        return f"{_hhmm(a)}–{_hhmm(b)}"
    fmt = "%d %b %H:%M"
    return f"{time.strftime(fmt, time.localtime(a))} – {time.strftime(fmt, time.localtime(b))}"


def _kb(n: Optional[float]) -> str:
    if n is None:
        return "?"
    return f"{n:.0f} B" if n < 1024 else f"{n / 1024:.1f} KB"


# Columns added after the first schema. Added with ALTER TABLE when missing, so an existing
# smartrelay.db keeps working; the sound-label rows need all nodes reflashed (docs/BRINGUP.md).
MIGRATIONS = [
    ("sound_conf", "INTEGER"),
    ("sound_label", "TEXT"),
    ("sound_group", "TEXT"),
    ("image_name", "TEXT"),
    ("display_id", "TEXT"),
]

# One real capture = one row. Demo runs restart their counters, so demo rows are left out.
UNIQUE_CAPTURE_INDEX = ("CREATE UNIQUE INDEX IF NOT EXISTS idx_captures_unique "
                        "ON captures(node_id, capture_id) WHERE is_demo = 0")

log = logging.getLogger("smartrelay.storage")


def _locked(fn):
    """Run a Store method while holding the connection lock (the serial thread and the API share it)."""
    def wrapper(self, *args, **kwargs):
        with self.lock:
            return fn(self, *args, **kwargs)
    wrapper.__name__ = fn.__name__
    wrapper.__doc__ = fn.__doc__
    return wrapper


class Store:
    def __init__(self, db_path: str, captures_dir: str):
        self.db_path = db_path
        self.captures_dir = Path(captures_dir)
        self.captures_dir.mkdir(parents=True, exist_ok=True)
        self.conn = sqlite3.connect(db_path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        # one connection shared by the serial thread (writes on every fragment) and the API: serialise it
        self.lock = threading.RLock()
        had_transfers = self.conn.execute(
            "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'transfers'").fetchone() is not None
        self.conn.executescript(SCHEMA)
        have = {r["name"] for r in self.conn.execute("PRAGMA table_info(captures)")}
        for col, typ in MIGRATIONS:
            if col not in have:
                self.conn.execute(f"ALTER TABLE captures ADD COLUMN {col} {typ}")
        try:
            self.conn.execute(UNIQUE_CAPTURE_INDEX)
        except sqlite3.IntegrityError:
            log.warning("smartrelay.db already holds duplicate (node, capture id) rows from before capture ids "
                        "were persistent; delete backend/smartrelay.db to get the uniqueness check")
        self.conn.commit()
        if not had_transfers:
            n = self.backfill_transfers_from_log()
            if n:
                log.info("transfers: rebuilt %d file transfers from the stored bridge log", n)

    # -- history: rebuild transfers from the stored bridge log ----------------------------------

    @_locked
    def backfill_transfers_from_log(self) -> int:
        """One-off, when the transfers table is first created: every stored "rx from node" line is the
        bridge's report of a CRC-verified file, so it becomes a verified transfer row (no live fields: those
        need @EVT lines). A row is linked to a capture only on an exact match (the saved image is named
        n<node>_<sid>, or the capture row's session id is this file's); nothing is inferred from sizes."""
        rows = self.conn.execute("SELECT line, received_at FROM mesh_log ORDER BY id").fetchall()
        by_image = {}
        by_session = {}
        for c in self.conn.execute("SELECT id, node_id, session_id, capture_id, image_path FROM captures "
                                   "WHERE is_demo = 0").fetchall():
            if c["image_path"]:
                m = re.search(r"n(\d+)_(\d+)\.", Path(c["image_path"]).name)
                if m:
                    by_image[(int(m.group(1)), int(m.group(2)))] = c
            by_session[(c["node_id"], c["session_id"])] = c
        n = 0
        for r in rows:
            rx = parse_rx_line(r["line"])
            if not rx or rx["session_id"] is None:
                continue
            key = (rx["node_id"], rx["session_id"])
            cap, kind = by_image.get(key), None
            if cap is not None:
                kind = "image"
            elif key in by_session:
                # the capture row's last merged file, and its saved image (if any) has another session id:
                # on_transfer() saves every non-telemetry file as an image, so this one was the telemetry file
                cap, kind = by_session[key], "telemetry"
            self.conn.execute(
                """INSERT INTO transfers (node_id, sid, capture_id, capture_row_id, kind, radio, size_bytes, status,
                   crc_bridge_ok, crc_laptop_ok, dups, verified_at, last_at, is_demo)
                   VALUES (?, ?, ?, ?, ?, ?, ?, 'verified', 1, ?, ?, ?, ?, 0)""",
                (rx["node_id"], rx["session_id"], cap["capture_id"] if cap else None, cap["id"] if cap else None,
                 kind, rx["radio"], rx["size_bytes"], 1 if cap else None, rx["duplicates"], r["received_at"],
                 r["received_at"]))
            n += 1
        self.conn.commit()
        return n

    @_locked
    def transfers_for(self, capture_row_ids: Iterable[int]) -> Dict[int, List[int]]:
        """Transfer ids per capture row (the caller builds each view with TransferTracker.transfer_view)."""
        ids = [int(i) for i in capture_row_ids]
        if not ids:
            return {}
        marks = ",".join("?" * len(ids))
        out: Dict[int, List[int]] = {i: [] for i in ids}
        for r in self.conn.execute(f"SELECT id, capture_row_id FROM transfers WHERE capture_row_id IN ({marks}) "
                                   f"ORDER BY id", ids):
            out[r["capture_row_id"]].append(r["id"])
        return out

    # -- filtered reads for the dashboard ----------------------------------------

    @_locked
    def filtered_captures(self, f: Filters, limit: int = 20, before_id: Optional[int] = None) -> List[Dict[str, Any]]:
        where, args = f.capture_where("c")
        if before_id is not None:
            where += (" AND " if where else " WHERE ") + "c.id < ?"
            args.append(before_id)
        q = f"SELECT c.* FROM captures c{where} ORDER BY c.received_at DESC, c.id DESC LIMIT ?"
        return [dict(r) for r in self.conn.execute(q, args + [limit]).fetchall()]

    @_locked
    def count_captures(self, f: Filters) -> int:
        where, args = f.capture_where("c")
        return self.conn.execute(f"SELECT COUNT(*) n FROM captures c{where}", args).fetchone()["n"]

    @_locked
    def data_range(self, mode: str = "real") -> Dict[str, Optional[float]]:
        where, args = Filters(mode=mode).capture_where("c")
        r = self.conn.execute(f"SELECT MIN(received_at) a, MAX(received_at) b FROM captures c{where}", args).fetchone()
        return {"first": r["a"], "last": r["b"]}

    @_locked
    def nodes_reporting(self, now: Optional[float] = None) -> Dict[str, Any]:
        now = now or time.time()
        rows = self.conn.execute(
            f"""SELECT DISTINCT node_id FROM captures WHERE is_demo = 0 AND received_at >= ?
                AND node_id IN ({",".join("?" * len(FIELD_NODES))})""",
            [now - REPORTING_WINDOW_S, *FIELD_NODES]).fetchall()
        return {"count": len(rows), "of": len(FIELD_NODES), "window_s": REPORTING_WINDOW_S,
                "basis": "last real capture", "nodes": sorted(r["node_id"] for r in rows)}

    @_locked
    def sound_classes(self, f: Filters) -> List[Dict[str, Any]]:
        where, args = f.capture_where("c")
        where += (" AND " if where else " WHERE ") + "c.sound_label IS NOT NULL"
        q = f"""SELECT c.sound_class, c.sound_label, c.sound_group, COUNT(*) AS count, AVG(c.sound_conf) AS mean_conf,
                       GROUP_CONCAT(DISTINCT c.node_id) AS nodes, MAX(c.received_at) AS last_seen,
                       SUM(c.is_demo) AS sim_count
                FROM captures c{where} GROUP BY c.sound_class ORDER BY count DESC, last_seen DESC"""
        out = []
        for r in self.conn.execute(q, args).fetchall():
            d = dict(r)
            d["nodes"] = sorted(int(n) for n in str(d["nodes"]).split(",")) if d["nodes"] else []
            d["mean_conf"] = round(d["mean_conf"], 1) if d["mean_conf"] is not None else None
            out.append(d)
        return out

    @_locked
    def radio_mix(self, f: Filters) -> Dict[str, Any]:
        where, args = f.transfer_where("t")
        where += (" AND " if where else " WHERE ") + "t.status = 'verified'"
        rows = self.conn.execute(
            f"""SELECT COALESCE(t.radio, 'unknown') AS radio, t.node_id, COALESCE(t.kind, 'unknown') AS kind,
                       COUNT(*) AS files, AVG(t.size_bytes) AS avg_size, AVG(t.frag_count) AS avg_frags,
                       MAX(t.size_bytes) AS max_size, SUM(t.dups) AS duplicates
                FROM transfers t{where} GROUP BY 1, 2, 3""", args).fetchall()
        return {"rows": [dict(r) for r in rows],
                "note": "counts of completed files, not success rates"}

    @_locked
    def timeline(self, f: Filters) -> Dict[str, Any]:
        where, args = f.capture_where("c")
        rows = self.conn.execute(f"SELECT c.received_at, c.is_demo FROM captures c{where}", args).fetchall()
        if not rows:
            return {"bin_s": 900, "bins": []}
        first = min(r["received_at"] for r in rows)
        last = max(r["received_at"] for r in rows)
        start = f.since if f.since is not None else first
        end = min(f.until if f.until is not None else max(last, time.time()), time.time())
        end = max(end, last)
        bin_s = 900
        for candidate in (900, 3600, 6 * 3600, 86400):
            bin_s = candidate
            if (end - start) / candidate <= 96:
                break
        t0 = int(start // bin_s) * bin_s
        n = int((end - t0) // bin_s) + 1
        bins = [{"t": t0 + i * bin_s, "real": 0, "sim": 0} for i in range(n)]
        for r in rows:
            i = int((r["received_at"] - t0) // bin_s)
            if 0 <= i < n:
                bins[i]["sim" if r["is_demo"] else "real"] += 1
        return {"bin_s": bin_s, "bins": bins}

    @_locked
    def _median_radio_s(self, tw: str, targs: List[Any], kind: str = "image") -> Dict[str, Optional[float]]:
        """Median bridge-measured radio time (done_ms - first_ms) per radio. None where never reported."""
        out: Dict[str, Optional[float]] = {}
        for radio in ("wifi", "lora"):
            vals = [r["d"] for r in self.conn.execute(
                f"""SELECT (t.done_ms - t.first_ms) / 1000.0 d FROM transfers t{tw}
                    {" AND " if tw else " WHERE "} t.done_ms IS NOT NULL AND t.first_ms IS NOT NULL
                    AND t.status = 'verified' AND t.kind = ? AND t.radio = ?""", targs + [kind, radio])]
            out[radio] = _median(vals)
        return out

    @_locked
    def summary(self, f: Filters) -> Dict[str, Any]:
        cw, cargs = f.capture_where("c")
        c = self.conn.execute(
            f"""SELECT COUNT(*) n, SUM(1 - c.is_demo) real_n, SUM(c.is_demo) sim_n,
                       SUM(CASE WHEN c.trigger_pir = 1 AND c.trigger_sound = 1 THEN 1 ELSE 0 END) dual_n,
                       MIN(c.received_at) first, MAX(c.received_at) last
                FROM captures c{cw}""", cargs).fetchone()
        tw, targs = f.transfer_where("t")
        t = self.conn.execute(
            f"""SELECT SUM(CASE WHEN t.status = 'verified' THEN 1 ELSE 0 END) verified,
                       SUM(CASE WHEN t.status = 'failed' THEN 1 ELSE 0 END) failed,
                       SUM(CASE WHEN t.crc_laptop_ok = 0 THEN 1 ELSE 0 END) laptop_failed,
                       SUM(CASE WHEN t.status = 'verified' AND t.radio = 'lora' THEN 1 ELSE 0 END) lora,
                       SUM(CASE WHEN t.status = 'verified' AND t.radio = 'wifi' THEN 1 ELSE 0 END) wifi,
                       SUM(CASE WHEN t.status = 'verified' AND (t.radio IS NULL OR t.radio NOT IN ('lora', 'wifi'))
                                THEN 1 ELSE 0 END) unknown
                FROM transfers t{tw}""", targs).fetchone()
        by_node = {str(r["node_id"]): r["n"] for r in self.conn.execute(
            f"SELECT c.node_id, COUNT(*) n FROM captures c{cw} GROUP BY c.node_id", cargs)}
        uploads = [r["d"] for r in self.conn.execute(
            f"""SELECT t.upload_end_at - t.upload_start_at d FROM transfers t{tw}
                {" AND " if tw else " WHERE "} t.upload_end_at IS NOT NULL AND t.upload_start_at IS NOT NULL
                AND t.kind = 'image'""", targs)]
        med = self._median_radio_s(tw, targs)
        n = c["n"] or 0
        return {
            "range": self.data_range(f.mode),
            "filtered_range": {"first": c["first"], "last": c["last"]},
            "captures": n,
            "real_captures": c["real_n"] or 0,
            "sim_captures": c["sim_n"] or 0,
            "dual_sensor": {"count": c["dual_n"] or 0, "of": n},
            "crc_verified_transfers": t["verified"] or 0,
            "failed_transfers": t["failed"] or 0,
            "laptop_crc_failed": t["laptop_failed"] or 0,
            "field_nodes_reporting": None if f.mode == "sim" else self.nodes_reporting(),
            "by_node": by_node,
            "by_radio": {"lora": t["lora"] or 0, "wifi": t["wifi"] or 0, "unknown": t["unknown"] or 0},
            "timeline": self.timeline(f),
            # bridge clock: done_ms - first_ms from RXOK; None when no transfer reported it (old firmware)
            "median_transfer_s": med if any(v is not None for v in med.values()) else None,
            "median_usb_upload_s": _median(uploads),
            "sound_classes": self.sound_classes(f),
            "radio_mix": self.radio_mix(f),
            "insights": self.insights(f, n, by_node),
        }

    @_locked
    def insights(self, f: Filters, n: int, by_node: Dict[str, int]) -> List[Dict[str, str]]:
        """Max 3 plain statements, each with the evidence behind it, from the filtered rows only."""
        kind = {"real": "real", "sim": "simulated", "both": ""}[f.mode]
        noun = f"{kind} captures".strip()
        if n < 10:
            return [{"level": "warn",
                     "text": f"Not enough data for insights. Only {n} {noun} match these filters; at least 10 are "
                             f"needed to compare nodes or time periods.",
                     "evidence": "Insights are computed from the filtered captures only"}]
        cw, cargs = f.capture_where("c")
        span = self.conn.execute(f"SELECT MIN(c.received_at) a, MAX(c.received_at) b FROM captures c{cw}",
                                 cargs).fetchone()
        out: List[Dict[str, str]] = []
        field = {k: v for k, v in by_node.items() if int(k) in FIELD_NODES}
        if field and f.node_id is None:
            top, cnt = max(field.items(), key=lambda kv: kv[1])
            out.append({"level": "info",
                        "text": f"Node {top} is the busiest field node with {cnt} of {n} {noun} ({cnt * 100 // n}%).",
                        "evidence": f"Based on {n} {noun}, {_span(span['a'], span['b'])}"})
        tw, targs = f.transfer_where("t")
        vw = tw + (" AND " if tw else " WHERE ") + "t.status = 'verified'"
        rad = {r["radio"]: dict(r) for r in self.conn.execute(
            f"""SELECT t.radio, COUNT(*) files, AVG(t.size_bytes) avg_size, MAX(t.size_bytes) max_size
                FROM transfers t{vw} GROUP BY t.radio""", targs)}
        lo, wi = rad.get("lora"), rad.get("wifi")
        if lo or wi:
            parts = []
            if lo:
                parts.append(f"{lo['files']} LoRa files (avg {_kb(lo['avg_size'])}, largest {_kb(lo['max_size'])})")
            if wi:
                parts.append(f"{wi['files']} Wi-Fi files (avg {_kb(wi['avg_size'])})")
            lead = ""
            if lo and wi and lo["max_size"] is not None and wi["avg_size"] and lo["max_size"] < wi["avg_size"]:
                lead = "LoRa carried the small files, Wi-Fi the large ones. "
            total = sum(r["files"] for r in rad.values())
            out.append({"level": "info", "text": lead + " and ".join(parts) + ".",
                        "evidence": f"{total} verified transfers · radio from the bridge's file header"})
        end = span["b"]
        last_h = self.count_captures(Filters(**{**f.__dict__, "since": end - 3600, "until": end}))
        prev_h = self.count_captures(Filters(**{**f.__dict__, "since": end - 7200, "until": end - 3600 - 1e-6}))
        if last_h or prev_h:
            word = "More" if last_h > prev_h else ("Less" if last_h < prev_h else "Same")
            out.append({"level": "info",
                        "text": f"{word} activity in the last hour of data: {last_h} captures from "
                                f"{_hhmm(end - 3600)} to {_hhmm(end)} vs {prev_h} in the hour before.",
                        "evidence": "Two equal 60-min windows ending at the newest capture"})
        if len(out) < 3:
            fw, _ = f.transfer_where("t2")
            fw += (" AND " if fw else " WHERE ") + "t2.status = 'verified'"
            r = self.conn.execute(
                f"""SELECT COUNT(*) total, SUM(CASE WHEN fr.round >= 1 THEN 1 ELSE 0 END) resent
                    FROM fragments fr JOIN transfers t2 ON t2.id = fr.transfer_id{fw}""", targs).fetchone()
            if r["total"]:
                out.append({"level": "info",
                            "text": f"{(r['resent'] or 0) * 100 // r['total']}% of fragments were resent after a "
                                    f"bridge NACK ({r['resent'] or 0} of {r['total']}).",
                            "evidence": "Fragment rounds from the bridge's @EVT RXF lines"})
            else:
                d = self.conn.execute(
                    f"""SELECT SUM(t.dups) dup, COUNT(*) files FROM transfers t{vw} AND t.dups IS NOT NULL""",
                    targs).fetchone()
                if d["files"]:
                    out.append({"level": "info",
                                "text": f"The bridge ignored {d['dup'] or 0} duplicate fragment copies across "
                                        f"{d['files']} files (relays flood every packet; the first copy wins).",
                                "evidence": "Duplicate counts reported by the bridge"})
        return out[:3]

    @_locked
    def node_detail(self, node_id: int, f: Filters) -> Dict[str, Any]:
        nf = Filters(**{**f.__dict__, "node_id": node_id})
        caps = self.filtered_captures(nf, limit=20)
        cw, cargs = nf.capture_where("c")
        readings = [dict(r) for r in self.conn.execute(
            f"""SELECT c.id, c.received_at, c.temperature_c, c.humidity_pct, c.light_raw, c.sound_rms, c.is_demo
                FROM captures c{cw} ORDER BY c.received_at DESC LIMIT 200""", cargs)][::-1]
        tw, targs = nf.transfer_where("t")
        vw = tw + (" AND " if tw else " WHERE ") + "t.status = 'verified'"
        agg = self.conn.execute(
            f"""SELECT COUNT(*) files, SUM(t.frag_count) frags, SUM(t.dups) dup,
                       SUM(CASE WHEN t.dups IS NOT NULL THEN 1 ELSE 0 END) dup_known,
                       SUM(CASE WHEN t.radio = 'wifi' THEN 1 ELSE 0 END) wifi_files,
                       SUM(CASE WHEN t.radio = 'lora' THEN 1 ELSE 0 END) lora_files
                FROM transfers t{vw}""", targs).fetchone()
        last_t = self.conn.execute(
            f"""SELECT t.radio, t.kind, {TRANSFER_TIME} AS received_at FROM transfers t{vw}
                ORDER BY {TRANSFER_TIME} DESC LIMIT 1""", targs).fetchone()
        last = self.conn.execute(
            "SELECT MAX(received_at) m FROM captures WHERE node_id = ? AND is_demo = 0", (node_id,)).fetchone()["m"]
        fw, _ = nf.transfer_where("t2")
        fw += (" AND " if fw else " WHERE ") + "t2.status = 'verified'"
        fr = self.conn.execute(
            f"""SELECT COUNT(*) total, SUM(CASE WHEN fr.round >= 1 THEN 1 ELSE 0 END) resent
                FROM fragments fr JOIN transfers t2 ON t2.id = fr.transfer_id{fw}""", targs).fetchone()
        via_rows = self.conn.execute(
            f"""SELECT t2.radio radio, COALESCE(fr.via, 0) via, COUNT(*) c
                FROM fragments fr JOIN transfers t2 ON t2.id = fr.transfer_id{fw} GROUP BY 1, 2""", targs).fetchall()
        relay_counts = None
        if via_rows:
            relay_counts = {"wifi": {}, "lora": 0}
            for r in via_rows:
                if r["radio"] == "lora":
                    relay_counts["lora"] += r["c"]
                else:
                    key = "direct" if r["via"] == node_id else ("unknown" if r["via"] == 0 else str(r["via"]))
                    relay_counts["wifi"][key] = relay_counts["wifi"].get(key, 0) + r["c"]
        med = self._median_radio_s(tw, targs)
        last_radio = (last_t["radio"] if last_t else None) or "wifi"
        return {
            "node_id": node_id,
            "role": "bridge" if node_id == 1 else "field",
            "captures_count": self.count_captures(nf),
            "last_capture_at": last,
            "last_radio": dict(last_t) if last_t else None,
            "median_image_transfer_s": med.get("wifi") if med.get("wifi") is not None else med.get(last_radio),
            "median_image_transfer_radio": "wifi" if med.get("wifi") is not None else last_radio,
            "fragments_resent": {"resent": fr["resent"] or 0, "of": fr["total"]} if fr["total"] else None,
            "files": dict(agg),
            "captures": caps,
            "readings": readings,
            "relay_counts": relay_counts,
        }

    @_locked
    def bridge_detail(self) -> Dict[str, Any]:
        t = self.conn.execute(
            """SELECT SUM(CASE WHEN status = 'verified' THEN 1 ELSE 0 END) verified,
                      SUM(CASE WHEN status = 'failed' THEN 1 ELSE 0 END) failed,
                      SUM(CASE WHEN crc_laptop_ok = 0 THEN 1 ELSE 0 END) laptop_failed,
                      SUM(CASE WHEN crc_laptop_ok = 1 THEN 1 ELSE 0 END) laptop_ok,
                      SUM(CASE WHEN is_demo = 0 THEN 1 ELSE 0 END) real
               FROM transfers""").fetchone()
        reasons = {r["fail_reason"] or "unknown": r["c"] for r in self.conn.execute(
            "SELECT fail_reason, COUNT(*) c FROM transfers WHERE status = 'failed' GROUP BY fail_reason")}
        crc_retries = self.conn.execute(
            "SELECT COUNT(*) c FROM mesh_log WHERE line LIKE '%file CRC-32 mismatch on the bridge%'").fetchone()["c"]
        return {"transfers": {**dict(t), "fail_reasons": reasons, "bridge_crc_retries": crc_retries}}

    @_locked
    def log_page(self, limit: int = 20, before_id: Optional[int] = None, kind: str = "all",
                 show_evt: bool = False) -> List[Dict[str, Any]]:
        where, args = [], []
        if before_id is not None:
            where.append("id < ?")
            args.append(before_id)
        if not show_evt:
            where.append("line NOT LIKE '@EVT%'")
        if kind == "errors":
            where.append("(line LIKE '%error%' OR line LIKE '%fail%' OR line LIKE '%REJECT%' OR line LIKE '%WARN%' "
                         "OR line LIKE '%ABORT%' OR line LIKE '%lost%' OR line LIKE '%mismatch%')")
        elif kind == "nack":
            where.append("(line LIKE '%nack%' OR line LIKE '%missing fragment%')")
        elif kind == "fail":
            where.append("(line LIKE '%fail%' OR line LIKE '%REJECT%' OR line LIKE '%ABORT%')")
        q = "SELECT * FROM mesh_log" + (" WHERE " + " AND ".join(where) if where else "") + " ORDER BY id DESC LIMIT ?"
        return [dict(r) for r in self.conn.execute(q, args + [limit]).fetchall()]

    def close(self) -> None:
        self.conn.close()

    # -- writes -------------------------------------------------------------

    @_locked
    def save_capture_image(self, node_id: int, session_id: int, data: bytes, ext: str = ".jpg",
                           is_demo: bool = False) -> str:
        prefix = "demo_" if is_demo else ""
        name = f"{prefix}n{node_id}_{session_id:05d}{ext}"
        path = self.captures_dir / name
        path.write_bytes(data)
        return str(path)

    # Columns that only mean anything once an image has actually arrived (Task A.2: the telemetry
    # and image halves of one capture are two separate ALFP transfers, and either can arrive
    # first). An update carrying None for one of these must not erase a value a previous transfer
    # for the same capture already stored -- COALESCE(new, old) instead of a blind overwrite.
    _IMAGE_ONLY_COLUMNS = ("image_path", "image_name")

    @_locked
    def record_capture(self, node_id: int, session_id: int, radio: str, frag_total: int,
                       size_bytes: int, crc32: str, image_path: Optional[str], meta: Dict[str, Any],
                       is_demo: bool = False) -> int:
        """Stores one capture and returns its row id. The same real capture delivered twice (e.g.
        re-sent after a node reboot, or telemetry and image arriving as separate transfers) updates
        its existing row instead of adding a second one. image_path is None for a telemetry-only
        transfer (no image attached yet); an update with image_path=None never erases an image a
        previous transfer for the same capture_id already stored (see _IMAGE_ONLY_COLUMNS)."""
        values = {
            "node_id": node_id, "capture_id": meta.get("capture_id"), "session_id": session_id, "radio": radio,
            "frag_total": frag_total, "size_bytes": size_bytes, "crc32": crc32, "image_path": image_path,
            "received_at": time.time(), "uptime_ms": meta.get("uptime_ms"),
            "trigger_pir": int(bool(meta.get("trigger_pir"))), "trigger_sound": int(bool(meta.get("trigger_sound"))),
            "temperature_c": meta.get("temperature_c"), "humidity_pct": meta.get("humidity_pct"),
            "light_raw": meta.get("light_raw"), "weather_label": meta.get("weather_label"),
            "weather_name": meta.get("weather_name"), "trigger_trust": meta.get("trigger_trust"),
            "sound_class": meta.get("sound_class"), "sound_name": meta.get("sound_name"),
            "sound_rms": meta.get("sound_rms"), "sound_zcr": meta.get("sound_zcr"),
            "image_index": meta.get("image_index"), "is_demo": int(is_demo),
            "sound_conf": meta.get("sound_confidence"), "sound_label": meta.get("sound_label"),
            "sound_group": meta.get("sound_group"), "image_name": meta.get("original_name"),
            "display_id": meta.get("display_id"),
        }
        cols = ", ".join(values)
        marks = ", ".join("?" * len(values))
        cur = self.conn.execute(f"INSERT OR IGNORE INTO captures ({cols}) VALUES ({marks})", list(values.values()))
        if cur.rowcount == 0:  # this real (node, capture id) is already stored: merge into it
            sets = ", ".join(f"{k} = COALESCE(?, {k})" if k in self._IMAGE_ONLY_COLUMNS else f"{k} = ?"
                             for k in values)
            row = self.conn.execute("SELECT id FROM captures WHERE node_id = ? AND capture_id = ? AND is_demo = 0",
                                    (node_id, meta.get("capture_id"))).fetchone()
            self.conn.execute(f"UPDATE captures SET {sets} WHERE id = ?", list(values.values()) + [row["id"]])
            self.conn.commit()
            return row["id"]
        self.conn.commit()
        return cur.lastrowid

    @_locked
    def log_line(self, line: str) -> None:
        self.conn.execute("INSERT INTO mesh_log (line, received_at) VALUES (?, ?)", (line, time.time()))
        self.conn.commit()

    # -- reads ----------------------------------------------------------

    @_locked
    def recent_captures(self, limit: int = 50, node_id: Optional[int] = None,
                        include_demo: bool = True) -> List[Dict[str, Any]]:
        q = "SELECT * FROM captures"
        where, args = [], []
        if node_id is not None:
            where.append("node_id = ?")
            args.append(node_id)
        if not include_demo:
            where.append("is_demo = 0")
        if where:
            q += " WHERE " + " AND ".join(where)
        q += " ORDER BY received_at DESC LIMIT ?"
        args.append(limit)
        return [dict(r) for r in self.conn.execute(q, args).fetchall()]

    @_locked
    def get_capture(self, capture_row_id: int) -> Optional[Dict[str, Any]]:
        row = self.conn.execute("SELECT * FROM captures WHERE id = ?", (capture_row_id,)).fetchone()
        return dict(row) if row else None

    @_locked
    def node_last_seen(self) -> Dict[int, Dict[str, Any]]:
        """Last REAL (non-demo) capture per node -- this is the only honest
        signal the bridge gives us about a node being alive, since the
        firmware does not broadcast a periodic heartbeat to the laptop."""
        rows = self.conn.execute(
            """SELECT node_id, MAX(received_at) AS last_seen, radio
               FROM captures WHERE is_demo = 0 GROUP BY node_id"""
        ).fetchall()
        return {r["node_id"]: {"last_seen": r["last_seen"], "radio": r["radio"]} for r in rows}

    @_locked
    def recent_log(self, limit: int = 200) -> List[Dict[str, Any]]:
        rows = self.conn.execute(
            "SELECT * FROM mesh_log ORDER BY id DESC LIMIT ?", (limit,)
        ).fetchall()
        return [dict(r) for r in reversed(rows)]

    @_locked
    def species_tally(self, include_demo: bool = False) -> List[Dict[str, Any]]:
        """Captures per sound label the nodes reported: count, last seen, nodes, mean confidence.
        Pure counting of stored values; the labels were decided on the node."""
        q = """SELECT sound_class, sound_label, sound_group, COUNT(*) AS count, MAX(received_at) AS last_seen,
                      GROUP_CONCAT(DISTINCT node_id) AS nodes, AVG(sound_conf) AS mean_conf,
                      SUM(is_demo) AS demo_count
               FROM captures WHERE sound_class IS NOT NULL AND sound_label IS NOT NULL"""
        if not include_demo:
            q += " AND is_demo = 0"
        q += " GROUP BY sound_class ORDER BY count DESC, last_seen DESC"
        out = []
        for r in self.conn.execute(q).fetchall():
            d = dict(r)
            d["nodes"] = sorted(int(n) for n in str(d["nodes"]).split(",")) if d["nodes"] else []
            d["mean_conf"] = round(d["mean_conf"], 1) if d["mean_conf"] is not None else None
            out.append(d)
        return out

    @_locked
    def stats(self) -> Dict[str, Any]:
        total = self.conn.execute("SELECT COUNT(*) c FROM captures WHERE is_demo = 0").fetchone()["c"]
        demo = self.conn.execute("SELECT COUNT(*) c FROM captures WHERE is_demo = 1").fetchone()["c"]
        by_radio = self.conn.execute(
            "SELECT radio, COUNT(*) c FROM captures WHERE is_demo = 0 GROUP BY radio"
        ).fetchall()
        return {
            "real_captures": total,
            "demo_captures": demo,
            "by_radio": {r["radio"]: r["c"] for r in by_radio},
        }

    # -- transfers (docs/PROTOCOLS.md) ----------------------------------------------------

    TRANSFER_COLUMNS = ("node_id", "sid", "capture_row_id", "capture_id", "kind", "radio", "size_bytes",
                        "frag_count", "frag_size", "crc_sender", "crc_bridge_ok", "crc_laptop_ok", "status",
                        "fail_reason", "first_ms", "done_ms", "first_at", "verified_at", "upload_start_at",
                        "upload_end_at", "queued_ms", "nacks", "dups", "rounds", "hops_min", "hops_max", "last_at",
                        "is_demo")

    @_locked
    def transfer_insert(self, **values: Any) -> int:
        bad = set(values) - set(self.TRANSFER_COLUMNS)
        if bad:
            raise ValueError(f"unknown transfer columns {bad}")
        cols = ", ".join(values)
        marks = ", ".join("?" * len(values))
        cur = self.conn.execute(f"INSERT INTO transfers ({cols}) VALUES ({marks})", list(values.values()))
        self.conn.commit()
        return cur.lastrowid

    @_locked
    def transfer_update(self, transfer_id: int, **values: Any) -> None:
        if not values:
            return
        bad = set(values) - set(self.TRANSFER_COLUMNS)
        if bad:
            raise ValueError(f"unknown transfer columns {bad}")
        sets = ", ".join(f"{k} = ?" for k in values)
        self.conn.execute(f"UPDATE transfers SET {sets} WHERE id = ?", list(values.values()) + [transfer_id])
        self.conn.commit()

    @_locked
    def get_transfer(self, transfer_id: int) -> Optional[Dict[str, Any]]:
        row = self.conn.execute("SELECT * FROM transfers WHERE id = ?", (transfer_id,)).fetchone()
        return dict(row) if row else None

    @_locked
    def find_transfer(self, node_id: int, sid: int, is_demo: bool, statuses=None,
                      since: Optional[float] = None) -> Optional[Dict[str, Any]]:
        """Newest transfer with this (node, session id), optionally only with one of statuses and
        last activity at or after since."""
        q = "SELECT * FROM transfers WHERE node_id = ? AND sid = ? AND is_demo = ?"
        args: List[Any] = [node_id, sid, int(is_demo)]
        if statuses:
            q += " AND status IN (%s)" % ", ".join("?" * len(statuses))
            args += list(statuses)
        if since is not None:
            q += " AND last_at >= ?"
            args.append(since)
        row = self.conn.execute(q + " ORDER BY id DESC LIMIT 1", args).fetchone()
        return dict(row) if row else None

    @_locked
    def fragment_record(self, transfer_id: int, seq: int, rnd: int, hops: Optional[int], via: Optional[int],
                        bridge_ms: Optional[int], dup: bool) -> bool:
        """Records one RXF line. The first sighting of a fragment keeps its round/hops/via (which copy
        delivered it); any later sighting only bumps dup_count. Returns True if it was new."""
        cur = self.conn.execute(
            "INSERT OR IGNORE INTO fragments (transfer_id, seq, round, hops, via, bridge_ms, dup_count) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)", (transfer_id, seq, rnd, hops, via, bridge_ms, 1 if dup else 0))
        new = cur.rowcount == 1
        if not new:
            self.conn.execute("UPDATE fragments SET dup_count = dup_count + 1 WHERE transfer_id = ? AND seq = ?",
                              (transfer_id, seq))
        self.conn.commit()
        return new

    @_locked
    def fragments_clear(self, transfer_id: int) -> None:
        self.conn.execute("DELETE FROM fragments WHERE transfer_id = ?", (transfer_id,))
        self.conn.commit()

    @_locked
    def fragments_for(self, transfer_id: int) -> List[Dict[str, Any]]:
        rows = self.conn.execute("SELECT * FROM fragments WHERE transfer_id = ? ORDER BY seq",
                                 (transfer_id,)).fetchall()
        return [dict(r) for r in rows]

    @_locked
    def fragment_summary(self, transfer_id: int) -> Dict[str, Any]:
        """have = distinct fragments; via_counts = fragments per delivering node (0 = unknown);
        rounds = 1 + highest round; hops range."""
        r = self.conn.execute(
            "SELECT COUNT(*) AS have, MAX(round) AS max_round, MIN(hops) AS hmin, MAX(hops) AS hmax "
            "FROM fragments WHERE transfer_id = ?", (transfer_id,)).fetchone()
        via = self.conn.execute(
            "SELECT COALESCE(via, 0) AS v, COUNT(*) AS c FROM fragments WHERE transfer_id = ? GROUP BY v",
            (transfer_id,)).fetchall()
        return {
            "have": r["have"],
            "rounds": (r["max_round"] + 1) if r["max_round"] is not None else None,
            "hops_min": r["hmin"] or None,  # 0 = unknown hops
            "hops_max": r["hmax"] or None,
            "via_counts": {str(x["v"]): x["c"] for x in via},
        }

    @_locked
    def list_transfers(self, limit: int = 20, node_id: Optional[int] = None, radio: Optional[str] = None,
                       status: Optional[str] = None, mode: str = "both") -> List[Dict[str, Any]]:
        q, where, args = "SELECT * FROM transfers", [], []
        if node_id is not None:
            where.append("node_id = ?")
            args.append(node_id)
        if radio:
            where.append("radio = ?")
            args.append(radio)
        if status:
            where.append("status = ?")
            args.append(status)
        if mode == "real":
            where.append("is_demo = 0")
        elif mode == "sim":
            where.append("is_demo = 1")
        if where:
            q += " WHERE " + " AND ".join(where)
        q += " ORDER BY id DESC LIMIT ?"
        args.append(limit)
        return [dict(r) for r in self.conn.execute(q, args).fetchall()]

    @_locked
    def open_transfers(self) -> List[Dict[str, Any]]:
        rows = self.conn.execute("SELECT * FROM transfers WHERE status = 'in_progress' ORDER BY id").fetchall()
        return [dict(r) for r in rows]

    @_locked
    def transfers_for_capture(self, capture_row_id: int) -> List[Dict[str, Any]]:
        rows = self.conn.execute("SELECT * FROM transfers WHERE capture_row_id = ? ORDER BY id",
                                 (capture_row_id,)).fetchall()
        return [dict(r) for r in rows]
