#!/usr/bin/env python3
"""
record_dataset.py - play every manifest clip at the Pico mic and keep what the Pico heard.

For each manifest row x each --volumes value x each take: start PicoAudio.record(5) in a thread,
wait --pre-ms (150 ms), then play the clip on the chosen speaker. Saves
    ml/data/raw/<LABEL>/<clip_id>_v<vol>_t<take>.npy     raw uint16 ADC samples (the source of truth)
    ml/data/raw/<LABEL>/<clip_id>_v<vol>_t<take>.wav     listenable int16 copy, (x - mean) * 16
and appends a row to ml/data/recordings.csv (file, clip_id, label_id, group_id, split_key, volume,
take, mean_adc, crc_ok, ts). Resumable: files that already exist are skipped, so just run it again.

--ambient K records K x 5 s with no playback (label AMBIENT). Vary the room between runs (quiet,
outdoors, fan on) and say which with --note. Ambient takes get split_key 1-5 round-robin.

Run:      python ml/record_dataset.py --list-devices
          python ml/record_dataset.py --dry-run                       (plan + ETA, no hardware)
          python ml/record_dataset.py --device 3 --volumes 1.0 0.5
          python ml/record_dataset.py --ambient 10 --note fan
Phase B reuses it with --manifest ml/data/xc_manifest.csv.
"""
import argparse
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

from mlcommon import DATA_DIR, append_csv, read_csv, rel, resolve, sc

REC_FIELDS = ["file", "clip_id", "label_id", "group_id", "split_key", "volume", "take", "mean_adc",
              "crc_ok", "ts"]
SECONDS = 5


def take_path(out_dir: Path, label_id: int, clip_id: str, volume: float, take: int) -> Path:
    return out_dir / sc.name(label_id) / f"{clip_id}_v{volume:.1f}_t{take}.npy"


def plan(manifest: list, volumes: list, takes: int, ambient: int, note: str, out_dir: Path) -> list:
    """Every take to record as dicts; those whose .npy already exists are marked done."""
    jobs = []
    for row in manifest:
        for vol in volumes:
            for t in range(takes):
                jobs.append({"clip_id": row["clip_id"], "label_id": int(row["label_id"]),
                             "group_id": int(row["group_id"]), "split_key": row["split_key"], "volume": vol,
                             "take": t, "src": resolve(row["src_path"])})
    for k in range(ambient):
        jobs.append({"clip_id": f"ambient_{note}_{k:03d}", "label_id": sc.by_name("AMBIENT").id, "group_id": 0,
                     "split_key": str(k % 5 + 1), "volume": 0.0, "take": 0, "src": None})
    for j in jobs:
        j["path"] = take_path(out_dir, j["label_id"], j["clip_id"], j["volume"], j["take"])
        j["done"] = j["path"].exists()
    return jobs


def save_take(path: Path, x: np.ndarray) -> None:
    import soundfile as sf
    path.parent.mkdir(parents=True, exist_ok=True)
    np.save(path, x)
    xf = x.astype(np.float64)
    sf.write(str(path.with_suffix(".wav")), np.clip((xf - xf.mean()) * 16, -32768, 32767).astype(np.int16), 16000)


def record_one(pico, job: dict, pre_ms: int, device) -> np.ndarray:
    """Record SECONDS while playing the clip pre_ms after the recording starts."""
    from playback import play
    result, error = {}, {}

    def rec():
        try:
            result["x"] = pico.record(SECONDS)
        except Exception as e:  # reported below, after playback finishes
            error["e"] = e

    th = threading.Thread(target=rec)
    th.start()
    if job["src"] is not None:
        time.sleep(pre_ms / 1000.0)
        play(job["src"], volume=job["volume"], device=device)
    th.join()
    if "e" in error:
        raise error["e"]
    return result["x"]


