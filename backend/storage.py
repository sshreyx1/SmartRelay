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
"""
from __future__ import annotations

import json
import sqlite3
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

CREATE TABLE IF NOT EXISTS mesh_log (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    line        TEXT NOT NULL,
    received_at REAL NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_captures_node ON captures(node_id);
CREATE INDEX IF NOT EXISTS idx_captures_received ON captures(received_at);
"""


class Store:
    def __init__(self, db_path: str, captures_dir: str):
        self.db_path = db_path
        self.captures_dir = Path(captures_dir)
        self.captures_dir.mkdir(parents=True, exist_ok=True)
        self.conn = sqlite3.connect(db_path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.conn.executescript(SCHEMA)
        self.conn.commit()

    def close(self) -> None:
        self.conn.close()

    # -- writes -------------------------------------------------------------

    def save_capture_image(self, node_id: int, session_id: int, data: bytes, ext: str = ".jpg",
                           is_demo: bool = False) -> str:
        prefix = "demo_" if is_demo else ""
        name = f"{prefix}n{node_id}_{session_id:05d}{ext}"
        path = self.captures_dir / name
        path.write_bytes(data)
        return str(path)

    def record_capture(self, node_id: int, session_id: int, radio: str, frag_total: int,
                       size_bytes: int, crc32: str, image_path: str, meta: Dict[str, Any],
                       is_demo: bool = False) -> int:
        cur = self.conn.execute(
            """INSERT INTO captures
               (node_id, capture_id, session_id, radio, frag_total, size_bytes, crc32,
                image_path, received_at, uptime_ms, trigger_pir, trigger_sound,
                temperature_c, humidity_pct, light_raw, weather_label, weather_name,
                trigger_trust, sound_class, sound_name, sound_rms, sound_zcr, image_index,
                is_demo)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (
                node_id, meta.get("capture_id"), session_id, radio, frag_total, size_bytes, crc32,
                image_path, time.time(), meta.get("uptime_ms"),
                int(bool(meta.get("trigger_pir"))), int(bool(meta.get("trigger_sound"))),
                meta.get("temperature_c"), meta.get("humidity_pct"), meta.get("light_raw"),
                meta.get("weather_label"), meta.get("weather_name"), meta.get("trigger_trust"),
                meta.get("sound_class"), meta.get("sound_name"), meta.get("sound_rms"),
                meta.get("sound_zcr"), meta.get("image_index"),
                int(is_demo),
            ),
        )
        self.conn.commit()
        return cur.lastrowid

    def log_line(self, line: str) -> None:
        self.conn.execute("INSERT INTO mesh_log (line, received_at) VALUES (?, ?)", (line, time.time()))
        self.conn.commit()

    # -- reads ----------------------------------------------------------

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

    def get_capture(self, capture_row_id: int) -> Optional[Dict[str, Any]]:
        row = self.conn.execute("SELECT * FROM captures WHERE id = ?", (capture_row_id,)).fetchone()
        return dict(row) if row else None

    def node_last_seen(self) -> Dict[int, Dict[str, Any]]:
        """Last REAL (non-demo) capture per node -- this is the only honest
        signal the bridge gives us about a node being alive, since the
        firmware does not broadcast a periodic heartbeat to the laptop."""
        rows = self.conn.execute(
            """SELECT node_id, MAX(received_at) AS last_seen, radio
               FROM captures WHERE is_demo = 0 GROUP BY node_id"""
        ).fetchall()
        return {r["node_id"]: {"last_seen": r["last_seen"], "radio": r["radio"]} for r in rows}

    def recent_log(self, limit: int = 200) -> List[Dict[str, Any]]:
        rows = self.conn.execute(
            "SELECT * FROM mesh_log ORDER BY id DESC LIMIT ?", (limit,)
        ).fetchall()
        return [dict(r) for r in reversed(rows)]

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
