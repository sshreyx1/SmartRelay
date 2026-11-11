"""key_run.py - drive one node's console on a fixed key schedule and keep the evidence.

  python key_run.py --port /dev/cu.usbmodemMTK3PICO1 --label t6 --secs 75 --keys "2:s,5:x,65:s"

Writes <out>/<label>_log.txt (every line, host-timestamped, plus the keys sent) and
<out>/<label>_results.txt (a summary of every 's' block, then the full log). Paste the results file.
"""
import argparse
import os
import re
import sys
import time

import serial

PATTERNS = {
    "capture q full": re.compile(r"capture q full (\d+)"),
    "log dropped": re.compile(r"log dropped (\d+) lines"),
    "usb dropped": re.compile(r"usb dropped (\d+) lines"),
    "longest comms loop ms": re.compile(r"longest comms loop (\d+) ms"),
    "heap used B": re.compile(r"heap used (\d+) B"),
    "outbox": re.compile(r"\| outbox (\d+)$"),
    "dht22 fail": re.compile(r"dht22 good \d+ fail (\d+)"),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--secs", type=float, required=True)
    ap.add_argument("--keys", required=True, help='"t:k,t:k" e.g. "2:s,5:x,65:s"')
    ap.add_argument("--out", default=os.path.dirname(os.path.abspath(__file__)))
    a = ap.parse_args()

    keys = sorted((float(t), k) for t, k in (item.split(":", 1) for item in a.keys.split(",") if item))
    try:
        ser = serial.Serial(a.port, 115200, timeout=0)
    except Exception as e:
        print(f"cannot open {a.port}: {e}")
        sys.exit(1)
    try:
        ser.dtr = True
    except Exception:
        pass

    rows = []  # (t, kind, text) kind: 'rx' or 'key'
    buf = b""
    t0 = time.time()
    print(f"[{a.label}] recording {a.secs:.0f} s, keys {keys}; do not touch the board", flush=True)
    while time.time() - t0 < a.secs:
        t = time.time() - t0
        while keys and t >= keys[0][0]:
            k = keys.pop(0)[1]
            ser.write(k.encode())
            rows.append((t, "key", f">>> host sent '{k}'"))
            print(f"{t:8.3f} >>> host sent '{k}'", flush=True)
        buf += ser.read(4096)
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            rows.append((time.time() - t0, "rx", raw.decode("utf-8", "replace").rstrip()))
        time.sleep(0.005)
    ser.close()

    os.makedirs(a.out, exist_ok=True)
    log_lines = [f"{t:8.3f} {x}" for t, _, x in rows]
    with open(os.path.join(a.out, f"{a.label}_log.txt"), "w") as f:
        f.write("\n".join(log_lines) + "\n")

    # one entry per stats block: values of each pattern, keyed by the block's first line time
    blocks = []
    for t, kind, x in rows:
        if kind != "rx":
            continue
        if re.match(r"node \d+ \| alfp in", x):
            blocks.append({"t": t})
        if blocks:
            for name, pat in PATTERNS.items():
                m = pat.search(x)
                if m and name not in blocks[-1]:
                    blocks[-1][name] = int(m.group(1))
    r = [f"== key_run {a.label}  {time.strftime('%Y-%m-%d %H:%M:%S')}  port={a.port}  {a.secs:.0f} s",
         f"   keys: {a.keys}", "== stats blocks (in order)"]
    for b in blocks:
        r.append(f"   t={b['t']:7.2f}  " + "  ".join(f"{k}={b[k]}" for k in PATTERNS if k in b))
    r.append("== key lines")
    r += [f"{t:8.3f} {x}" for t, _, x in rows if re.search(r"\[T6\]|>>> host|queue full|FATAL|ASSERT", x)]
    r.append("== tx results")
    r += [f"{t:8.3f} {x}" for t, _, x in rows if re.search(r"^tx \S+: ", x)]
    r.append("== full log (without [ENV])")
    r += [f"{t:8.3f} {x}" for t, _, x in rows if not x.startswith("[ENV]")]
    path = os.path.join(a.out, f"{a.label}_results.txt")
    with open(path, "w") as f:
        f.write("\n".join(r) + "\n")
    print(f"\nwrote {path}")


if __name__ == "__main__":
    main()
