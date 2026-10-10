#!/usr/bin/env python3
"""
bridge_receiver.py - save the images the bridge node (node 1) streams over USB serial.

The bridge prints normal log lines plus, for every verified transfer:

    @ALFP FILE v1 src=3 sid=00042 size=8000 crc=1a2b3c4d radio=lora frags=34 meta=<hex>
    @D <base64>
    ...
    @ALFP END crc=1a2b3c4d

This script re-checks the CRC-32, then writes
    received/n3_00042.jpg       the image
    received/n3_00042.json      node, radio, capture metadata decoded (see common/capture_meta.h)
Other lines are echoed so you still see the bridge's log.

Install:  pip install pyserial
Run:      python3 tools/bridge_receiver.py --port /dev/ttyACM0      (Windows: --port COM5)
          python3 tools/bridge_receiver.py --file capture.txt        (replay a saved log)
Change the output folder with --out.
"""
import argparse
import base64
import json
import os
import struct
import sys
import zlib

# names used by Maryam's weather_tree.h and Rhaylene's mic.c
WEATHER_NAMES = {0: "Cloudy", 1: "Hot_Dry", 2: "Humid_Rainy"}
SOUND_NAMES = {0: "AMBIENT_NOISE", 1: "LOUD_ANIMAL_CALL", 2: "HIGH_FREQ_RUSTLE"}

MISSING_I16 = -32768
MISSING_U16 = 0xFFFF
UNKNOWN_U8 = 0xFF


def decode_meta(raw: bytes) -> dict:
    """Decode the 27-byte capture record. Missing readings become None, never a fake number."""
    if len(raw) < 27 or raw[0] != 1:
        return {"raw_hex": raw.hex()}
    extra = raw[27:]
    (ver, node, cap_id, uptime, trig, temp, hum, light, wlabel, wconf, trust, sclass, sconf, rms, zcr,
     img) = struct.unpack("<BBIIBhHHBBBBBHHB", raw[:27])

    def u8(v):
        return None if v == UNKNOWN_U8 else v

    def u16(v):
        return None if v == MISSING_U16 else v

    return {
        "version": ver,
        "node_id": node,
        "capture_id": cap_id,
        "uptime_ms": uptime,
        "trigger_pir": bool(trig & 1),
        "trigger_sound": bool(trig & 2),
        "temperature_c": None if temp == MISSING_I16 else temp / 10.0,
        "humidity_pct": None if hum == MISSING_U16 else hum / 10.0,
        "light_raw": u16(light),
        "weather_label": u8(wlabel),
        "weather_name": WEATHER_NAMES.get(wlabel) if wlabel != UNKNOWN_U8 else None,
        "weather_confidence": u8(wconf),
        "trigger_trust": u8(trust),
        "sound_class": u8(sclass),
        "sound_name": SOUND_NAMES.get(sclass) if sclass != UNKNOWN_U8 else None,
        "sound_confidence": u8(sconf),
        "sound_rms": u16(rms),
        "sound_zcr": u16(zcr),
        "image_index": img,
        # optional: bytes after the 27-byte record carry the original file name (photo test)
        "original_name": extra.decode("utf-8", "replace") if extra else None,
    }


def parse_header(line: str) -> dict:
    fields = {}
    for part in line.split()[3:]:
        if "=" in part:
            k, v = part.split("=", 1)
            fields[k] = v
    return fields


class Receiver:
    def __init__(self, out_dir: str, echo=True):
        self.out_dir = out_dir
        self.echo = echo
        self.cur = None
        self.saved = []
        os.makedirs(out_dir, exist_ok=True)

    def feed(self, line: str):
        line = line.rstrip("\r\n")
        if line.startswith("@ALFP FILE"):
            self.cur = {"hdr": parse_header(line), "data": bytearray()}
        elif line.startswith("@D ") and self.cur is not None:
            self.cur["data"] += base64.b64decode(line[3:])
        elif line.startswith("@ALFP END") and self.cur is not None:
            self._finish()
        elif line.startswith("@ALFP ABORT"):
            print("bridge aborted a transfer:", line, file=sys.stderr)
            self.cur = None
        elif self.echo:
            print(line)

    def _finish(self):
        hdr, data = self.cur["hdr"], bytes(self.cur["data"])
        self.cur = None
        want = int(hdr.get("crc", "0"), 16)
        got = zlib.crc32(data) & 0xFFFFFFFF
        size_ok = len(data) == int(hdr.get("size", "-1"))
        if got != want or not size_ok:
            print(f"REJECTED transfer src={hdr.get('src')} sid={hdr.get('sid')}: crc {got:08x} != {want:08x}"
                  f" or size mismatch", file=sys.stderr)
            return
        meta = decode_meta(bytes.fromhex(hdr.get("meta", "")))
        orig = os.path.basename(meta.get("original_name") or "") if isinstance(meta, dict) else ""
        orig = "".join(ch for ch in orig if ch.isalnum() or ch in "._-")
        name = f"n{hdr['src']}_{hdr['sid']}" + (f"_{os.path.splitext(orig)[0]}" if orig else "")
        ext = os.path.splitext(orig)[1] if orig else ".jpg"
        img_path = os.path.join(self.out_dir, name + (ext or ".jpg"))
        with open(img_path, "wb") as f:
            f.write(data)
        info = {
            "src": int(hdr["src"]),
            "session": int(hdr["sid"]),
            "radio": hdr.get("radio"),
            "fragments": int(hdr.get("frags", "0")),
            "size": len(data),
            "crc32": f"{got:08x}",
            "capture": meta,
        }
        with open(os.path.join(self.out_dir, name + ".json"), "w") as f:  # same name, .json
            json.dump(info, f, indent=2)
        self.saved.append(img_path)
        extra = ""
        if isinstance(meta, dict) and "node_id" in meta:
            t = meta["temperature_c"]
            extra = (f" | {meta.get('sound_name') or 'sound ?'}, {meta.get('weather_name') or 'weather ?'}"
                     f", {'%.1f C' % t if t is not None else 'temp ?'}")
        print(f"saved {img_path} ({len(data)} bytes via {info['radio']}, crc ok){extra}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port of the bridge (e.g. /dev/ttyACM0 or COM5)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--file", help="read a saved log instead of a serial port ('-' = stdin)")
    ap.add_argument("--out", default="received", help="folder for images and JSON")
    ap.add_argument("--quiet", action="store_true", help="do not echo other log lines")
    args = ap.parse_args()
    rx = Receiver(args.out, echo=not args.quiet)

    if args.file:
        src = sys.stdin if args.file == "-" else open(args.file, "r", errors="replace")
        for line in src:
            rx.feed(line)
        print(f"{len(rx.saved)} file(s) saved to {args.out}/")
        return 0 if rx.saved else 1

    if not args.port:
        ap.error("give --port or --file")
    try:
        import serial  # pyserial
    except ImportError:
        print("pyserial missing: pip install pyserial", file=sys.stderr)
        return 2
    with serial.Serial(args.port, args.baud, timeout=1) as s:
        print(f"listening on {args.port}, saving to {args.out}/ (Ctrl+C to stop)")
        try:
            while True:
                raw = s.readline()
                if raw:
                    rx.feed(raw.decode("ascii", errors="replace"))
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