def main() -> int:
    ap = argparse.ArgumentParser(description="Record manifest clips through the Pico mic (audio_dump.uf2).")
    ap.add_argument("--manifest", type=Path, default=DATA_DIR / "manifest.csv")
    ap.add_argument("--volumes", type=float, nargs="+", default=[1.0, 0.5])
    ap.add_argument("--takes", type=int, default=1)
    ap.add_argument("--ambient", type=int, default=0, metavar="K", help="also record K ambient takes (no playback)")
    ap.add_argument("--ambient-only", action="store_true", help="skip the manifest, record only --ambient")
    ap.add_argument("--note", default="room", help="ambient setting, part of the file name (room, outdoor, fan)")
    ap.add_argument("--labels", nargs="*", help="only these label names (e.g. DOG CAT)")
    ap.add_argument("--limit", type=int, help="stop after this many new takes")
    ap.add_argument("--port", help="Pico serial port (default: auto-detect)")
    ap.add_argument("--device", help="output (speaker) device index or name, see --list-devices")
    ap.add_argument("--list-devices", action="store_true")
    ap.add_argument("--pre-ms", type=int, default=150, help="start playback this long after recording starts")
    ap.add_argument("--out", type=Path, default=DATA_DIR / "raw")
    ap.add_argument("--csv", type=Path, default=DATA_DIR / "recordings.csv")
    ap.add_argument("--dry-run", action="store_true", help="print the plan and ETA, touch no hardware")
    args = ap.parse_args()

    if args.list_devices:
        from playback import list_devices
        list_devices()
        return 0
    manifest = [] if args.ambient_only else read_csv(args.manifest) if args.manifest.exists() else None
    if manifest is None:
        print(f"error: {args.manifest} not found; run prepare_esc50.py first", file=sys.stderr)
        return 1
    if args.labels:
        want = {sc.by_name(n).id for n in args.labels}
        manifest = [r for r in manifest if int(r["label_id"]) in want]
    jobs = plan(manifest, args.volumes, args.takes, args.ambient, args.note, args.out)
    todo = [j for j in jobs if not j["done"]]
    if args.limit:
        todo = todo[:args.limit]
    missing = [j for j in todo if j["src"] is not None and not j["src"].exists()]
    per_take = SECONDS + 4.5  # 5 s recording + ~3 s base64 transfer + playback start
    print(f"{len(jobs)} takes planned, {len(jobs) - len(todo)} already recorded, {len(todo)} to do, "
          f"ETA {len(todo) * per_take / 60:.0f} min")
    if missing:
        print(f"error: {len(missing)} source files missing, e.g. {missing[0]['src']}", file=sys.stderr)
        return 1
    if args.dry_run or not todo:
        return 0

    from pico_audio import PicoAudio
    device = int(args.device) if args.device and args.device.isdigit() else args.device
    t_start = time.monotonic()
    with PicoAudio(args.port) as pico:
        print(f"Pico on {pico.port}")
        for i, job in enumerate(todo, 1):
            ok, x = True, None
            try:
                x = record_one(pico, job, args.pre_ms, device)
                save_take(job["path"], x)
            except Exception as e:
                ok = False
                print(f"  take failed: {e}", file=sys.stderr)
            append_csv(args.csv, {
                "file": rel(job["path"]), "clip_id": job["clip_id"], "label_id": job["label_id"],
                "group_id": job["group_id"], "split_key": job["split_key"], "volume": job["volume"],
                "take": job["take"], "mean_adc": getattr(pico, "last_mean", "") if ok else "", "crc_ok": int(ok),
                "ts": datetime.now(timezone.utc).isoformat(timespec="seconds")}, REC_FIELDS)
            el = time.monotonic() - t_start
            eta = el / i * (len(todo) - i)
            print(f"[{i}/{len(todo)}] {sc.name(job['label_id']):<12} {job['path'].name}  "
                  f"{'ok' if ok else 'FAILED'}  ETA {eta / 60:.0f} min")
    return 0


if __name__ == "__main__":
    sys.exit(main())
