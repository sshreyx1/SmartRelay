#!/usr/bin/env python3
"""soak_pair.py - long two-node hardware run: bridge (node 1) + one sender, both consoles logged.

Opens both serial ports at once, sends 'c' (capture) to the sender every --c-every seconds and
's' (stats) to both every --s-every seconds, and optionally extra keys at fixed times. Every
console line is written host-timestamped to <out>/<label>_n1.log and <label>_n2.log; the bridge
log can be replayed with  tools/bridge_receiver.py --file <label>_n1.log  (the timestamps are
stripped by --raw-n1, which also writes <label>_n1_raw.txt).

At the end it prints and writes <label>_summary.txt: transfers per kind and radio, failures,
fallbacks, and the worst value of each health field the 's' command reports (longest comms
loop, slowest comms step, slowest SD call, stack free, drops).

  pip install pyserial
  python3 tools/soak_pair.py --n1 /dev/cu.usbmodemMTK3PICO1 --n2 /dev/cu.usbmodem3 \\
          --minutes 60 --c-every 120 --s-every 120 --label soak --out docs/verify/evidence
  extra keys: --keys "n2:300:k,n1:310:k"   (node:second:key)
"""
import argparse
import os
import re
import sys
import time

import serial

TX_RE = re.compile(r"^tx (\S+): (\w+) via (\w+), (\d+) frags, (\d+) rounds, (\d+) resent, ([\d.]+) s")
CHOICE_RE = re.compile(r"^tx (\S+) size=(-?\d+) thr=(\d+) radio=(\w+) reason=(\w+)")
RX_RE = re.compile(r"^rx from node (\d+): (\S+), (\d+) bytes via (\w+), (\d+) dup")
LOOP_RE = re.compile(r"longest comms loop (\d+) ms")
STEP_RE = re.compile(r"slowest comms step \(ms\): (.*)")
SD_RE = re.compile(r"slowest SD call \(ms\): (.*) \| worst (\w+) (\d+) ms on (\S*)")
STACK_RE = re.compile(r"task (\w+): core (\d+) prio (\d+) stack free (\d+) of (\d+) B")
RADIO_RE = re.compile(r"^\s*(wifi|lora): tx (\d+) rx (\d+) delivered (\d+) relayed (\d+) dup (\d+) rxerr (\d+)")
DHT_RE = re.compile(r"dht22 good (\d+) fail (\d+)")
DROP_RE = re.compile(r"log dropped (\d+) lines \| usb dropped (\d+) lines")
QFULL_RE = re.compile(r"capture q full (\d+)")
HEAP_RE = re.compile(r"heap used (\d+) B")
UPLINK_RE = re.compile(r"uplink sent (\d+) bad (\d+) waiting (\d+) \(max (\d+)\) inline (\d+)")
SOUND_LOST_RE = re.compile(r"lost=(\d+)")


def parse_keys(spec):
    out = []
    for item in filter(None, (x.strip() for x in (spec or "").split(","))):
        node, sec, key = item.split(":", 2)
        out.append((float(sec), node, key.encode()))
    return sorted(out)


