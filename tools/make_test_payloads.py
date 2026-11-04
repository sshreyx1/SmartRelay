#!/usr/bin/env python3
"""
make_test_payloads.py - a ladder of valid JPEGs of known, exact sizes, for testing the size-based
radio choice (Task A, docs/DESIGN.md "Radio choice by size"): small files must go over LoRa, large
ones over WiFi, with the boundary right at APP_RADIO_SIZE_THRESHOLD.

    python3 tools/make_test_payloads.py
    -> sd_card/test_payloads/p_001024.jpg, p_004096.jpg, ... (default ladder)
    -> sd_card/test_payloads/MANIFEST.csv (size, sha256, filename)

Every file decodes as a real JPEG (Pillow writes the real image; this script then pads it to the
exact target size with one or more COM (0xFFFE) comment marker segments right after the header --
legal anywhere in a JPEG stream and ignored by every decoder, so the padding is part of the valid
file, not trailing garbage). sd_card/samples/ (the camera stand-ins) is never touched.

Needs: pip install pillow
"""
import argparse
import csv
import hashlib
import io
import os
import sys

# Largest payload either radio profile can carry in one ALFP transfer: min(WiFi, LoRa) fragment
# capacity at ALFP_MAX_FRAGS(512), alfp/include/alfp_config.h + alfp_wire.h ALFP_DATA_OVERHEAD(9).
# WiFi: (245 - 9) * 512 = 120832   LoRa: (250 - 9) * 512 = 123392  -> 120832 is safe for both.
ALFP_MAX_PAYLOAD_BYTES = 120832

DEFAULT_LADDER = [1024, 4096, 16384, 32768, 65536, ALFP_MAX_PAYLOAD_BYTES]

MIN_COM_SEGMENT = 4  # FF FE + 2-byte length (minimum payload 0): smallest possible pad granularity
MAX_COM_SEGMENT = 65537  # FF FE + length field maxed at 0xFFFF (65535), including itself


def _base_jpeg(width: int, height: int, quality: int) -> bytes:
    from PIL import Image

    im = Image.new("RGB", (width, height))
    for y in range(height):
        for x in range(width):
            im.putpixel((x, y), ((x * 3) % 256, (y * 5) % 256, ((x + y) * 7) % 256))
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=quality, optimize=True)
    return buf.getvalue()


def _com_segment(payload_len: int) -> bytes:
    """One COM marker segment totalling payload_len + 4 bytes."""
    length_field = payload_len + 2  # JPEG marker length includes the 2 length bytes themselves
    return b"\xff\xfe" + length_field.to_bytes(2, "big") + bytes(payload_len)


def _pad_segments(pad_needed: int) -> bytes:
    if pad_needed == 0:
        return b""
    if pad_needed < MIN_COM_SEGMENT:
        raise ValueError(f"cannot pad by {pad_needed} bytes (minimum non-zero pad is {MIN_COM_SEGMENT})")
    out = bytearray()
    remaining = pad_needed
    while remaining > MAX_COM_SEGMENT:
        out += _com_segment(MAX_COM_SEGMENT - 4)
        remaining -= MAX_COM_SEGMENT
    out += _com_segment(remaining - 4)
    return bytes(out)


def make_jpeg_of_size(target_size: int, base_width: int = 48, base_height: int = 36,
                      base_quality: int = 20) -> bytes:
    """A real, decodable JPEG exactly target_size bytes long."""
    base = _base_jpeg(base_width, base_height, base_quality)
    pad_needed = target_size - len(base)
    if pad_needed < 0:
        raise ValueError(f"base JPEG is already {len(base)} bytes, larger than target {target_size}; "
                         f"lower --base-quality or --base-width/--base-height")
    if 0 < pad_needed < MIN_COM_SEGMENT:
        # Nudge up to a size padding can hit exactly, by shrinking the base image's quality is not
        # precise enough; instead grow the target by reusing the next achievable size (quality is
        # not bisectable to the byte). Simplest exact fix: ask Pillow for one quality step down,
        # which is always at least a few bytes smaller, then pad from there.
        base = _base_jpeg(base_width, base_height, max(base_quality - 1, 1))
        pad_needed = target_size - len(base)
        if pad_needed < 0 or (0 < pad_needed < MIN_COM_SEGMENT):
            raise ValueError(f"cannot hit exactly {target_size} bytes from this base image; "
                             f"try different --base-quality")
    # SOI (2 bytes) then every marker Pillow wrote (APP0/DQT/...) up to SOS; inserting right after
    # SOI is always legal, before any other marker.
    assert base[:2] == b"\xff\xd8", "not a JPEG (missing SOI)"
    return base[:2] + _pad_segments(pad_needed) + base[2:]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="sd_card/test_payloads")
    ap.add_argument("--sizes", type=int, nargs="+", default=DEFAULT_LADDER,
                    help=f"target sizes in bytes (default: {DEFAULT_LADDER})")
    ap.add_argument("--base-width", type=int, default=48)
    ap.add_argument("--base-height", type=int, default=36)
    ap.add_argument("--base-quality", type=int, default=20)
    a = ap.parse_args()
    try:
        import PIL  # noqa: F401
    except ImportError:
        print("Pillow missing: pip install pillow", file=sys.stderr)
        return 2

    os.makedirs(a.out, exist_ok=True)
    rows = []
    for size in a.sizes:
        data = make_jpeg_of_size(size, a.base_width, a.base_height, a.base_quality)
        assert len(data) == size, f"padding bug: wanted {size}, got {len(data)}"
        name = f"p_{size:07d}.jpg"
        path = os.path.join(a.out, name)
        with open(path, "wb") as f:
            f.write(data)
        digest = hashlib.sha256(data).hexdigest()
        rows.append({"filename": name, "size_bytes": size, "sha256": digest})
        print(f"{path}: {size} bytes, sha256 {digest[:12]}...")

    manifest = os.path.join(a.out, "MANIFEST.csv")
    with open(manifest, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["filename", "size_bytes", "sha256"])
        w.writeheader()
        w.writerows(rows)
    print(f"manifest: {manifest} ({len(rows)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
