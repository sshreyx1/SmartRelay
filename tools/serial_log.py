#!/usr/bin/env python3
"""
serial_log.py - timestamped serial logger for the hardware tests (docs/TESTING.md).

Echoes a node's serial output with a time stamp (seconds since start) and writes the same lines to
docs/sound/evidence/<test>_<date>.log. Keys can be scripted with --send (each KEY@SECONDS is sent once at that
time), and while it runs you can type a line and press Enter to send it (e.g. `c`, `H`, `s`). Stops after
--duration seconds or Ctrl+C, then prints how often each --count pattern appeared.

Cross-platform (pyserial only). Close every other serial monitor on that port first.

Run:      python tools/serial_log.py --test T6
          python tools/serial_log.py --test T6 --send H@2 c@5 c@20 c@35 c@50 c@65 c@80 H@100 s@120 --duration 140 \\
                 --count "capture queue full, dropped" "queued (" "[ENV]"
          python tools/serial_log.py --port COM5 --test T3 --duration 300
          python tools/serial_log.py --identify      (which port is which node: sends 's' to every Pico)
"""
import argparse
import queue
import re
import sys
import threading
import time
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs" / "sound" / "evidence"
PICO_VID = 0x2E8A


def find_pico_port() -> str:
    from serial.tools import list_ports
    picos = sorted(p.device for p in list_ports.comports() if p.vid == PICO_VID)
    if not picos:
        sys.exit("error: no Pico found (USB VID 0x2E8A); pass --port")
    if len(picos) > 1:
        print(f"note: several Picos {picos}, using {picos[0]} (pass --port to choose)")
    return picos[0]


def identify() -> int:
    """Send 's' to every Pico and print which node answers on which port (node firmware prints 'node N | ...';
    audio_dump/sensor_test print something else). Close the backend and every monitor first."""
    import serial
    from serial.tools import list_ports
    ports = sorted(p.device for p in list_ports.comports() if p.vid == PICO_VID)
    if not ports:
        print("no Pico found (USB VID 0x2E8A)")
        return 1
    for dev in ports:
        try:
            with serial.Serial(dev, 115200, timeout=0.2) as ser:
                ser.reset_input_buffer()
                ser.write(b"s")
                text, t_end = b"", time.monotonic() + 2.0
                while time.monotonic() < t_end:
                    text += ser.read(512)
            t = text.decode("utf-8", "replace")
            m = re.search(r"node (\d+) \|", t)
            what = (f"node {m.group(1)}" + (" (BRIDGE)" if m.group(1) == "1" else "")) if m else \
                "not node firmware (audio_dump / sensor_test?) or no answer"
        except Exception as e:  # port busy (backend or a monitor has it open)
            what = f"cannot open: {e}"
        print(f"{dev:<28} {what}")
    return 0


def parse_send(items: list) -> list:
    """['c@5', 'H@2.5'] -> [(2.5, 'H'), (5.0, 'c')]"""
    out = []
    for it in items:
        m = re.fullmatch(r"(.+)@(\d+(?:\.\d+)?)", it, re.S)
        if not m:
            sys.exit(f"error: --send {it!r} is not KEY@SECONDS")
        out.append((float(m.group(2)), m.group(1)))
    return sorted(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Timestamped serial logger with scripted keys, for the hardware tests.")
    ap.add_argument("--port", help="serial port (default: the first Pico, USB VID 0x2E8A)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--test", default="serial", help="test name, used in the log file name (e.g. T3, T6)")
    ap.add_argument("--out", type=Path, help="log file (default docs/sound/evidence/<test>_<date>.log)")
    ap.add_argument("--send", nargs="*", default=[], metavar="KEY@SEC", help="keys to send at given seconds")
    ap.add_argument("--duration", type=float, help="stop after this many seconds (default: until Ctrl+C)")
    ap.add_argument("--count", nargs="*", default=[], metavar="TEXT", help="count lines containing TEXT at the end")
    ap.add_argument("--dry-run", action="store_true", help="check the arguments and print the plan, open no port")
    ap.add_argument("--identify", action="store_true", help="list every Pico port and which node it is, then exit")
    args = ap.parse_args()
    if args.identify:
        return identify()

    plan = parse_send(args.send)
    out = args.out or EVIDENCE / f"{args.test}_{date.today().isoformat()}.log"
    if args.dry_run:
        print(f"would log to {out}; send {plan}; duration {args.duration}; count {args.count}")
        return 0

    import serial
    port = args.port or find_pico_port()
    out.parent.mkdir(parents=True, exist_ok=True)
    typed: "queue.Queue[str]" = queue.Queue()
    threading.Thread(target=lambda: [typed.put(ln.rstrip("\r\n")) for ln in sys.stdin], daemon=True).start()
    counts = {c: 0 for c in args.count}
    t0 = time.monotonic()

    with serial.serial_for_url(port, args.baud, timeout=0.1) as ser, open(out, "a", encoding="utf-8", newline="\n") as log:
        def note(text: str) -> None:
            line = f"{time.monotonic() - t0:8.2f}  {text}"
            print(line, flush=True)
            log.write(line + "\n")
            log.flush()

        note(f"--- serial_log {args.test} on {port} at {time.strftime('%Y-%m-%d %H:%M:%S')} ---")
        buf = b""
        try:
            while args.duration is None or time.monotonic() - t0 < args.duration:
                now = time.monotonic() - t0
                while plan and plan[0][0] <= now:
                    key = plan.pop(0)[1]
                    ser.write(key.encode())
                    note(f">>> sent {key!r}")
                while not typed.empty():
                    key = typed.get()
                    if key:
                        ser.write(key.encode())
                        note(f">>> sent {key!r} (typed)")
                buf += ser.read(ser.in_waiting or 1)
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    text = raw.decode("utf-8", "replace").rstrip("\r")
                    note(text)
                    for c in counts:
                        counts[c] += c in text
        except KeyboardInterrupt:
            note("--- stopped (Ctrl+C) ---")
        if counts:
            note("--- counts: " + ", ".join(f"{c!r} x{n}" for c, n in counts.items()) + " ---")
    print(f"log: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
