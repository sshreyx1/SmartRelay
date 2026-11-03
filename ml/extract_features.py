#!/usr/bin/env python3
"""
extract_features.py - run the C feature extractor (build/sound_featx) over every recorded take
and write ml/data/features_v1.csv: the recording metadata + the 25 features + legacy mic.c energy/zcr.

Features always come from the C binary, never a Python re-implementation, so training sees
exactly the numbers the Pico computes. Build it first: cmake -S host -B build && cmake --build build -j
(Windows/MSVC: cmake -S host -B build -DSOUND_ONLY=ON && cmake --build build --config Release)

Run:      python ml/extract_features.py
          python ml/extract_features.py --recordings ml/data/recordings.csv --out ml/data/features_v1.csv
"""
import argparse
import csv
import io
import subprocess
import sys
from pathlib import Path

from hosttools import find_host_tool
from mlcommon import DATA_DIR, read_csv, rel, resolve, write_csv

BATCH = 100  # files per sound_featx call (keeps Windows command lines short)
META_FIELDS = ["file", "clip_id", "source", "label_id", "group_id", "split_key", "volume", "take"]


def featx(paths: list, build_dir: Path = None) -> tuple:
    """Run sound_featx over paths. Returns (feature column names, {str(path): {name: float}})."""
    exe = find_host_tool("sound_featx", build_dir) if build_dir else find_host_tool("sound_featx")
    names, out = None, {}
    for i in range(0, max(len(paths), 1), BATCH):
        batch = [str(p) for p in paths[i:i + BATCH]]
        p = subprocess.run([str(exe), "--header"] + batch, capture_output=True, text=True, encoding="utf-8")
        if p.returncode not in (0, 1):
            raise RuntimeError(f"sound_featx failed: {p.stderr.strip()}")
        if p.stderr.strip():
            print(p.stderr.strip(), file=sys.stderr)
        reader = csv.reader(io.StringIO(p.stdout))
        header = next(reader)
        names = header[1:]
        for row in reader:
            out[row[0]] = {n: float(v) for n, v in zip(names, row[1:])}
    return names, out


def main() -> int:
    ap = argparse.ArgumentParser(description="Extract features from recorded takes with the C extractor.")
    ap.add_argument("--recordings", type=Path, default=DATA_DIR / "recordings.csv")
    ap.add_argument("--out", type=Path, default=DATA_DIR / "features_v1.csv")
    ap.add_argument("--build-dir", type=Path, default=None)
    args = ap.parse_args()

    if not args.recordings.exists():
        print(f"error: {args.recordings} not found (run record_dataset.py first)", file=sys.stderr)
        return 1
    rows = {}
    for r in read_csv(args.recordings):  # last row per file wins (a re-recorded take)
        if r.get("crc_ok") == "1" and resolve(r["file"]).exists():
            rows[r["file"]] = r
    paths = [resolve(f) for f in rows]
    names, feats = featx(paths, args.build_dir)
    out = []
    for f, r in rows.items():
        v = feats.get(str(resolve(f)))
        if v is None:
            continue
        row = {k: r.get(k, "") for k in META_FIELDS}
        row["source"] = r.get("source") or ("ambient" if r["clip_id"].startswith("ambient_") else "esc50_mic")
        row.update(v)
        out.append(row)
    write_csv(args.out, out, META_FIELDS + (names or []))
    print(f"{len(out)} takes -> {rel(args.out)} ({len(rows) - len(out)} failed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
