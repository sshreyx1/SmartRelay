#!/usr/bin/env python3
"""
esc50_direct.py - BACKUP dataset: ESC-50 clips converted straight to Pico-ADC-like samples, without
playing them through the real mic. Lets the whole pipeline (features -> train -> export -> node) run
before any mic recording exists. Tonight's mic recordings replace it (record_dataset.py), same pipeline.

Per clip and per variant (volume 1.0 / 0.5 stand-ins), deterministic (seeded by the clip name):
  1. resample 44.1 kHz -> 16 kHz (scipy.signal.resample_poly 160/441)
  2. MAX9814-AGC stand-in: scale so the loudest 50 ms RMS hits a random target (250-700 counts at
     v1.0, 0.6x that at v0.5); the AGC makes absolute level nearly meaningless on the real node
  3. mic noise floor: white noise, sigma 2-6 counts; DC bias 2083-2117 counts (measured range)
  4. round and clip to the 12-bit ADC range 0-4095
AMBIENT has no ESC-50 category, so --ambient N synthetic quiet takes are made (white noise, filtered
"fan" noise, 50 Hz hum + harmonics), split_key 1-5 round-robin.
What this cannot model: the room, the speaker, the RC filter, the real AGC time constants. Hence BACKUP.

Writes ml/data/direct/<LABEL>/<clip_id>_v<vol>.npy (gitignored) and ml/data/features_esc50_direct.csv
(via the C extractor sound_featx).

Run:      python ml/esc50_direct.py
          python ml/esc50_direct.py --per-category 40 --ambient 40
"""
import argparse
import sys
import zlib
from pathlib import Path

import numpy as np

from extract_features import META_FIELDS, featx
from mlcommon import DATA_DIR, load_label_map, read_csv, rel, sc, write_csv

N = 80000
RATE = 16000


def rng_for(name: str) -> np.random.Generator:
    return np.random.default_rng(zlib.crc32(name.encode()))


def to_adc(x: np.ndarray, rng: np.random.Generator, level: float) -> np.ndarray:
    """float audio at 16 kHz -> uint16 ADC-like samples (see module docstring)."""
    x = x[:N] if len(x) >= N else np.pad(x, (0, N - len(x)))
    win = 800  # 50 ms
    env = np.sqrt(np.convolve(x * x, np.ones(win) / win, mode="same"))
    peak = env.max()
    target = rng.uniform(250, 700) * level
    y = x * (target / peak) if peak > 1e-9 else x
    y = y + rng.normal(0, rng.uniform(2, 6), N) + rng.uniform(2083, 2117)
    return np.clip(np.round(y), 0, 4095).astype(np.uint16)


def ambient_take(k: int) -> np.ndarray:
    from scipy.signal import lfilter
    rng = np.random.default_rng(10_000 + k)
    kind = k % 4
    t = np.arange(N) / RATE
    if kind == 0:  # quiet room: white noise only
        y = rng.normal(0, rng.uniform(2, 8), N)
    elif kind == 1:  # fan / air conditioner: low-passed noise
        a = rng.uniform(0.90, 0.99)
        y = lfilter([1 - a], [1, -a], rng.normal(0, 1, N))
        y = y / y.std() * rng.uniform(5, 40) + rng.normal(0, 3, N)
    elif kind == 2:  # mains hum + harmonics
        f0 = 50.0
        y = sum(rng.uniform(1, 15) / h * np.sin(2 * np.pi * f0 * h * t + rng.uniform(0, 6.3)) for h in (1, 2, 3, 5))
        y = y + rng.normal(0, rng.uniform(2, 6), N)
    else:  # outdoors-ish: pink-ish noise, slow level drift
        y = np.cumsum(rng.normal(0, 1, N))
        y = y - np.convolve(y, np.ones(400) / 400, mode="same")
        y = y / y.std() * rng.uniform(5, 30) * (1 + 0.3 * np.sin(2 * np.pi * rng.uniform(0.1, 0.5) * t))
    y = y + rng.uniform(2083, 2117)
    return np.clip(np.round(y), 0, 4095).astype(np.uint16)


def main() -> int:
    ap = argparse.ArgumentParser(description="BACKUP dataset: ESC-50 converted directly to ADC-like samples.")
    ap.add_argument("--esc50-dir", type=Path, default=DATA_DIR / "esc50")
    ap.add_argument("--per-category", type=int, default=40, help="clips per category (ESC-50 has 40)")
    ap.add_argument("--volumes", type=float, nargs="+", default=[1.0, 0.5])
    ap.add_argument("--ambient", type=int, default=80, help="synthetic ambient takes")
    ap.add_argument("--out-dir", type=Path, default=DATA_DIR / "direct")
    ap.add_argument("--out", type=Path, default=DATA_DIR / "features_esc50_direct.csv")
    args = ap.parse_args()

    import soundfile as sf
    from scipy.signal import resample_poly
    from prepare_esc50 import select

    meta = args.esc50_dir / "meta" / "esc50.csv"
    if not meta.exists():
        print(f"error: {meta} not found (see prepare_esc50.py for the download)", file=sys.stderr)
        return 1
    label_map = load_label_map()
    rows = []
    for cat, fold, name in select(read_csv(meta), label_map, args.per_category):
        c = label_map[cat]
        clip = Path(name).stem
        todo = [(v, args.out_dir / c.name / f"{clip}_v{v:.1f}.npy") for v in args.volumes]
        if not all(p.exists() for _, p in todo):
            x, sr = sf.read(str(args.esc50_dir / "audio" / name), dtype="float64", always_2d=True)
            x = resample_poly(x.mean(axis=1), 160, 441) if sr == 44100 else resample_poly(x.mean(axis=1), RATE, sr)
            for v, p in todo:
                p.parent.mkdir(parents=True, exist_ok=True)
                np.save(p, to_adc(x, rng_for(f"{clip}_{v}"), v))
        for v, p in todo:
            rows.append({"file": rel(p), "clip_id": clip, "source": "esc50_direct", "label_id": c.id,
                         "group_id": c.group, "split_key": fold, "volume": v, "take": 0})
    for k in range(args.ambient):
        p = args.out_dir / "AMBIENT" / f"synthetic_{k:03d}.npy"
        if not p.exists():
            p.parent.mkdir(parents=True, exist_ok=True)
            np.save(p, ambient_take(k))
        rows.append({"file": rel(p), "clip_id": f"synthetic_{k:03d}", "source": "synthetic_ambient",
                     "label_id": sc.by_name("AMBIENT").id, "group_id": 0, "split_key": k % 5 + 1, "volume": 0.0,
                     "take": 0})

    names, feats = featx([DATA_DIR.parent / r["file"] for r in rows])
    out = []
    for r in rows:
        v = feats.get(str(DATA_DIR.parent / r["file"]))
        if v is not None:
            out.append({**r, **v})
    write_csv(args.out, out, META_FIELDS + names)
    print(f"{len(out)} takes ({len(rows) - len(out)} failed) -> {rel(args.out)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
