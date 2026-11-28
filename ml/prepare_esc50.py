#!/usr/bin/env python3
"""
prepare_esc50.py - pick the ESC-50 clips to play at the Pico mic and write ml/data/manifest.csv.

Reads ESC-50's meta/esc50.csv and ml/label_map.yaml, takes --per-category clips from each mapped
category spread evenly over the 5 ESC folds (deterministic), and writes one manifest row per clip:
clip_id, source, category, label_id, group_id, split_key (= ESC fold), src_path.
Split rule (docs/SENSORS.md): fold 5 = test, fold 4 = val, folds 1-3 = train.

Get ESC-50 first (about 650 MB) into ml/data/esc50/:
    macOS:    curl -L -o ml/data/esc50.zip https://github.com/karolpiczak/ESC-50/archive/refs/heads/master.zip
              then unzip it and rename ESC-50-master to ml/data/esc50
    Windows:  download the same URL in a browser, extract, rename ESC-50-master to ml\\data\\esc50

Run:      python ml/prepare_esc50.py --per-category 20
"""
import argparse
import random
import sys
from pathlib import Path

from mlcommon import DATA_DIR, load_label_map, read_csv, rel, write_csv

FIELDS = ["clip_id", "source", "category", "label_id", "group_id", "split_key", "src_path"]
DOWNLOAD_HELP = """Get ESC-50 (about 650 MB) into ml/data/esc50/:
  https://github.com/karolpiczak/ESC-50/archive/refs/heads/master.zip
  extract it and rename the folder ESC-50-master to ml/data/esc50 (same on macOS and Windows)"""


def select(meta_rows: list, label_map: dict, per_category: int, seed: int = 0) -> list:
    """Per mapped category, per_category clips spread over folds 1-5 (extra clips go to the lower folds)."""
    rng = random.Random(seed)
    out = []
    for cat in label_map:
        by_fold = {f: sorted(r["filename"] for r in meta_rows if r["category"] == cat and int(r["fold"]) == f)
                   for f in range(1, 6)}
        for f in by_fold.values():
            rng.shuffle(f)
        want = {f: per_category // 5 + (1 if f <= per_category % 5 else 0) for f in range(1, 6)}
        for fold in range(1, 6):
            for name in by_fold[fold][:want[fold]]:
                out.append((cat, fold, name))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="Select ESC-50 clips for recording and write the manifest.")
    ap.add_argument("--esc50-dir", type=Path, default=DATA_DIR / "esc50")
    ap.add_argument("--per-category", type=int, default=20, help="clips per category (ESC-50 has 40)")
    ap.add_argument("--out", type=Path, default=DATA_DIR / "manifest.csv")
    args = ap.parse_args()

    meta = args.esc50_dir / "meta" / "esc50.csv"
    if not meta.exists():
        print(f"error: {meta} not found.\n{DOWNLOAD_HELP}", file=sys.stderr)
        return 1
    label_map = load_label_map()
    rows = []
    for cat, fold, name in select(read_csv(meta), label_map, args.per_category):
        c = label_map[cat]
        rows.append({"clip_id": Path(name).stem, "source": "esc50", "category": cat, "label_id": c.id,
                     "group_id": c.group, "split_key": fold, "src_path": rel(args.esc50_dir / "audio" / name)})
    write_csv(args.out, rows, FIELDS)
    print(f"{len(rows)} clips from {len({r['category'] for r in rows})} categories -> {rel(args.out)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
