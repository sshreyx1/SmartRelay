#!/usr/bin/env python3
"""
demo_feed.py - MANUAL-ONLY demo data for the smartrelay dashboard.

This is for showing the dashboard UI when no hardware is around (e.g.
presenting indoors). It is never started automatically by the backend --
you run it yourself, on purpose, every time:

    python3 simulator/demo_feed.py --once --node 3
    python3 simulator/demo_feed.py --burst 5
    python3 simulator/demo_feed.py --continuous --every 20
    python3 simulator/demo_feed.py --scenario normal        (also: lora, loss, bridge-down)

--scenario also sends the bridge's live transfer events ("@EVT RXS/RXM/RXF/NACK/RXOK", the lines
firmware/app/main.c prints, docs/PROTOCOLS.md) with real pauses between them, so the
dashboard shows a file arriving fragment by fragment, then the usual @ALFP FILE stream:
    normal       node 5 over WiFi, 40 fragments, ~60% via node 3 and ~40% via node 4, 2 hops, no NACK
    lora         a small file (< 1024 B, so the size rule picks LoRa), relay unknown (v=0)
    loss         ~8% of fragments lost, one NACK, the missing ones resent (r=1), one duplicate
    bridge-down  three captures that waited 60-120 s on node 5's SD card arrive back to back

It builds text in the EXACT real wire format the bridge streams
(common/uplink.h: "@ALFP FILE ... / @D <base64> / @ALFP END"), using the
real capture_meta.h layout (backend/live_bridge.py decodes it with the same
parser as the real bridge, imported straight from tools/bridge_receiver.py --
there is no separate "fake" decode path). It POSTs that text to the
backend's /api/demo/ingest endpoint rather than writing to the database
directly, specifically so it goes through the identical parsing and
validation a real transfer would.

Every row this produces is stored with is_demo=1 and the dashboard always
shows a "SIMULATED" badge on it -- it is never mixed into node status or
counted as a real mesh capture. If you need a sample image to look right on
the dashboard, point --image at a real jpg; otherwise a small placeholder
is generated so the transfer still has a non-trivial size to fragment-count
realistically. With --label-images sd_card/samples it sends the label image a real node would
pick for the simulated sound label (dog_00.jpg for DOG, ...), still tagged SIMULATED.

Sound labels are random ids from common/sound_classes.py: the simulator never classifies, it just
pretends a node reported that label, so the dashboard's label chips and species tally can be shown.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import random
import struct
import sys
import time
import zlib

try:
    import requests
except ImportError:
    requests = None

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common"))
import sound_classes  # noqa: E402

WEATHER = {0: "Cloudy", 1: "Hot_Dry", 2: "Humid_Rainy"}
SOUND_IDS = [c.id for c in sound_classes.CLASSES]
MISSING_I16 = -32768
MISSING_U16 = 0xFFFF
UNKNOWN_U8 = 0xFF

_session_counter = random.randint(1, 500)
# capture ids continue from a time-based start so separate runs do not reuse N2-000001
_capture_counter = {}
_CAPTURE_ID_BASE = int(time.time()) // 60 % 100000


# a real 120x90 grey "SIMULATED" JPEG (375 B), so browsers can draw it
_PLACEHOLDER_JPEG = base64.b64decode(
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDABQODxIPDRQSEBIXFRQYHjIhHhwcHj0sLiQySUBMS0dARkVQWnNiUFVtVkVGZIhlbXd7"
    "gYKBTmCNl4x9lnN+gXz/wAALCABaAHgBAREA/8QAGQABAAMBAQAAAAAAAAAAAAAAAAECBAUD/8QAKxABAAIBAgQEBQUAAAAAAAAA"
    "AAEDAhESBBMiQSExUZEFFDIzoWFxgbHh/9oACAEBAAA/AOIAAAAAAAAAAAAAAAAAAAAAAAAAL0ffr0jGZ3R4ZeU/u3WxlyLcs5si"
    "Zrnps+qOvDv3j/Xvdp8zxNvezGyv2jLX+sfdlsiYrts06MqK8ccu0zGzWPxPsm2nLH4lbZbhljXjZlnrOPhOkzKcbMo4mc67M9tt"
    "OefprMYzEzP8xKts258FTlvvmJrndpEzjPVl5zqtxvM38Xz92zfPL3eu7t+mmv4c8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAf//Z")


def _placeholder_jpeg(seed: int) -> bytes:
    """A small, clearly-synthetic JPEG -- enough bytes to fragment
    and show a size on the dashboard, not a real photo. If you have an
    actual sample image, pass --image instead. The padding goes after the
    JPEG's end marker, which image decoders ignore, so it still renders."""
    rnd = random.Random(seed)
    return _PLACEHOLDER_JPEG + bytes(rnd.randrange(256) for _ in range(4000 + rnd.randrange(4000)))


