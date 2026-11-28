#!/usr/bin/env python3
"""
demo_multihop.py - the multi-hop part of the demo, on one laptop: three processes stand in for three Picos,
each running the REAL third_party/wifi_mesh/wifi_mesh.c + ALFP (host/integration/integ_node.c), with the
ESP-01 replaced by UDP on localhost (host/integration/fake_wst_link.c). The topology filter from
wifi_mesh_config.h is ON, so E (node 5) cannot hear A (node 1, the bridge): the photo must go E -> C -> A.

It builds the node program, sends one label photo (default sd_card/samples/dog_00.jpg) from E, prints every
node's log line by line, then the hop path, the relay counters and a byte-for-byte check. Exit code 0 = PASS.
Same idea as host/integration/run_wifi_integration.sh, which stays as the test-suite version.

Needs a C compiler (cc/clang/gcc) and POSIX sockets: macOS and Linux natively; on Windows run it inside WSL
(the fake radio uses <arpa/inet.h>). Python is stdlib only.

Run:      python tools/demo_multihop.py                 (about 15 s)
          python tools/demo_multihop.py --loss 0.1      (10% of packets lost in the "air": watch the resends)
          python tools/demo_multihop.py --build-only
"""
import argparse
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INTEG = ROOT / "host" / "integration"
WM = ROOT / "third_party" / "wifi_mesh"
BUILD = ROOT / "build" / "demo_multihop"
EXE = BUILD / "integ_node_wifi"
NAMES = {1: "A (node 1, bridge)", 3: "C (node 3, relay)", 5: "E (node 5, field)"}
STATS = re.compile(r"mesh stats: tx (\d+) rx (\d+) delivered (\d+) relayed (\d+) dup (\d+)(?:.*?filtered (\d+))?")


def neighbours() -> dict:
    """Who hears whom, read from wifi_mesh_config.h (the same table the firmware uses)."""
    text = (WM / "wifi_mesh_config.h").read_text(encoding="utf-8")
    body = text[text.index("wifi_mesh_neighbours["):]
    body = body[body.index("{") + 1:body.index("};")]
    rows = [r for r in (ln.split("/*")[0].strip().rstrip(",") for ln in body.splitlines()) if r]
    return {i: sorted(int(n) for n in re.findall(r"NB\((\d)\)", row)) for i, row in enumerate(rows) if i}


def find_cc() -> str:
    for cc in [os.environ.get("CC"), "cc", "clang", "gcc"]:
        if cc and shutil.which(cc):
            return cc
    sys.exit("error: no C compiler found (install Xcode command line tools / build-essential, or set CC)")


def build() -> None:
    BUILD.mkdir(parents=True, exist_ok=True)
    srcs = [INTEG / "integ_node.c", INTEG / "fake_wst_link.c", WM / "wifi_mesh.c", ROOT / "host/sim/storage_posix.c",
            *sorted((ROOT / "alfp" / "src").glob("*.c"))]
    newest = max(p.stat().st_mtime for p in srcs + [WM / "wifi_mesh_config.h"])
    if EXE.exists() and EXE.stat().st_mtime > newest:
        return
    inc = [INTEG, WM, ROOT / "mesh", ROOT / "alfp/include", ROOT / "common", ROOT / "host/sim"]
    cmd = [find_cc(), "-O1", "-g", "-Wall", "-DUSE_WIFI", *[f"-I{p}" for p in inc], *map(str, srcs), "-lm", "-o", str(EXE)]
    print("building the node program (real wifi_mesh.c + ALFP) ...")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("error: build failed\n" + r.stderr)


def pump(proc: subprocess.Popen, tag: str, sink: list) -> None:
    for line in proc.stdout:
        line = line.rstrip()
        sink.append(line)
        print(f"  {tag:<20} | {line}", flush=True)


def stats_of(lines: list) -> dict:
    for ln in reversed(lines):
        m = STATS.search(ln)
        if m:
            k = ["tx", "rx", "delivered", "relayed", "dup", "filtered"]
            return {a: int(b) if b is not None else None for a, b in zip(k, m.groups())}
    return {}


