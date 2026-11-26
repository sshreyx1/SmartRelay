"""
live_bridge.py - real-time version of tools/bridge_receiver.py for the backend.

Reuses the exact wire-format decoder from tools/bridge_receiver.py (decode_meta,
parse_header) so there is ONE place that understands the bridge's serial protocol
documented in common/uplink.h:

    @ALFP FILE v1 src=3 sid=00042 size=8000 crc=1a2b3c4d radio=lora frags=34 meta=<hex>
    @D <base64, 64 chars per line>
    ...
    @ALFP END crc=1a2b3c4d

Transfer events (TRANSFER_VISIBILITY.md 3.3) are one line each, interleaved with everything else:

    @EVT RXS src=5 sid=42 radio=wifi t=1234567
    @EVT RXF src=5 sid=42 seq=17 r=0 h=2 v=3 d=0 t=1234890      (RXM, NACK, RXOK, RXFAIL likewise)

They go to on_event(kind, fields, is_demo) and are NOT passed to on_line (they would flood the log).
BridgeClock turns their bridge-ms "t" into laptop time.

This module adds nothing to the protocol decode itself -- it just wraps
tools/bridge_receiver.py's Receiver with a callback so the backend can store
each verified transfer and push it to connected dashboards the moment it
completes, instead of (or in addition to) writing files to disk.

Two ways to feed it, both real:
  - LiveBridge.run_serial(port)   reads an actual bridge node over USB serial
  - LiveBridge.feed_lines(lines)  replays a saved log, or is used by the demo
                                  simulator's text (see simulator/demo_feed.py) --
                                  the simulator still goes through this exact
                                  parser, it does not get a shortcut into the DB.
"""
from __future__ import annotations

import base64
import logging
import os
import sys
import threading
import time
import zlib
from dataclasses import dataclass, field
from typing import Callable, Optional

# tools/bridge_receiver.py is the single source of truth for the wire format.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from bridge_receiver import decode_meta, parse_header  # noqa: E402

log = logging.getLogger("smartrelay.bridge")


@dataclass
class CompletedTransfer:
    src: int
    session_id: int
    radio: str
    frag_total: int
    size: int
    crc32: str
    meta: dict
    data: bytes
    received_at: float = field(default_factory=time.time)
    is_demo: bool = False
    hdr: dict = field(default_factory=dict)  # the raw @ALFP FILE fields (first= done= nacks= dups= ...)


def parse_event(line: str):
    """'@EVT RXF src=5 sid=42 seq=17 ...' -> ('RXF', {'src': 5, 'sid': 42, 'seq': 17, ...}).
    Values that are plain decimal numbers become ints; everything else (radio, why, meta hex, crc)
    stays a string. Returns None for a line that is not a well-formed event."""
    parts = line.split()
    if len(parts) < 2 or parts[0] != "@EVT":
        return None
    fields = {}
    for part in parts[2:]:
        if "=" not in part:
            continue
        k, v = part.split("=", 1)
        fields[k] = int(v) if v.isdigit() and k not in ("crc", "meta") else v
    return parts[1], fields


class BridgeClock:
    """Maps the bridge's ms-since-boot onto laptop time (TRANSFER_VISIBILITY.md 4.2).

    Every event line gives one sample: offset = laptop_time - t/1000. USB delay only ever makes that
    larger, so the smallest offset seen since the bridge's last boot is the best estimate. If t goes
    backwards the bridge rebooted (its clock restarted): the estimate starts over."""

    REBOOT_SLACK_MS = 2000  # a line a little older than the newest one is reordering, not a reboot

    def __init__(self) -> None:
        self.offset: Optional[float] = None
        self.last_t: Optional[int] = None
        self.reboots = 0

    def update(self, t_ms: int, laptop_now: float) -> bool:
        """Feed one sample. Returns True if this sample showed a bridge reboot."""
        rebooted = self.last_t is not None and t_ms + self.REBOOT_SLACK_MS < self.last_t
        if rebooted:
            self.offset = None
            self.last_t = None
            self.reboots += 1
        if self.last_t is None or t_ms > self.last_t:
            self.last_t = t_ms
        sample = laptop_now - t_ms / 1000.0
        if self.offset is None or sample < self.offset:
            self.offset = sample
        return rebooted

    def to_laptop(self, t_ms: Optional[int]) -> Optional[float]:
        if t_ms is None or self.offset is None:
            return None
        return t_ms / 1000.0 + self.offset


