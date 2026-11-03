#!/usr/bin/env python3
"""
playback.py - play an audio file through a chosen output device, blocking until done.

Cross-platform (sounddevice + soundfile); no afplay or other OS tools. Used by
record_dataset.py to play clips at the Pico mic.

    from playback import play
    play("ml/data/esc50/audio/1-100032-A-0.wav", volume=0.5)

Run:      python ml/playback.py --list-devices
          python ml/playback.py clip.wav --volume 0.5 --device 3
          python ml/playback.py clip.wav --dry-run       (load and report, no sound)
"""
import argparse
import sys
from pathlib import Path

import numpy as np
import soundfile as sf


def load(path, volume: float = 1.0):
    """Read a file as float32 and scale it by volume (clipped to [-1, 1]). Returns (data, rate)."""
    if not 0.0 <= volume <= 1.0:
        raise ValueError(f"volume must be in 0..1, got {volume}")
    data, rate = sf.read(str(path), dtype="float32", always_2d=True)
    return np.clip(data * volume, -1.0, 1.0), rate


def play(path, volume: float = 1.0, device=None) -> None:
    """Play path at volume (0..1) on device (index or name; None = default output). Blocks."""
    import sounddevice as sd  # imported here so --dry-run works without an audio device
    data, rate = load(path, volume)
    sd.play(data, rate, device=device)
    sd.wait()


def list_devices() -> None:
    import sounddevice as sd
    print(sd.query_devices())


def main() -> int:
    ap = argparse.ArgumentParser(description="Play an audio file (blocking) on a chosen output device.")
    ap.add_argument("path", nargs="?", type=Path, help="audio file (wav, flac, ogg, mp3)")
    ap.add_argument("--volume", type=float, default=1.0, help="0..1 (default 1.0)")
    ap.add_argument("--device", default=None, help="output device index or name (see --list-devices)")
    ap.add_argument("--list-devices", action="store_true", help="list audio devices and exit")
    ap.add_argument("--dry-run", action="store_true", help="load the file and report it, play nothing")
    args = ap.parse_args()

    if args.list_devices:
        list_devices()
        return 0
    if args.path is None:
        ap.error("path is required unless --list-devices is given")
    device = int(args.device) if args.device is not None and args.device.isdigit() else args.device
    if args.dry_run:
        data, rate = load(args.path, args.volume)
        print(f"dry-run: {args.path.name} rate={rate} channels={data.shape[1]} "
              f"seconds={len(data) / rate:.2f} peak={float(np.abs(data).max()):.3f}")
        return 0
    play(args.path, args.volume, device)
    return 0


if __name__ == "__main__":
    sys.exit(main())