def main() -> int:
    ap = argparse.ArgumentParser(description="Multi-hop demo: E -> C -> A with the real wifi_mesh.c + ALFP "
                                             "as three local processes (topology filter ON).")
    ap.add_argument("--image", type=Path, default=ROOT / "sd_card" / "samples" / "dog_00.jpg",
                    help="file node E sends (default: the dog label photo)")
    ap.add_argument("--loss", type=float, default=0.0, help="fraction of packets lost per receiver, 0-1 (default 0)")
    ap.add_argument("--timeout", type=float, default=90, help="give up after this many seconds (default 90)")
    ap.add_argument("--build-only", action="store_true", help="only build the node program")
    args = ap.parse_args()

    if platform.system() == "Windows":
        sys.exit("error: the fake ESP-01 link uses POSIX sockets; on Windows run this inside WSL "
                 "(wsl, then: python3 tools/demo_multihop.py)")
    build()
    if args.build_only:
        print(f"built {EXE.relative_to(ROOT)}")
        return 0
    if not args.image.is_file():
        sys.exit(f"error: {args.image} not found")

    nb = neighbours()
    letter = " ABCDE"
    print("\ntopology filter ON (third_party/wifi_mesh/wifi_mesh_config.h), running nodes A, C, E only:")
    for n in (1, 3, 5):
        print(f"  {letter[n]} hears {', '.join(letter[x] for x in nb[n])}")
    print("  -> E cannot reach A directly; with B and D off, the only path is E -> C -> A\n")

    work = Path(tempfile.mkdtemp(prefix="demo_multihop_"))
    for n in (1, 3, 5):
        (work / f"node{n}").mkdir()
    shutil.copyfile(args.image, work / "node5" / "photo.jpg")
    env = dict(os.environ, WIFI_LOSS=str(args.loss))

    def start(n: int, *extra: str) -> subprocess.Popen:
        return subprocess.Popen([str(EXE), str(n), str(work / f"node{n}"), *extra], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, env=env)

    logs = {1: [], 3: [], 5: []}
    procs, threads = {}, []
    t0 = time.time()
    try:
        for n, extra in ((1, ()), (3, ("relay",))):
            procs[n] = start(n, *extra)
        time.sleep(0.5)
        procs[5] = start(5, "photo.jpg")
        for n, p in procs.items():
            th = threading.Thread(target=pump, args=(p, NAMES[n], logs[n]), daemon=True)
            th.start()
            threads.append(th)
        procs[1].wait(timeout=args.timeout)
        procs[5].wait(timeout=10)
    except subprocess.TimeoutExpired:
        print(f"\nerror: no verified transfer within {args.timeout:.0f} s")
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        relay = procs.get(3)
        if relay and relay.poll() is None:
            relay.send_signal(signal.SIGINT)  # the relay prints its counters and exits
            try:
                relay.wait(timeout=5)
            except subprocess.TimeoutExpired:
                relay.kill()
        for p in procs.values():
            if p.poll() is None:
                p.kill()
                p.wait()
        for th in threads:
            th.join(timeout=2)
    elapsed = time.time() - t0

    rx_file = next((ln.split("=", 1)[1] for ln in logs[1] if ln.startswith("RX_FILE=")), None)
    same = rx_file is not None and (work / "node1" / rx_file).read_bytes() == args.image.read_bytes()
    s = {n: stats_of(logs[n]) for n in (1, 3, 5)}
    sent = next((ln for ln in logs[5] if ln.startswith("[sender]")), "[sender] no report")

    print("\n" + "=" * 72)
    print(f" HOP PATH   E (node 5)  --wifi-->  C (node 3, relay)  --wifi-->  A (node 1, bridge)")
    print("=" * 72)
    print(f" file       {args.image.name}, {args.image.stat().st_size} bytes, loss {args.loss:.0%}")
    print(f" sender     {sent.removeprefix('[sender] ')}")
    if s[3]:
        print(f" relay C    relayed {s[3]['relayed']} packets (E's data towards A, A's ACKs back to E), "
              f"heard {s[3]['rx']}, duplicates dropped {s[3]['dup']}")
    if s[1]:
        print(f" bridge A   delivered {s[1]['delivered']} packets to ALFP; ignored {s[1]['filtered']} packets heard "
              f"straight from E (not a neighbour)")
    if s[5]:
        print(f" field E    transmitted {s[5]['tx']}; ignored {s[5]['filtered']} packets from A")
    print(f" result     {'PASS' if same else 'FAIL'}: "
          + ("the file at A is byte-for-byte the photo E sent" if same else "no verified, identical file at A")
          + f" ({elapsed:.1f} s)")
    shutil.rmtree(work, ignore_errors=True)
    return 0 if same else 1


if __name__ == "__main__":
    sys.exit(main())