class LiveBridge:
    """Parses the bridge's serial text stream and calls on_transfer for every
    file that passes its CRC-32 check -- exactly what tools/bridge_receiver.py
    does, plus a callback instead of (only) writing to disk."""

    def __init__(self, on_transfer: Callable[[CompletedTransfer], None],
                 on_line: Optional[Callable[[str], None]] = None,
                 on_event: Optional[Callable[[str, dict, bool], None]] = None,
                 on_upload: Optional[Callable[[str, dict, bool], None]] = None,
                 on_status: Optional[Callable[[bool, Optional[str]], None]] = None):
        """on_event(kind, fields, is_demo): one @EVT line (fields["t"] is bridge ms).
        on_upload(stage, hdr, is_demo): "start" at @ALFP FILE, "rejected" when the laptop's CRC
        re-check fails, "aborted" on @ALFP ABORT. (A file that passes goes to on_transfer.)
        on_status(connected, port): the serial link to the bridge came up or went down."""
        self._on_transfer = on_transfer
        self._on_line = on_line
        self._on_event = on_event
        self._on_upload = on_upload
        self._on_status = on_status
        self._cur: Optional[dict] = None
        self._serial_thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self.port: Optional[str] = None
        self.connected = False
        self.since: Optional[float] = None  # when `connected` last changed

    # -- feeding ----------------------------------------------------------

    def feed_line(self, line: str, is_demo: bool = False) -> None:
        line = line.rstrip("\r\n")
        if line.startswith("@EVT "):
            ev = parse_event(line)
            if ev is not None and self._on_event:
                self._on_event(ev[0], ev[1], is_demo)
        elif line.startswith("@ALFP FILE"):
            self._cur = {"hdr": parse_header(line), "data": bytearray(), "is_demo": is_demo}
            if self._on_upload:
                self._on_upload("start", self._cur["hdr"], is_demo)
        elif line.startswith("@D ") and self._cur is not None:
            self._cur["data"] += base64.b64decode(line[3:])
        elif line.startswith("@ALFP END") and self._cur is not None:
            self._finish()
        elif line.startswith("@ALFP ABORT"):
            log.warning("bridge aborted a transfer: %s", line)
            if self._cur is not None and self._on_upload:
                self._on_upload("aborted", self._cur["hdr"], self._cur.get("is_demo", False))
            self._cur = None
        else:
            if self._on_line:
                self._on_line(line)

    def feed_lines(self, lines, is_demo: bool = False) -> None:
        for line in lines:
            self.feed_line(line, is_demo=is_demo)

    def _finish(self) -> None:
        hdr, data = self._cur["hdr"], bytes(self._cur["data"])
        is_demo = self._cur.get("is_demo", False)
        self._cur = None
        want = int(hdr.get("crc", "0"), 16)
        got = zlib.crc32(data) & 0xFFFFFFFF
        size_ok = len(data) == int(hdr.get("size", "-1"))
        if got != want or not size_ok:
            log.warning("REJECTED transfer src=%s sid=%s: crc %08x != %08x or size mismatch",
                        hdr.get("src"), hdr.get("sid"), got, want)
            if self._on_upload:
                self._on_upload("rejected", hdr, is_demo)
            return
        meta = decode_meta(bytes.fromhex(hdr.get("meta", "")))
        xfer = CompletedTransfer(
            src=int(hdr["src"]),
            session_id=int(hdr["sid"]),
            radio=hdr.get("radio", "?"),
            frag_total=int(hdr.get("frags", "0")),
            size=len(data),
            crc32=f"{got:08x}",
            meta=meta if isinstance(meta, dict) else {},
            data=data,
            is_demo=is_demo,
            hdr=hdr,
        )
        self._on_transfer(xfer)

    # -- real serial port (the normal, automatic path) ---------------------

    def run_serial(self, port: str, baud: int = 115200) -> None:
        """Blocking read loop. Run this in a background thread."""
        try:
            import serial  # pyserial
        except ImportError:
            log.error("pyserial not installed (pip install pyserial) -- "
                      "cannot read the bridge node over USB serial")
            return
        self.port = port
        backoff = 1.0
        while not self._stop.is_set():
            try:
                with serial.Serial(port, baud, timeout=1) as s:
                    self.connected = True
                    self.since = time.time()
                    if self._on_status:
                        self._on_status(True, port)
                    backoff = 1.0
                    log.info("connected to bridge on %s", port)
                    while not self._stop.is_set():
                        raw = s.readline()
                        if raw:
                            self.feed_line(raw.decode("ascii", errors="replace"))
            except Exception as exc:
                if self.connected:
                    self.since = time.time()
                    if self._on_status:
                        self._on_status(False, port)
                self.connected = False
                self._cur = None  # a file cut off by the disconnect can never finish
                log.warning("serial connection to %s lost/unavailable (%s), retrying in %.0fs",
                            port, exc, backoff)
                time.sleep(backoff)
                backoff = min(backoff * 1.5, 15.0)

    def start_serial_background(self, port: str, baud: int = 115200) -> None:
        self._stop.clear()
        self._serial_thread = threading.Thread(
            target=self.run_serial, args=(port, baud), daemon=True, name="bridge-serial"
        )
        self._serial_thread.start()

    def stop(self) -> None:
        self._stop.set()


def autodetect_port() -> Optional[str]:
    """Best-effort: find a likely Pico serial port so the backend can connect
    with zero configuration on the common case. Returns None if unsure --
    the caller should fall back to asking for SMARTRELAY_BRIDGE_PORT."""
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    candidates = []
    for p in list_ports.comports():
        name = (p.description or "") + " " + (p.manufacturer or "")
        if "pico" in name.lower() or (p.vid == 0x2E8A):  # Raspberry Pi Foundation VID
            candidates.append(p.device)
    return candidates[0] if candidates else None