MISSING_U32 = 0xFFFFFFFF
WIFI_FRAG = 245 - 9  # WiFi mesh payload - ALFP DATA overhead (alfp_wire.h), as the real nodes use
LORA_FRAG = 250 - 9
SIZE_THRESHOLD = 1024  # APP_RADIO_SIZE_THRESHOLD: smaller files go over LoRa


def build_meta(node_id: int, capture_id: int, trigger_pir: bool, trigger_sound: bool,
               sound_class: int = UNKNOWN_U8, image_name: str = "", queued_ms: int | None = None) -> bytes:
    """A version-2 capture record (common/capture_meta.h): 31 bytes + the optional name tail."""
    rnd = random.Random(node_id * 1000 + capture_id)
    temp_x10 = round(rnd.uniform(24.0, 33.0) * 10)   # Singapore-ish trail temperature, 24-33 C
    hum_x10 = round(rnd.uniform(60, 95) * 10)
    light = rnd.randrange(0, 4096)
    weather = rnd.choice(list(WEATHER.keys()))
    sound_rms = rnd.randrange(100, 4000) if trigger_sound else MISSING_U16
    sound_zcr = rnd.randrange(50, 2000) if trigger_sound else MISSING_U16
    trig = (1 if trigger_pir else 0) | (2 if trigger_sound else 0)

    return struct.pack(
        "<BBIIBhHHBBBBBHHBI",
        2,                      # version
        node_id,
        capture_id,
        int(time.time() * 1000) % (2**32),  # uptime_ms stand-in
        trig,
        temp_x10,
        hum_x10,
        light,
        weather,
        rnd.randrange(50, 95),   # weather_conf
        UNKNOWN_U8,               # trigger_trust -- real firmware never fills this either (see sensing.c)
        sound_class,
        rnd.randrange(50, 95) if trigger_sound else UNKNOWN_U8,
        sound_rms,
        sound_zcr,
        0,                        # image_index
        MISSING_U32 if queued_ms is None else queued_ms,  # queued_ms (v2): trigger -> start of sending
    ) + image_name.encode("utf-8")  # optional tail: the image the node picked for its label


def pick_label_image(label_dir: str | None, label_id: int) -> tuple[bytes | None, str]:
    """The image a real node would pick (label -> group -> img_NN), from a local samples folder."""
    if not label_dir or not os.path.isdir(label_dir):
        return None, ""
    files = sorted(os.listdir(label_dir))
    c = sound_classes.get(label_id)
    for prefix in ([c.prefix, sound_classes.get(c.group).prefix] if c else []) + ["img"]:
        hits = [f for f in files if f.startswith(prefix + "_") and f.lower().endswith(".jpg")]
        if hits:
            name = random.choice(hits)
            with open(os.path.join(label_dir, name), "rb") as f:
                return f.read(), name
    return None, ""


