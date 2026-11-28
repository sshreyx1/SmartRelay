#!/usr/bin/env python3
"""
verify_received.py - CURVEBALL round 3, Task E: did the backend receive what a two-node hardware
test actually sent, correctly?

Two independent checks:

1. **Manifest check** (`--manifest sd_card/test_payloads/MANIFEST.csv`, or any CSV with
   `filename,size_bytes,sha256` columns): for each expected file, looks for a received file with
   the SAME SHA-256 anywhere under `--received backend/received` (file names the backend writes
   are its own, `n<node>_<session><ext>`, not the original test-payload name, so matching by
   content is the only reliable way). Reports present/missing, size match, hash match, and -- if
   a `--db` is given -- which radio actually carried it (the `captures` table's `radio` column).

2. **Capture linkage check** (`--db backend/smartrelay.db`): every real (non-demo) row in
   `captures` should eventually have both halves of Task A.2's telemetry/image split -- an
   `image_path` and the telemetry fields (`temperature_c`, `sound_class`, etc., whichever of them
   the sensors actually reported). Flags rows missing one half, which is only ever expected
   transiently (image still in flight) or permanently if a radio truly never recovered.

Exits non-zero on any manifest mismatch. The capture-linkage check is informational (a capture
missing its image is sometimes the correct, honest state -- e.g. docs/TESTING.md
3/7 -- so it is reported, not treated as failure, unless --strict-linkage is given.

**What this script's own logic has actually been exercised against**: a synthetic fixture built
in this session (see the bottom of this file's __main__ guard's sibling test invocation, or
backend/tests/test_verify_received.py) -- present/missing/size-mismatch/hash-mismatch files, and
a capture row with only telemetry vs. one with both halves. It has NOT been run against real
two-node hardware data: there is no board or radio hardware attached to the machine that wrote it.
"""
import argparse
import csv
import hashlib
import os
import sqlite3
import sys


def sha256_of(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def load_manifest(path: str):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def index_received_by_hash(received_dir: str):
    """sha256 -> (path, size). Hashing every file once, even a few hundred, is cheap enough not
    to need anything fancier for a test fixture this size."""
    index = {}
    if not os.path.isdir(received_dir):
        return index
    for name in os.listdir(received_dir):
        path = os.path.join(received_dir, name)
        if not os.path.isfile(path):
            continue
        index[sha256_of(path)] = (path, os.path.getsize(path))
    return index


def radio_for_image_path(conn: sqlite3.Connection, image_path: str):
    row = conn.execute("SELECT radio FROM captures WHERE image_path = ? ORDER BY received_at DESC LIMIT 1",
                       (image_path,)).fetchone()
    return row[0] if row else None


def check_manifest(manifest_rows, received_dir: str, conn):
    by_hash = index_received_by_hash(received_dir)
    rows = []
    any_fail = False
    for m in manifest_rows:
        want_size = int(m["size_bytes"])
        want_hash = m["sha256"]
        hit = by_hash.get(want_hash)
        present = hit is not None
        size_ok = present and hit[1] == want_size
        radio = radio_for_image_path(conn, hit[0]) if (present and conn) else None
        rows.append({
            "filename": m["filename"], "expected_size": want_size, "present": present,
            "size_match": size_ok, "hash_match": present, "radio": radio,
        })
        if not present or not size_ok:
            any_fail = True
    return rows, any_fail


def print_manifest_table(rows):
    print(f"{'filename':<20} {'present':<8} {'size_ok':<8} {'hash_ok':<8} {'radio':<8}")
    print("-" * 60)
    for r in rows:
        print(f"{r['filename']:<20} {str(r['present']):<8} {str(r['size_match']):<8} "
              f"{str(r['hash_match']):<8} {str(r['radio'] or '-'):<8}")


def check_capture_linkage(conn: sqlite3.Connection):
    rows = conn.execute(
        "SELECT id, node_id, capture_id, image_path, temperature_c, sound_class, received_at "
        "FROM captures WHERE is_demo = 0 ORDER BY capture_id"
    ).fetchall()
    out = []
    for r in rows:
        has_image = r["image_path"] is not None
        has_telemetry = r["temperature_c"] is not None or r["sound_class"] is not None
        out.append({
            "node_id": r["node_id"], "capture_id": r["capture_id"], "has_image": has_image,
            "has_telemetry": has_telemetry, "linked": has_image and has_telemetry,
        })
    return out


def print_linkage_table(rows):
    print(f"{'node':<6} {'capture_id':<12} {'has_image':<10} {'has_telemetry':<14} {'linked':<8}")
    print("-" * 56)
    for r in rows:
        print(f"{r['node_id']:<6} {r['capture_id']:<12} {str(r['has_image']):<10} "
              f"{str(r['has_telemetry']):<14} {str(r['linked']):<8}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", help="CSV with filename,size_bytes,sha256 (e.g. sd_card/test_payloads/MANIFEST.csv)")
    ap.add_argument("--received", default="backend/received", help="directory the backend saved images into")
    ap.add_argument("--db", default="backend/smartrelay.db", help="backend's SQLite database")
    ap.add_argument("--strict-linkage", action="store_true",
                    help="exit non-zero if any real capture is missing its image or its telemetry")
    a = ap.parse_args()

    conn = None
    if a.db and os.path.exists(a.db):
        conn = sqlite3.connect(a.db)
        conn.row_factory = sqlite3.Row

    exit_code = 0

    if a.manifest:
        if not os.path.exists(a.manifest):
            print(f"manifest not found: {a.manifest}", file=sys.stderr)
            return 2
        manifest_rows = load_manifest(a.manifest)
        rows, any_fail = check_manifest(manifest_rows, a.received, conn)
        print(f"=== manifest check: {a.manifest} vs {a.received} ===")
        print_manifest_table(rows)
        if any_fail:
            exit_code = 1
        print()

    if conn:
        linkage = check_capture_linkage(conn)
        print(f"=== capture telemetry/image linkage: {a.db} ===")
        if linkage:
            print_linkage_table(linkage)
        else:
            print("(no real, non-demo captures in the database)")
        unlinked = [r for r in linkage if not r["linked"]]
        if unlinked:
            print(f"\n{len(unlinked)} of {len(linkage)} real captures missing a half "
                 f"(image or telemetry) -- expected transiently, see docs/TESTING.md")
            if a.strict_linkage:
                exit_code = 1
    elif a.db:
        print(f"(no database at {a.db}, skipping linkage check)")

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