def summarize(lines, label):
    r = []
    n2tx = [TX_RE.search(x) for _, x in lines["n2"]]
    n2tx = [m for m in n2tx if m]
    by = {}
    for m in n2tx:
        kind = "telemetry" if m.group(1).startswith("tel/") else ("payload" if "test_payloads" in m.group(1) else "image")
        key = (kind, m.group(3), m.group(2))
        by.setdefault(key, []).append(float(m.group(7)))
    r.append(f"== {label}: sender transfers (kind, radio, result: count, time s min/mean/max)")
    for (kind, radio, res), ts in sorted(by.items()):
        r.append(f"   {kind:9} {radio:4} {res:8}: {len(ts):3}  {min(ts):.1f} / {sum(ts) / len(ts):.1f} / {max(ts):.1f}")
    reasons = {}
    for _, x in lines["n2"]:
        m = CHOICE_RE.search(x)
        if m:
            reasons[(m.group(4), m.group(5))] = reasons.get((m.group(4), m.group(5)), 0) + 1
    r.append("   radio choices: " + ", ".join(f"{k[0]}/{k[1]} {v}" for k, v in sorted(reasons.items())))
    rx = [RX_RE.search(x) for _, x in lines["n1"]]
    rx = [m for m in rx if m]
    r.append(f"   bridge rx: {len(rx)} files ({sum(1 for m in rx if m.group(4) == 'lora')} lora, "
             f"{sum(1 for m in rx if m.group(4) == 'wifi')} wifi)")
    n1_files = sum(1 for _, x in lines["n1"] if x.startswith("@ALFP FILE"))
    n1_end = sum(1 for _, x in lines["n1"] if x.startswith("@ALFP END"))
    n1_abort = sum(1 for _, x in lines["n1"] if x.startswith("@ALFP ABORT"))
    r.append(f"   bridge uplink: {n1_files} FILE, {n1_end} END, {n1_abort} ABORT")
    lost = [int(m.group(1)) for _, x in lines["n2"] for m in [SOUND_LOST_RE.search(x)] if m]
    if lost:
        r.append(f"   sound captures: {len(lost)}, lost= max {max(lost)}")

    for node in ("n1", "n2"):
        rows = lines[node]
        r.append(f"== {node} health over the run (first -> last 's', worst)")

        def vals(rx_, grp=1):
            return [int(m.group(grp)) for _, x in rows for m in [rx_.search(x)] if m]

        loops = vals(LOOP_RE)
        if loops:
            r.append(f"   longest comms loop ms: first {loops[0]} last {loops[-1]} max {max(loops)}")
        steps = [m.group(1) for _, x in rows for m in [STEP_RE.search(x)] if m]
        if steps:
            r.append(f"   slowest comms step (last): {steps[-1]}")
        sds = [m for _, x in rows for m in [SD_RE.search(x)] if m]
        if sds:
            r.append(f"   slowest SD call (last): {sds[-1].group(1)} | worst {sds[-1].group(2)} "
                     f"{sds[-1].group(3)} ms on {sds[-1].group(4)}")
        stacks = {}
        for _, x in rows:
            m = STACK_RE.search(x)
            if m:
                stacks.setdefault(m.group(1), []).append((int(m.group(4)), int(m.group(5)), m.group(2), m.group(3)))
        for t, v in stacks.items():
            r.append(f"   task {t}: core {v[-1][2]} prio {v[-1][3]}, stack free min {min(a for a, *_ in v)} "
                     f"of {v[0][1]} B (first {v[0][0]}, last {v[-1][0]})")
        for radio in ("wifi", "lora"):
            rr = [m for _, x in rows for m in [RADIO_RE.search(x)] if m and m.group(1) == radio]
            if rr:
                a, b = rr[0], rr[-1]
                r.append(f"   {radio}: rxerr {a.group(7)} -> {b.group(7)}, tx {a.group(2)} -> {b.group(2)}, "
                         f"relayed {a.group(5)} -> {b.group(5)}")
        dht = [m for _, x in rows for m in [DHT_RE.search(x)] if m]
        if dht:
            r.append(f"   dht22: good {dht[-1].group(1)} fail {dht[-1].group(2)}")
        drops = [m for _, x in rows for m in [DROP_RE.search(x)] if m]
        if drops:
            r.append(f"   log dropped {drops[-1].group(1)}, usb dropped {drops[-1].group(2)}")
        q = vals(QFULL_RE)
        if q:
            r.append(f"   capture q full: {q[-1]}")
        h = vals(HEAP_RE)
        if h:
            r.append(f"   heap used: min {min(h)} max {max(h)} B")
        up = [m for _, x in rows for m in [UPLINK_RE.search(x)] if m]
        if up:
            u = up[-1]
            r.append(f"   uplink: sent {u.group(1)} bad {u.group(2)} waiting {u.group(3)} max waiting {u.group(4)} "
                     f"inline {u.group(5)}")
        bad = [x for _, x in rows if re.search(r"FATAL|ASSERT|panic|HardFault|FULL \(", x)]
        r.append(f"   FATAL/ASSERT/outbox-FULL lines: {len(bad)}")
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n1", required=True)
    ap.add_argument("--n2", required=True)
    ap.add_argument("--label", default="soak")
    ap.add_argument("--out", default=".")
    ap.add_argument("--minutes", type=float, default=60)
    ap.add_argument("--c-every", type=float, default=120)
    ap.add_argument("--c-start", type=float, default=10)
    ap.add_argument("--s-every", type=float, default=120)
    ap.add_argument("--keys", default="", help='extra keys "n2:300:k,n1:310:k"')
    ap.add_argument("--no-c", action="store_true", help="do not send captures")
    a = ap.parse_args()

    ports = {}
    for name, dev in (("n1", a.n1), ("n2", a.n2)):
        s = serial.Serial(dev, 115200, timeout=0)
        try:
            s.dtr = True
        except Exception:
            pass
        ports[name] = s

    total = a.minutes * 60.0
    next_c = a.c_start if not a.no_c else float("inf")
    next_s = 5.0
    extra = parse_keys(a.keys)
    lines = {"n1": [], "n2": []}
    bufs = {"n1": b"", "n2": b""}
    os.makedirs(a.out, exist_ok=True)
    logs = {n: open(os.path.join(a.out, f"{a.label}_{n}.log"), "w") for n in ports}
    t0 = time.time()
    print(f"[{a.label}] {a.minutes:.0f} min; c every {a.c_every:.0f} s, s every {a.s_every:.0f} s", flush=True)
    last_print = 0.0
    while True:
        t = time.time() - t0
        if t >= total + 6:
            break
        if t >= next_c and t < total:
            ports["n2"].write(b"c")
            next_c += a.c_every
        if t >= next_s:
            for p in ports.values():
                p.write(b"s")
            next_s = next_s + a.s_every if t < total else float("inf")
            if next_s > total:
                next_s = total  # always one final 's' at the end
        while extra and t >= extra[0][0]:
            _, node, key = extra.pop(0)
            ports[node].write(key)
        for name, s in ports.items():
            chunk = s.read(8192)
            if chunk:
                bufs[name] += chunk
                while b"\n" in bufs[name]:
                    raw, bufs[name] = bufs[name].split(b"\n", 1)
                    text = raw.decode("utf-8", "replace").rstrip("\r")
                    lines[name].append((t, text))
                    logs[name].write(f"{t:9.3f} {text}\n")
        if t - last_print > 60:
            last_print = t
            print(f"  {t / 60:5.1f} min: n1 {len(lines['n1'])} lines, n2 {len(lines['n2'])} lines", flush=True)
            for f in logs.values():
                f.flush()
        time.sleep(0.005)
    for s in ports.values():
        s.close()
    for f in logs.values():
        f.close()
    with open(os.path.join(a.out, f"{a.label}_n1_raw.txt"), "w") as f:
        for _, x in lines["n1"]:
            f.write(x + "\n")
    summary = summarize(lines, a.label)
    with open(os.path.join(a.out, f"{a.label}_summary.txt"), "w") as f:
        f.write("\n".join(summary) + "\n")
    print("\n".join(summary))


if __name__ == "__main__":
    sys.exit(main())