def build_transfer_lines(node_id: int, image_bytes: bytes | None = None, label_dir: str | None = None) -> list[str]:
    global _session_counter
    _session_counter += 1
    sid = _session_counter
    cap_id = _capture_counter.get(node_id, _CAPTURE_ID_BASE) + 1
    _capture_counter[node_id] = cap_id

    trigger_pir = True
    trigger_sound = random.random() < 0.7
    sound_class = random.choice(SOUND_IDS) if trigger_sound else UNKNOWN_U8
    label_bytes, image_name = pick_label_image(label_dir, sound_class)
    data = image_bytes if image_bytes is not None else label_bytes or _placeholder_jpeg(sid)
    meta = build_meta(node_id, cap_id, trigger_pir, trigger_sound, sound_class, image_name)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    radio = "lora" if len(data) < 1024 else "wifi"  # the firmware's size rule (docs/PROTOCOLS.md)
    frags = max(1, (len(data) + 191) // 192)  # ALFP's real MAX_PAYLOAD-ish chunking, for a realistic count

    lines = [
        f"@ALFP FILE v1 src={node_id} sid={sid:05d} size={len(data)} crc={crc:08x} "
        f"radio={radio} frags={frags} meta={meta.hex()}"
    ]
    for i in range(0, len(data), 48):
        chunk = data[i:i + 48]
        lines.append("@D " + base64.b64encode(chunk).decode())
    lines.append(f"@ALFP END crc={crc:08x}")
    return lines


def send(backend_url: str, lines: list[str], quiet: bool = False) -> None:
    if requests is None:
        print("pip install requests  (needed to talk to the backend)", file=sys.stderr)
        sys.exit(2)
    r = requests.post(f"{backend_url}/api/demo/ingest", json={"lines": lines}, timeout=5)
    r.raise_for_status()
    if not quiet:
        print(f"[DEMO] sent 1 simulated capture -> {r.json()}")


# ---- live transfer scenarios (docs/PROTOCOLS.md) ------------------------------------------

def bridge_ms() -> int:
    """Stand-in for the bridge's ms-since-boot: wall time, so it keeps rising across runs."""
    return int(time.time() * 1000) & 0xFFFFFFFF


def file_stream(node_id: int, sid: int, data: bytes, meta: bytes, radio: str, frag_size: int,
                first: int, done: int, nacks: int, dups: int) -> list[str]:
    crc = zlib.crc32(data) & 0xFFFFFFFF
    frags = (len(data) + frag_size - 1) // frag_size
    lines = [f"@ALFP FILE v1 src={node_id} sid={sid:05d} size={len(data)} crc={crc:08x} radio={radio} "
             f"frags={frags} first={first} done={done} nacks={nacks} dups={dups} meta={meta.hex()}"]
    lines += ["@D " + base64.b64encode(data[i:i + 48]).decode() for i in range(0, len(data), 48)]
    lines.append(f"@ALFP END crc={crc:08x}")
    return lines


def play_transfer(backend: str, node_id: int, data: bytes, meta: bytes, radio: str, *, loss: float = 0.0,
                  relays=((3, 0.6), (4, 0.4)), hops: int = 2, frag_gap_s: float = 0.04) -> None:
    """Sends one transfer the way the bridge reports it: events paced in real time, then the upload.
    relays = ((node, share), ...); an empty tuple means the relay is unknown (LoRa: v=0)."""
    global _session_counter
    _session_counter += 1
    sid = _session_counter
    frag_size = WIFI_FRAG if radio == "wifi" else LORA_FRAG
    n = (len(data) + frag_size - 1) // frag_size
    crc = zlib.crc32(data) & 0xFFFFFFFF

    def via() -> int:
        if not relays:
            return 0
        x, acc = random.random(), 0.0
        for node, share in relays:
            acc += share
            if x < acc:
                return node
        return relays[-1][0]

    def ev(line: str) -> None:
        send(backend, [line], quiet=True)

    first = bridge_ms()
    ev(f"@EVT RXS src={node_id} sid={sid} radio={radio} t={first}")
    time.sleep(frag_gap_s)
    ev(f"@EVT RXM src={node_id} sid={sid} frags={n} fsz={frag_size} size={len(data)} crc={crc:08x} "
       f"meta={meta.hex()} t={bridge_ms()}")
    lost = [seq for seq in range(n) if random.random() < loss]
    if loss > 0 and not lost and n > 2:
        lost = [random.randrange(n)]  # the loss scenario always loses at least one
    for seq in range(n):
        time.sleep(frag_gap_s)
        if seq not in lost:
            ev(f"@EVT RXF src={node_id} sid={sid} seq={seq} r=0 h={hops} v={via()} d=0 t={bridge_ms()}")
    nacks = dups = 0
    if lost:
        time.sleep(0.2 if radio == "wifi" else 1.5)  # the sender's POLL timeout, then the bridge's NACK
        nacks = 1
        ev(f"@EVT NACK src={node_id} sid={sid} r=1 miss={len(lost)} t={bridge_ms()}")
        copies = 2 if len(lost) <= 4 else 1  # tail_dup: few missing -> each sent twice
        for seq in lost:
            for c in range(copies):
                time.sleep(frag_gap_s)
                ev(f"@EVT RXF src={node_id} sid={sid} seq={seq} r=1 h={hops} v={via()} d={1 if c else 0} "
                   f"t={bridge_ms()}")
                dups += 1 if c else 0
    time.sleep(frag_gap_s)
    done = bridge_ms()
    ev(f"@EVT RXOK src={node_id} sid={sid} first={first} done={done} nacks={nacks} dups={dups} frags={n} "
       f"t={done}")
    send(backend, file_stream(node_id, sid, data, meta, radio, frag_size, first, done, nacks, dups), quiet=True)
    print(f"[DEMO] node {node_id} session {sid}: {len(data)} B via {radio}, {n} fragments, "
          f"{len(lost)} lost, {nacks} NACK, {dups} dup, {(done - first) / 1000:.1f} s")


def sized_jpeg(seed: int, size: int) -> bytes:
    """The SIMULATED placeholder JPEG padded to exactly size bytes (padding after its end marker)."""
    rnd = random.Random(seed)
    pad = max(0, size - len(_PLACEHOLDER_JPEG))
    return _PLACEHOLDER_JPEG + bytes(rnd.randrange(256) for _ in range(pad))


def next_capture_id(node_id: int) -> int:
    cap_id = _capture_counter.get(node_id, _CAPTURE_ID_BASE) + 1
    _capture_counter[node_id] = cap_id
    return cap_id


def run_scenario(name: str, backend: str, node_id: int) -> None:
    if name == "normal":
        cap = next_capture_id(node_id)
        meta = build_meta(node_id, cap, True, True, random.choice(SOUND_IDS), "dog_00.jpg",
                          queued_ms=random.randint(5200, 6500))  # ~5 s of audio + a little queueing
        play_transfer(backend, node_id, sized_jpeg(cap, 40 * WIFI_FRAG - 100), meta, "wifi")
    elif name == "lora":
        cap = next_capture_id(node_id)
        meta = build_meta(node_id, cap, True, False, UNKNOWN_U8, "img_00.jpg", queued_ms=random.randint(5200, 6500))
        play_transfer(backend, node_id, sized_jpeg(cap, 900), meta, "lora", relays=(), hops=2, frag_gap_s=0.35)
    elif name == "loss":
        cap = next_capture_id(node_id)
        meta = build_meta(node_id, cap, True, True, random.choice(SOUND_IDS), "bird_00.jpg",
                          queued_ms=random.randint(5200, 6500))
        play_transfer(backend, node_id, sized_jpeg(cap, 40 * WIFI_FRAG - 100), meta, "wifi", loss=0.08)
    elif name == "bridge-down":
        # store-and-forward: the bridge was unreachable, captures waited on the SD card, then drain
        for waited in sorted((random.randint(60_000, 120_000) for _ in range(3)), reverse=True):
            cap = next_capture_id(node_id)
            meta = build_meta(node_id, cap, True, True, random.choice(SOUND_IDS), "img_00.jpg", queued_ms=waited)
            play_transfer(backend, node_id, sized_jpeg(cap, 25 * WIFI_FRAG), meta, "wifi", frag_gap_s=0.02)
    else:
        raise SystemExit(f"unknown scenario {name}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--backend", default="http://localhost:8000", help="backend base URL")
    ap.add_argument("--node", type=int, default=None, help="node id 2-5 (random field node if omitted)")
    ap.add_argument("--once", action="store_true", help="send a single simulated capture and exit")
    ap.add_argument("--burst", type=int, default=0, help="send N simulated captures back to back")
    ap.add_argument("--continuous", action="store_true", help="keep sending until Ctrl+C")
    ap.add_argument("--every", type=float, default=20.0, help="seconds between captures in --continuous")
    ap.add_argument("--image", help="path to a real JPEG to use instead of the synthetic placeholder")
    ap.add_argument("--label-images", help="folder of <prefix>_NN.jpg label images (e.g. sd_card/samples): "
                                           "send the one a node would pick for the simulated label")
    ap.add_argument("--scenario", choices=["normal", "lora", "loss", "bridge-down"],
                    help="play one live transfer scenario with paced @EVT lines (node 5 unless --node)")
    args = ap.parse_args()

    print("=" * 70)
    print(" DEMO MODE -- this sends SIMULATED captures, clearly tagged is_demo=1.")
    print(" It is NOT reading your real nodes. Run bridge_receiver.py / the backend")
    print(" with SMARTRELAY_BRIDGE_PORT set for real data.")
    print("=" * 70)

    image_bytes = open(args.image, "rb").read() if args.image else None

    if args.scenario:
        run_scenario(args.scenario, args.backend, args.node or 5)
        return 0

    def one():
        node_id = args.node or random.choice([2, 3, 4, 5])
        send(args.backend, build_transfer_lines(node_id, image_bytes, args.label_images))

    if args.continuous:
        try:
            while True:
                one()
                time.sleep(args.every)
        except KeyboardInterrupt:
            print("\nstopped")
        return 0

    n = args.burst if args.burst else 1
    for _ in range(n):
        one()
    return 0


if __name__ == "__main__":
    sys.exit(main())
