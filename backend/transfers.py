"""
transfers.py - live view of ALFP transfers as the bridge (node 1) receives them.

docs/PROTOCOLS.md. The bridge prints one "@EVT ..." line per receive event
(firmware/app/main.c); live_bridge.LiveBridge parses them and hands them to TransferTracker, which
keeps the `transfers` and `fragments` tables (storage.py) and tells the dashboards over WebSocket.

    RXS    -> new transfer row, status in_progress           (transfer_start)
    RXM    -> layout, sender's CRC, decoded capture metadata  (transfer_progress)
    RXF    -> one fragment: round, hops, delivering relay     (transfer_progress, <= 4/s per transfer)
    NACK   -> nacks + 1, and a readable log line
    RXOK   -> verified on the bridge                          (transfer_done)
    RXFAIL -> failed with the bridge's reason, log line       (transfer_failed); "crc" is not final
    @ALFP FILE / END (via app.py) -> upload to the laptop, the laptop's own CRC check, capture link

Only what the bridge reported is stored. A transfer the backend never saw start (old firmware, or
started mid-transfer) still gets a row from its @ALFP FILE header, with the live fields left NULL.
"""
from __future__ import annotations

import logging
import threading
import time
from typing import Any, Callable, Dict, Optional, Tuple

from live_bridge import BridgeClock
from storage import Store

# tools/bridge_receiver.py (already on sys.path through live_bridge) is the one meta decoder
from bridge_receiver import TELEMETRY_MARKER, decode_meta  # noqa: E402

log = logging.getLogger("smartrelay.transfers")

OPEN = "in_progress"
TIMEOUT_S = 180.0          # an open transfer with no event for this long is marked failed / timeout
PROGRESS_MIN_GAP_S = 0.25  # at most 4 transfer_progress events per second per transfer
RECENT_S = 300.0           # a refusal for the same (node, sid) within this window updates one row

Emit = Callable[[str, Dict[str, Any]], None]
LogLine = Callable[[str], None]


def kind_of(meta: Dict[str, Any]) -> str:
    """'telemetry' (the TELEMETRY marker), 'image' (a capture record) or 'test' (no capture record,
    e.g. a size-ladder test payload)."""
    if not isinstance(meta, dict) or "node_id" not in meta:
        return "test"
    return "telemetry" if meta.get("original_name") == TELEMETRY_MARKER else "image"


def meta_fields(meta_hex: Optional[str]) -> Dict[str, Any]:
    """capture_id / kind / queued_ms from a meta hex string, or {} if there is none."""
    if meta_hex is None:
        return {}
    try:
        meta = decode_meta(bytes.fromhex(meta_hex))
    except ValueError:
        return {}
    out: Dict[str, Any] = {"kind": kind_of(meta)}
    if "node_id" in meta:
        out["capture_id"] = meta.get("capture_id")
        out["queued_ms"] = meta.get("queued_ms")
    return out


