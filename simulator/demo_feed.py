#!/usr/bin/env python3
"""
demo_feed.py - MANUAL-ONLY demo data for the smartrelay dashboard.

This is for showing the dashboard UI when no hardware is around (e.g.
presenting indoors). It is never started automatically by the backend --
you run it yourself, on purpose, every time:

    python3 simulator/demo_feed.py --once --node 3
    python3 simulator/demo_feed.py --burst 5
    python3 simulator/demo_feed.py --continuous --every 20

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


def build_meta(node_id: int, capture_id: int, trigger_pir: bool, trigger_sound: bool,
               sound_class: int = UNKNOWN_U8, image_name: str = "") -> bytes:
    rnd = random.Random(node_id * 1000 + capture_id)
    temp_x10 = round(rnd.uniform(24.0, 33.0) * 10)   # Singapore-ish trail temperature, 24-33 C
    hum_x10 = round(rnd.uniform(60, 95) * 10)
    light = rnd.randrange(0, 4096)
    weather = rnd.choice(list(WEATHER.keys()))
    sound_rms = rnd.randrange(100, 4000) if trigger_sound else MISSING_U16
    sound_zcr = rnd.randrange(50, 2000) if trigger_sound else MISSING_U16
    trig = (1 if trigger_pir else 0) | (2 if trigger_sound else 0)

    return struct.pack(
        "<BBIIBhHHBBBBBHHB",
        1,                      # version
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
    radio = random.choice(["lora", "wifi"])
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


def send(backend_url: str, lines: list[str]) -> None:
    if requests is None:
        print("pip install requests  (needed to talk to the backend)", file=sys.stderr)
        sys.exit(2)
    r = requests.post(f"{backend_url}/api/demo/ingest", json={"lines": lines}, timeout=5)
    r.raise_for_status()
    print(f"[DEMO] sent 1 simulated capture -> {r.json()}")


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
    args = ap.parse_args()

    print("=" * 70)
    print(" DEMO MODE -- this sends SIMULATED captures, clearly tagged is_demo=1.")
    print(" It is NOT reading your real nodes. Run bridge_receiver.py / the backend")
    print(" with SMARTRELAY_BRIDGE_PORT set for real data.")
    print("=" * 70)

    image_bytes = open(args.image, "rb").read() if args.image else None

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
