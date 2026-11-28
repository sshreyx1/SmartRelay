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

import json
import logging
import sqlite3
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

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