class TransferTracker:
    def __init__(self, store: Store, emit: Emit, log_line: Optional[LogLine] = None,
                 now: Callable[[], float] = time.time) -> None:
        self.store = store
        self.emit = emit
        self.log_line = log_line or (lambda _line: None)
        self.now = now
        self.clocks = {False: BridgeClock(), True: BridgeClock()}  # real bridge, simulator
        self._last_progress: Dict[int, float] = {}
        self._last_frag: Dict[int, Tuple[Optional[int], Optional[int]]] = {}
        self._lock = threading.RLock()

    # -- helpers ---------------------------------------------------------------------------------

    def _open(self, node: int, sid: int, demo: bool) -> Optional[Dict[str, Any]]:
        return self.store.find_transfer(node, sid, demo, statuses=(OPEN,))

    def _create(self, node: int, sid: int, demo: bool, **values: Any) -> Dict[str, Any]:
        tid = self.store.transfer_insert(node_id=node, sid=sid, is_demo=int(demo), status=OPEN, nacks=0, dups=0,
                                         last_at=self.now(), **values)
        return self.store.get_transfer(tid)

    def _open_or_create(self, node: int, sid: int, demo: bool) -> Dict[str, Any]:
        """The open transfer for (node, sid); if the backend missed its RXS, a new row whose start
        stays unknown (first_ms / first_at NULL)."""
        return self._open(node, sid, demo) or self._create(node, sid, demo)

    def _elapsed(self, row: Dict[str, Any], t_ms: Optional[int]) -> Optional[float]:
        if row.get("first_ms") is None or t_ms is None:
            return None
        return round((t_ms - row["first_ms"]) / 1000.0, 3)

    def _progress(self, tid: int, t_ms: Optional[int], force: bool = False) -> None:
        now = self.now()
        if not force and now - self._last_progress.get(tid, 0.0) < PROGRESS_MIN_GAP_S:
            return
        self._last_progress[tid] = now
        row = self.store.get_transfer(tid)
        summ = self.store.fragment_summary(tid)
        last_hops, last_via = self._last_frag.get(tid, (None, None))
        self.emit("transfer_progress", {
            "transfer_id": tid, "node_id": row["node_id"], "sid": row["sid"], "radio": row["radio"],
            "have": summ["have"], "frag_count": row["frag_count"], "kind": row["kind"],
            "capture_id": row["capture_id"], "last_hops": last_hops, "last_via": last_via,
            "via_counts": summ["via_counts"], "nacks": row["nacks"], "dups": row["dups"],
            "elapsed_s": self._elapsed(row, t_ms), "is_demo": bool(row["is_demo"]),
        })

    # -- @EVT lines --------------------------------------------------------------------------------

    def on_event(self, kind: str, f: Dict[str, Any], is_demo: bool) -> None:
        with self._lock:
            try:
                self._on_event(kind, f, is_demo)
            except Exception:  # one bad line must never stop the serial reader
                log.exception("could not handle @EVT %s %s", kind, f)

    def _on_event(self, kind: str, f: Dict[str, Any], demo: bool) -> None:
        node, sid, t = f.get("src"), f.get("sid"), f.get("t")
        if not isinstance(node, int) or not isinstance(sid, int):
            return
        clock = self.clocks[demo]
        if isinstance(t, int):
            if clock.update(t, self.now()):
                log.info("%sbridge clock went backwards: bridge rebooted", "[DEMO] " if demo else "")
        else:
            t = None
        at = clock.to_laptop(t)
        handler = getattr(self, "_ev_" + kind.lower(), None)
        if handler is not None:
            handler(node, sid, demo, f, t, at)

    def _ev_rxs(self, node, sid, demo, f, t, at) -> None:
        old = self._open(node, sid, demo)
        if old is not None:  # the bridge opened this session again (it never said the old one ended)
            self._fail(old, "restarted")
        row = self._create(node, sid, demo, radio=f.get("radio"), first_ms=t, first_at=at)
        self.emit("transfer_start", {"transfer_id": row["id"], "node_id": node, "sid": sid, "radio": row["radio"],
                                     "first_at": at, "is_demo": demo})

    def _ev_rxm(self, node, sid, demo, f, t, at) -> None:
        row = self._open_or_create(node, sid, demo)
        upd = {"frag_count": f.get("frags"), "frag_size": f.get("fsz"), "size_bytes": f.get("size"),
               "crc_sender": f.get("crc"), "last_at": self.now()}
        upd.update(meta_fields(f.get("meta", "")))
        self.store.transfer_update(row["id"], **upd)
        self._progress(row["id"], t, force=True)

    def _ev_rxf(self, node, sid, demo, f, t, at) -> None:
        row = self._open_or_create(node, sid, demo)
        hops = f.get("h") or None  # 0 = unknown
        via = f.get("v") or None   # 0 = unknown (always on LoRa)
        dup = bool(f.get("d"))
        self.store.fragment_record(row["id"], f.get("seq", 0), f.get("r", 0), hops, via, t, dup)
        summ = self.store.fragment_summary(row["id"])
        self.store.transfer_update(row["id"], dups=(row["dups"] or 0) + (1 if dup else 0), rounds=summ["rounds"],
                                   hops_min=summ["hops_min"], hops_max=summ["hops_max"], last_at=self.now())
        self._last_frag[row["id"]] = (hops, via)
        done = row["frag_count"] is not None and summ["have"] >= row["frag_count"]
        self._progress(row["id"], t, force=done)

    def _ev_nack(self, node, sid, demo, f, t, at) -> None:
        row = self._open_or_create(node, sid, demo)
        self.store.transfer_update(row["id"], nacks=(row["nacks"] or 0) + 1, last_at=self.now())
        self.log_line(f"{'[DEMO] ' if demo else ''}node {node} session {sid}: bridge asked for "
                      f"{f.get('miss', '?')} missing fragment(s) (NACK {f.get('r', '?')})")
        self._progress(row["id"], t, force=True)

    def _ev_rxok(self, node, sid, demo, f, t, at) -> None:
        row = self._open_or_create(node, sid, demo)
        summ = self.store.fragment_summary(row["id"])
        first_ms = f.get("first", row["first_ms"])
        clock = self.clocks[demo]
        # both ends through the same (best so far) offset, so verified_at - first_at = done - first
        first_at = clock.to_laptop(first_ms)
        self.store.transfer_update(
            row["id"], status="verified", crc_bridge_ok=1, first_ms=first_ms,
            first_at=first_at if first_at is not None else row["first_at"],
            done_ms=f.get("done", t), verified_at=clock.to_laptop(f.get("done", t)),
            nacks=f.get("nacks", row["nacks"]), dups=f.get("dups", row["dups"]),
            frag_count=f.get("frags", row["frag_count"]), rounds=summ["rounds"], last_at=self.now())
        self._last_progress.pop(row["id"], None)
        self._last_frag.pop(row["id"], None)
        self.emit("transfer_done", self.transfer_view(row["id"]))

    def _ev_rxfail(self, node, sid, demo, f, t, at) -> None:
        why = str(f.get("why", "unknown"))
        tag = "[DEMO] " if demo else ""
        if why == "crc":
            # Not final: the bridge threw the data away and the sender resends it all in this session.
            row = self._open_or_create(node, sid, demo)
            self.store.fragments_clear(row["id"])
            self.store.transfer_update(row["id"], last_at=self.now())
            self.log_line(f"{tag}node {node} session {sid}: file CRC-32 mismatch on the bridge, "
                          f"whole file requested again")
            return
        self.log_line(f"{tag}node {node} session {sid}: transfer failed on the bridge ({why})")
        row = self._open(node, sid, demo)
        if row is None:
            # refused before a session opened (busy, too_big, storage): one row per (node, sid), not one
            # per retry
            row = self.store.find_transfer(node, sid, demo, statuses=("failed",), since=self.now() - RECENT_S)
            if row is not None:
                self.store.transfer_update(row["id"], fail_reason=why, last_at=self.now())
                return
            row = self._create(node, sid, demo)
        self._fail(row, why)

    def _fail(self, row: Dict[str, Any], why: str) -> None:
        self.store.transfer_update(row["id"], status="failed", fail_reason=why, last_at=self.now())
        self._last_progress.pop(row["id"], None)
        self._last_frag.pop(row["id"], None)
        self.emit("transfer_failed", {"transfer_id": row["id"], "reason": why, "node_id": row["node_id"],
                                      "sid": row["sid"], "is_demo": bool(row["is_demo"])})

    # -- the @ALFP FILE upload (bridge -> laptop) --------------------------------------------------

    def _upload_row(self, hdr: Dict[str, str], demo: bool) -> Optional[Dict[str, Any]]:
        try:
            node, sid = int(hdr["src"]), int(hdr["sid"])
        except (KeyError, ValueError):
            return None
        row = self.store.find_transfer(node, sid, demo, statuses=("verified", OPEN))
        if row is not None:
            return row
        # No RXS/RXOK seen (old firmware, or the backend started late): the bridge only uploads files
        # it verified, so the row starts as verified; live fields stay NULL ("Data unavailable").
        def num(k):
            v = hdr.get(k)
            return int(v) if v is not None and v.isdigit() else None
        values = {"radio": hdr.get("radio"), "size_bytes": num("size"), "frag_count": num("frags"),
                  "crc_sender": hdr.get("crc"), "first_ms": num("first"), "done_ms": num("done"),
                  "nacks": num("nacks"), "dups": num("dups"), "crc_bridge_ok": 1}
        values.update(meta_fields(hdr.get("meta", "")))
        tid = self.store.transfer_insert(node_id=node, sid=sid, is_demo=int(demo), status="verified",
                                         last_at=self.now(), **values)
        return self.store.get_transfer(tid)

    def on_upload(self, stage: str, hdr: Dict[str, str], is_demo: bool) -> None:
        with self._lock:
            try:
                row = self._upload_row(hdr, is_demo)
                if row is None:
                    return
                if stage == "start":
                    self.store.transfer_update(row["id"], upload_start_at=self.now(), last_at=self.now())
                elif stage == "rejected":
                    self.store.transfer_update(row["id"], upload_end_at=self.now(), crc_laptop_ok=0)
                    self.emit("transfer_done", self.transfer_view(row["id"]))
                elif stage == "aborted":
                    self.store.transfer_update(row["id"], upload_end_at=self.now())
            except Exception:
                log.exception("could not record upload %s %s", stage, hdr)

    def on_file_saved(self, hdr: Dict[str, str], is_demo: bool, capture_row_id: int) -> Optional[Dict[str, Any]]:
        """The laptop re-checked the file's CRC-32 and stored it as capture_row_id."""
        with self._lock:
            row = self._upload_row(hdr, is_demo)
            if row is None:
                return None
            self.store.transfer_update(row["id"], upload_end_at=self.now(), crc_laptop_ok=1,
                                       capture_row_id=capture_row_id)
            view = self.transfer_view(row["id"])
            self.emit("transfer_done", view)
            return view

    # -- housekeeping and views --------------------------------------------------------------------

    def sweep(self) -> int:
        """Open transfers with no activity for TIMEOUT_S become failed / timeout. Returns how many."""
        with self._lock:
            n = 0
            cutoff = self.now() - TIMEOUT_S
            for row in self.store.open_transfers():
                if (row["last_at"] or 0) < cutoff:
                    self._fail(row, "timeout")
                    n += 1
            return n

    def transfer_view(self, tid: int, with_fragments: bool = False) -> Optional[Dict[str, Any]]:
        row = self.store.get_transfer(tid)
        if row is None:
            return None
        row["is_demo"] = bool(row["is_demo"])
        summ = self.store.fragment_summary(tid)
        row["have"] = summ["have"]
        row["via_counts"] = summ["via_counts"]
        # derived durations, only where both ends were reported (None = not measured)
        row["radio_s"] = (round((row["done_ms"] - row["first_ms"]) / 1000.0, 3)
                          if row["done_ms"] is not None and row["first_ms"] is not None else None)
        row["upload_s"] = (round(row["upload_end_at"] - row["upload_start_at"], 3)
                           if row["upload_end_at"] is not None and row["upload_start_at"] is not None else None)
        if with_fragments:
            frags = self.store.fragments_for(tid)
            first = row["first_ms"]
            row["fragments"] = [{
                "seq": fr["seq"], "round": fr["round"], "hops": fr["hops"], "via": fr["via"],
                "t_rel_s": (round((fr["bridge_ms"] - first) / 1000.0, 3)
                            if fr["bridge_ms"] is not None and first is not None else None),
                "dup_count": fr["dup_count"],
            } for fr in frags]
        return row

    def in_flight(self):
        out = []
        for row in self.store.open_transfers():
            view = self.transfer_view(row["id"])
            if view is not None:
                out.append(view)
        return out
