#!/usr/bin/env python3
"""
demo_playlist.py - choose the demo clips: per sound group, the 2 test-fold clips the classifier gets right with the
highest confidence. Copies them to ml/demo_clips/ and writes ml/demo_clips/playlist.csv (play order, truth,
prediction, confidence, the playback command).

Two sources of predictions:
  offline (default)  the exported model (ml/models/model_<phase>.pkl) run with ml/qmodel.py, which computes exactly
                     what the node computes, on that model's own test-fold features. Before the live test.
  --live CSV         what the Pico said on the bench: docs/sound/evidence/bench_eval_<phase>_<date>.csv from
                     bench_eval.py (columns clip_id, true_id, pico_id, conf). Use this after H6.

"Correct" means the exact label. A group-only answer for an animal (e.g. MAMMAL for a dog) is kept as a fallback,
ranked below every exact hit. A clip recorded more than once (volumes, takes) counts only if every take is correct;
its confidence is the lowest one. AMBIENT has no clip to play (it is the quiet room), so it is skipped.

Run:      python ml/demo_playlist.py --device 4
          python ml/demo_playlist.py --live ../docs/sound/evidence/bench_eval_A_2026-11-03.csv
"""
import argparse
import pickle
import shutil
import sys
from collections import defaultdict

import numpy as np

from mlcommon import DATA_DIR, ML_DIR, REPO_ROOT, read_csv, rel, resolve, sc, write_csv

OUT = ML_DIR / "demo_clips"
ESC_AUDIO = DATA_DIR / "esc50" / "audio"
FIELDS = ["order", "group", "clip_id", "true_name", "predicted", "conf", "match", "takes", "source", "file", "play"]


def offline_predictions(phase: str) -> list:
    """(clip_id, true id, predicted id, conf) for every test-fold take, from the exported model."""
    import pandas as pd
    pkl = ML_DIR / "models" / f"model_{phase}.pkl"
    if not pkl.exists():
        sys.exit(f"error: {pkl} not found, run train.py first")
    with open(pkl, "rb") as f:
        d = pickle.load(f)
    csvs = d["features_csv"] if isinstance(d["features_csv"], list) else [d["features_csv"]]
    df = pd.concat([pd.read_csv(resolve(c)) for c in csvs], ignore_index=True).loc[d["test_rows"]]
    label, conf, *_ = d["model"].predict(df[d["features"]].to_numpy(np.float32))
    return [(c, int(t), int(p), int(k)) for c, t, p, k in zip(df["clip_id"], df["label_id"], label, conf)]


def live_predictions(path) -> list:
    return [(r["clip_id"], int(r["true_id"]), int(r["pico_id"]), int(r["conf"])) for r in read_csv(path)]


def pick(preds: list, per_group: int = 2) -> dict:
    """{group id: [clip dicts]}: the best `per_group` correct clips per group, exact labels first, then confidence."""
    by_clip = defaultdict(list)
    for clip, true_id, pred_id, conf in preds:
        by_clip[clip].append((true_id, pred_id, conf))
    cands = defaultdict(list)
    for clip, takes in by_clip.items():
        true_id = takes[0][0]
        exact = all(p == true_id for _, p, _ in takes)
        grp = all(p == true_id or p == sc.group(true_id) for _, p, _ in takes)
        if not grp:
            continue
        cands[sc.group(true_id)].append({
            "clip_id": clip, "true_id": true_id, "pred_id": takes[0][1] if exact else sc.group(true_id),
            "conf": min(c for _, _, c in takes), "match": "exact" if exact else "group", "takes": len(takes)})
    return {g: sorted(c, key=lambda x: (x["match"] != "exact", -x["conf"], x["clip_id"]))[:per_group]
            for g, c in cands.items()}


def main() -> int:
    ap = argparse.ArgumentParser(description="Pick the best demo clips per sound group (offline model or live bench).")
    ap.add_argument("--live", type=lambda s: resolve(s), help="bench_eval CSV with the Pico's own answers")
    ap.add_argument("--phase", default="A")
    ap.add_argument("--per-group", type=int, default=2)
    ap.add_argument("--device", default="<N>", help="speaker device for the play commands (playback.py --list-devices)")
    args = ap.parse_args()

    if args.live:
        preds, source = live_predictions(args.live), f"live: {args.live.name}"
    else:
        preds, source = offline_predictions(args.phase), f"offline: model_{args.phase}.pkl test fold"
    chosen = pick(preds, args.per_group)

    OUT.mkdir(exist_ok=True)
    for old in list(OUT.glob("*.wav")):
        old.unlink()
    rows = []
    for g in sc.GROUP_IDS:
        if g == sc.by_name("AMBIENT").id:
            continue
        for c in chosen.get(g, []):
            src = ESC_AUDIO / f"{c['clip_id']}.wav"
            if not src.exists():
                print(f"warning: {src} missing, skipped")
                continue
            dst = OUT / f"{len(rows) + 1:02d}_{sc.name(c['true_id']).lower()}_{c['clip_id']}.wav"
            shutil.copyfile(src, dst)
            rows.append({"order": len(rows) + 1, "group": sc.name(g), "clip_id": c["clip_id"],
                         "true_name": sc.name(c["true_id"]), "predicted": sc.name(c["pred_id"]), "conf": c["conf"],
                         "match": c["match"], "takes": c["takes"], "source": source, "file": rel(dst),
                         "play": f"python ml/playback.py {dst.relative_to(REPO_ROOT).as_posix()} --device {args.device}"})
    write_csv(OUT / "playlist.csv", rows, FIELDS)

    print(f"{source}: {len(preds)} predictions, {len(rows)} clips chosen -> {rel(OUT / 'playlist.csv')}")
    print(f"{'#':>2}  {'group':<12} {'true':<12} {'predicted':<12} {'conf':>4}  match  clip")
    for r in rows:
        print(f"{r['order']:>2}  {r['group']:<12} {r['true_name']:<12} {r['predicted']:<12} {r['conf']:>3}%  "
              f"{r['match']:<5}  {r['clip_id']}")
    missing = [sc.name(g) for g in sc.GROUP_IDS if g != sc.by_name("AMBIENT").id and not chosen.get(g)]
    if missing:
        print(f"no correct clip for: {', '.join(missing)} (leave these out of the demo)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
