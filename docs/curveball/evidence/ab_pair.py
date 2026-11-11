"""ab_pair.py - one half of the bridge A/B pair (docs/curveball/STATUS.md, handoff section 5).

Opens node 1 (bridge) and node 2 (sender) at the same time, sends 'c' to node 2 on a fixed
schedule so both halves get identical traffic, sends 's' to both at the end, and writes:

  <out>/ab_<label>_n1_log.txt, _n2_log.txt every console line, host-timestamped
  <out>/ab_<label>_results.txt                 summary + the lines that matter (paste this back)

  python ab_pair.py --n1 /dev/cu.usbmodem101 --n2 /dev/cu.usbmodemMTK3PICO1 --label freertos
  python ab_pair.py --n1 COM5 --n2 COM7 --label smp            (Windows)

Defaults: 150 s, 'c' at 10, 30, 50, 70, 90, 110 s (6 x every 20 s), then 's' and 8 s more.
"""
import argparse
import os
import re
import statistics
import sys
import time

import serial

TX_RE = re.compile(r"tx (\S+): (\w+) via (\w+), (\d+) frags, (\d+) rounds, (\d+) resent, ([\d.]+) s")
RX_RE = re.compile(r"rx from node (\d+): (\S+), (\d+) bytes via (\w+), (\d+) dup")


def summarize_tx(lines):
    groups = {"telemetry": [], "image": []}
    for _, text in lines:
        m = TX_RE.search(text)
        if m:
            kind = "telemetry" if m.group(1).startswith("tel/") else "image"
            groups[kind].append((m.group(2), int(m.group(4)), int(m.group(6)), float(m.group(7))))
    out = []
    for kind, rows in groups.items():
        if not rows:
            out.append(f"{kind:9}: none")
            continue
        times = [r[3] for r in rows]
        done = sum(1 for r in rows if r[0] == "done")
        gave = sum(1 for r in rows if r[0] == "gave_up")
        out.append(
            f"{kind:9}: n={len(rows)} done={done} gave_up={gave} other={len(rows) - done - gave} "
            f"time s min={min(times):.1f} mean={statistics.mean(times):.2f} max={max(times):.1f} "
            f"| frags {min(r[1] for r in rows)}-{max(r[1] for r in rows)} | resent total {sum(r[2] for r in rows)}"
        )
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n1", required=True, help="node 1 (bridge) serial port")
    ap.add_argument("--n2", required=True, help="node 2 (sender) serial port")
    ap.add_argument("--label", required=True, help="e.g. freertos or smp (used in file names)")
    ap.add_argument("--out", default=os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--secs", type=float, default=150)
    ap.add_argument("--c-start", type=float, default=10)
    ap.add_argument("--c-every", type=float, default=20)
    ap.add_argument("--c-count", type=int, default=6)
    ap.add_argument("--stats-wait", type=float, default=8)
    a = ap.parse_args()

    ports = {}
    for name, dev in (("n1", a.n1), ("n2", a.n2)):
        try:
            s = serial.Serial(dev, 115200, timeout=0)
        except Exception as e:
            print(f"cannot open {name} {dev}: {e}")
            sys.exit(1)
        try:
            s.dtr = True  # USB CDC: tells the firmware a host is reading
        except Exception:
            pass
        ports[name] = s

    c_times = [a.c_start + i * a.c_every for i in range(a.c_count)]
    lines = {"n1": [], "n2": []}
    bufs = {"n1": b"", "n2": b""}
    events = []
    stats_sent = False
    t0 = time.time()
    print(f"[{a.label}] recording {a.secs:.0f} s; 'c' to node 2 at {c_times}; do not touch the boards", flush=True)
    while True:
        t = time.time() - t0
        if t >= a.secs + a.stats_wait:
            break
        while c_times and t >= c_times[0]:
            ports["n2"].write(b"c")
            events.append((t, "host -> n2: c"))
            print(f"{t:8.3f} host -> n2: c", flush=True)
            c_times.pop(0)
        if not stats_sent and t >= a.secs:
            for name in ("n1", "n2"):
                ports[name].write(b"s")
            events.append((t, "host -> n1, n2: s"))
            stats_sent = True
        for name, s in ports.items():
            chunk = s.read(4096)
            if chunk:
                bufs[name] += chunk
                while b"\n" in bufs[name]:
                    raw, bufs[name] = bufs[name].split(b"\n", 1)
                    lines[name].append((time.time() - t0, raw.decode("utf-8", "replace").rstrip()))
        time.sleep(0.005)
    for s in ports.values():
        s.close()

    os.makedirs(a.out, exist_ok=True)
    for name in ("n1", "n2"):
        with open(os.path.join(a.out, f"ab_{a.label}_{name}_log.txt"), "w") as f:
            for t, text in lines[name]:
                f.write(f"{t:8.3f} {text}\n")

    n1, n2 = lines["n1"], lines["n2"]
    stats_t = a.secs
    in_window = lambda rows: [(t, x) for t, x in rows if t < stats_t]  # noqa: E731
    rx = [(t, x) for t, x in in_window(n1) if RX_RE.search(x)]
    rx_names = [RX_RE.search(x).group(2) for _, x in rx]
    rx_dup = sum(1 for _, x in rx if int(RX_RE.search(x).group(5)) > 0)
    captures_queued = [x for _, x in in_window(n2) if re.search(r"capture \S+ queued \(samples/", x)]

    def show(rows, pat):
        return [f"{t:8.3f} {x}" for t, x in rows if re.search(pat, x)]

    r = []
    r.append(f"== ab_pair {a.label}  {time.strftime('%Y-%m-%d %H:%M:%S')}  n1={a.n1} n2={a.n2}")
    r.append(f"   window {a.secs:.0f} s, 'c' at {[round(e[0], 1) for e in events if e[1].endswith(': c')]}")
    r.append("== summary (node 2 tx, in window)")
    r += ["   " + x for x in summarize_tx(in_window(n2))]
    r.append(f"   captures queued on node 2: {len(captures_queued)} (want {a.c_count})")
    r.append(f"   node 1 rx: {len(rx)} files, {len(set(rx_names))} distinct names, {rx_dup} with dup>0")
    r.append(f"   node 1 @ALFP lines: {sum(1 for _, x in in_window(n1) if x.startswith('@ALFP'))}")
    r.append(f"   node 2 [ALERT] lines (PIR cover check, should be 0 apart from forced captures): "
             f"{sum(1 for _, x in in_window(n2) if 'Dual trigger' in x)}")
    r.append("== n2 tx")
    r += show(in_window(n2), r"^tx ")
    r.append("== n2 captures")
    r += show(in_window(n2), r"capture |\[ALERT\]|\[SOUND\]|queue full")
    r.append("== n1 rx")
    r += show(in_window(n1), r"rx from|FATAL|ASSERT")
    r.append("== n1 stats")
    r += [f"{t:8.3f} {x}" for t, x in n1 if t >= stats_t and not x.startswith(("@D", "[ENV]"))]
    r.append("== n2 stats")
    r += [f"{t:8.3f} {x}" for t, x in n2 if t >= stats_t and not x.startswith("[ENV]")]
    path = os.path.join(a.out, f"ab_{a.label}_results.txt")
    with open(path, "w") as f:
        f.write("\n".join(r) + "\n")
    print(f"\nwrote {path}")


if __name__ == "__main__":
    main()
