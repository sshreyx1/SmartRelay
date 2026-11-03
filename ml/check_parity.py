#!/usr/bin/env python3
"""
check_parity.py - the Pico's own feature vector (@FEAT from audio_dump.uf2) must match sound_featx on
the same samples within 1e-3 relative. Proves the laptop-trained model sees what the node computes.

For each take: record 5 s on the Pico (make some noise: talk, clap, play a clip), ask the Pico for
its features, save the samples, run sound_featx on them, compare. Prints the max difference per feature.

Run:      python ml/check_parity.py --takes 5
          python ml/check_parity.py --dry-run          (no hardware: sound_featx against itself)
"""
import argparse
import sys
import tempfile
from pathlib import Path

import numpy as np

from extract_features import featx

REL_TOL = 1e-3
ABS_TOL = 1e-5  # for features that are ~0 (e.g. active_frac of a steady sound)


def compare(names: list, pico: np.ndarray, host: np.ndarray) -> np.ndarray:
    """Relative difference per feature (with a small absolute floor)."""
    return np.abs(pico - host) / np.maximum(np.maximum(np.abs(pico), np.abs(host)), ABS_TOL / REL_TOL)


def report(names: list, diffs: list) -> bool:
    d = np.max(np.array(diffs), axis=0)
    print(f"{'feature':<20} max rel diff")
    for n, v in zip(names, d):
        print(f"{n:<20} {v:.2e}{'  <-- FAIL' if v > REL_TOL else ''}")
    ok = bool(np.all(d <= REL_TOL))
    print(f"parity: {'PASS' if ok else 'FAIL'} over {len(diffs)} takes (tolerance {REL_TOL:g} relative)")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description="Compare on-Pico features with sound_featx on the same samples.")
    ap.add_argument("--takes", type=int, default=5)
    ap.add_argument("--port")
    ap.add_argument("--dry-run", action="store_true", help="no hardware: check sound_featx and the comparison")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        diffs, names = [], None
        if args.dry_run:
            rng = np.random.default_rng(0)
            t = np.arange(80000) / 16000
            x = (2100 + 500 * np.sin(2 * np.pi * 800 * t) + rng.normal(0, 20, t.size)).clip(0, 4095).astype(np.uint16)
            p = Path(tmp) / "dry.npy"
            np.save(p, x)
            names, f = featx([p])
            v = np.array([f[str(p)][n] for n in names if not n.startswith("legacy_")], dtype=np.float32)
            names = [n for n in names if not n.startswith("legacy_")]
            diffs.append(compare(names, v, v))
            return 0 if report(names, diffs) else 1

        from pico_audio import PicoAudio
        with PicoAudio(args.port) as pico:
            for i in range(args.takes):
                input(f"take {i + 1}/{args.takes}: press Enter, then make a sound for 5 s ...")
                x = pico.record(5)
                fp = pico.features()
                p = Path(tmp) / f"take{i}.npy"
                np.save(p, x)
                all_names, f = featx([p])
                names = [n for n in all_names if not n.startswith("legacy_")]
                fh = np.array([f[str(p)][n] for n in names], dtype=np.float32)
                if len(fp) != len(fh):
                    print(f"error: Pico sent {len(fp)} features, sound_featx {len(fh)} (different SF_VERSION?)")
                    return 1
                diffs.append(compare(names, fp, fh))
                print(f"  take {i + 1}: max rel diff {diffs[-1].max():.2e}")
        return 0 if report(names, diffs) else 1


if __name__ == "__main__":
    sys.exit(main())
