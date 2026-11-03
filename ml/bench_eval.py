#!/usr/bin/env python3
"""
bench_eval.py - on-device evaluation: play every test-fold clip at the Pico running audio_dump.uf2,
let the PICO classify what it heard (@CLS), and compare with the truth and with the offline numbers.

Writes docs/sound/evidence/bench_eval_<phase>_<date>.csv (one row per clip) and
docs/sound/figures/confusion_bench_{group,label}.png (--tag adds a suffix to both), and prints on-device vs offline metrics
(offline = ml/models/metrics_<phase>.json, test fold).

Run:      python ml/bench_eval.py --device 3                 (all test-fold clips, volume 1.0)
          python ml/bench_eval.py --device 3 --per-label 2   (quick: 2 clips per label)
          python ml/bench_eval.py --dry-run
"""
import argparse
import json
import sys
from collections import defaultdict
from datetime import date

import numpy as np

from mlcommon import DATA_DIR, ML_DIR, REPO_ROOT, read_csv, resolve, sc, write_csv
from record_dataset import record_one

FIELDS = ["clip_id", "true_id", "true_name", "true_group", "pico_id", "pico_name", "pico_group", "conf", "model_us",
          "volume"]


def main() -> int:
    ap = argparse.ArgumentParser(description="Play test-fold clips at the Pico and score its own classification.")
    ap.add_argument("--manifest", type=lambda s: resolve(s), default=DATA_DIR / "manifest.csv")
    ap.add_argument("--split-key", default="5", help="which split_key is the test fold")
    ap.add_argument("--per-label", type=int, help="at most this many clips per label")
    ap.add_argument("--volume", type=float, default=1.0)
    ap.add_argument("--phase", default="A")
    ap.add_argument("--tag", default="", help="added to the output names (e.g. board2), so runs do not overwrite each other")
    ap.add_argument("--port")
    ap.add_argument("--device", help="output (speaker) device index or name")
    ap.add_argument("--pre-ms", type=int, default=150)
    ap.add_argument("--dry-run", action="store_true", help="list the clips, touch no hardware")
    args = ap.parse_args()

    from train import confusion_png, macro_f1

    if not args.manifest.exists():
        print(f"error: {args.manifest} not found (prepare_esc50.py)", file=sys.stderr)
        return 1
    rows = [r for r in read_csv(args.manifest) if r["split_key"] == args.split_key]
    if args.per_label:
        seen = defaultdict(int)
        keep = []
        for r in rows:
            if seen[r["label_id"]] < args.per_label:
                seen[r["label_id"]] += 1
                keep.append(r)
        rows = keep
    print(f"{len(rows)} test clips, about {len(rows) * 7 / 60:.0f} min")
    if args.dry_run or not rows:
        return 0

    from pico_audio import PicoAudio
    device = int(args.device) if args.device and args.device.isdigit() else args.device
    out = []
    with PicoAudio(args.port) as pico:
        for i, r in enumerate(rows, 1):
            job = {"src": resolve(r["src_path"]), "volume": args.volume}
            record_one(pico, job, args.pre_ms, device)
            c = pico.classify()
            tid = int(r["label_id"])
            out.append({"clip_id": r["clip_id"], "true_id": tid, "true_name": sc.name(tid),
                        "true_group": sc.group_name(tid), "pico_id": c["id"], "pico_name": c["name"],
                        "pico_group": c.get("group"), "conf": c["conf"], "model_us": c.get("model_us"),
                        "volume": args.volume})
            ok = "ok " if c["id"] == tid else ("grp" if sc.group(c["id"]) == sc.group(tid) else "   ")
            print(f"[{i}/{len(rows)}] {ok} true {sc.name(tid):<12} pico {c['name']:<12} {c['conf']:>3}%")

    stamp = date.today().isoformat() + (f"_{args.tag}" if args.tag else "")
    suffix = f"_{args.tag}" if args.tag else ""
    write_csv(REPO_ROOT / "docs" / "sound" / "evidence" / f"bench_eval_{args.phase}_{stamp}.csv", out, FIELDS)
    yt = np.array([o["true_id"] for o in out])
    yp = np.array([o["pico_id"] for o in out])
    gt = np.array([sc.group(int(i)) for i in yt])
    gp = np.array([sc.group(int(i)) if sc.group(int(i)) is not None else 255 for i in yp])
    fig = REPO_ROOT / "docs" / "sound" / "figures"
    confusion_png(gt, gp, sc.GROUP_IDS, f"Group, on-device bench (n={len(out)})", fig / f"confusion_bench_group{suffix}.png")
    ids = sorted(set(yt.tolist()) | set(yp.tolist()))
    confusion_png(yt, yp, ids, f"Label, on-device bench (n={len(out)})", fig / f"confusion_bench_label{suffix}.png")

    bench = {"group_accuracy": float((gt == gp).mean()), "group_macro_f1": macro_f1(gt, gp),
             "label_accuracy": float((yt == yp).mean()), "label_macro_f1": macro_f1(yt, yp)}
    offline = {}
    mpath = ML_DIR / "models" / f"metrics_{args.phase}.json"
    if mpath.exists():
        offline = json.loads(mpath.read_text(encoding="utf-8"))["test"]
    print(f"\n{'metric':<18} {'on-device':>10} {'offline':>10}")
    for k, v in bench.items():
        off = offline.get(k)
        print(f"{k:<18} {v:>10.3f} {off if off is None else f'{off:.3f}':>10}")
    us = [o["model_us"] for o in out if o["model_us"] is not None]
    if us:
        print(f"model time on the Pico: mean {np.mean(us):.0f} us, max {max(us)} us")
    gap = (offline.get("group_macro_f1") or 0) - bench["group_macro_f1"]
    print(f"done when on-device macro-F1 is within ~0.1 of offline: group gap {gap:+.3f} "
          f"-> {'OK' if gap <= 0.1 else 'NOT YET'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
