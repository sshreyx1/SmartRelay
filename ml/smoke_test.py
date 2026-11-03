#!/usr/bin/env python3
"""
smoke_test.py - check the sound ML tooling works on this machine (macOS or Windows), no hardware needed.

Checks: every requirement imports, serial ports can be listed (Picos marked by USB VID 0x2E8A),
every CLI answers --help, and each CLI's no-hardware dry run succeeds. Every task that adds a
Python tool adds it to CLIS / DRY_RUNS below.

Run (from the repo root, with the venv's Python):
    macOS:    ml/.venv/bin/python ml/smoke_test.py
    Windows:  ml\\.venv\\Scripts\\python ml\\smoke_test.py
Paste the whole output (it starts with the platform line) when asked for a Windows check.
"""
import importlib
import platform
import subprocess
import sys
import tempfile
from pathlib import Path

ML_DIR = Path(__file__).resolve().parent
TOOLS_DIR = ML_DIR.parent / "tools"
PICO_VID = 0x2E8A

# module name -> pip package (requirements.txt)
IMPORTS = {
    "numpy": "numpy", "scipy": "scipy", "pandas": "pandas", "sklearn": "scikit-learn",
    "soundfile": "soundfile", "sounddevice": "sounddevice", "serial": "pyserial",
    "yaml": "PyYAML", "matplotlib": "matplotlib", "PIL": "pillow", "pytest": "pytest",
}

# scripts that must answer --help (ml/ unless a path is given)
CLIS = ["hosttools.py", "playback.py", "pico_audio.py", "prepare_esc50.py", "record_dataset.py",
        "extract_features.py", "check_parity.py", "plots.py", "esc50_direct.py", "train.py", "export_c.py",
        "bench_eval.py", TOOLS_DIR / "make_label_images.py", TOOLS_DIR / "fetch_label_images.py",
        TOOLS_DIR / "gen_sound_tables.py", TOOLS_DIR / "bridge_receiver.py"]


def dry_runs(tmp: Path) -> list:
    """(name, argv) pairs for no-hardware, no-network dry runs; tmp holds the inputs they need:
    a 2-category fake ESC-50 (meta csv + short wavs) and a folder of label photos."""
    import numpy as np
    import soundfile as sf
    from PIL import Image
    tone = tmp / "tone.wav"
    t = np.arange(1600) / 16000.0
    sf.write(str(tone), (0.3 * np.sin(2 * np.pi * 1000 * t)).astype("float32"), 16000)

    esc = tmp / "esc50"
    (esc / "meta").mkdir(parents=True)
    (esc / "audio").mkdir()
    rows = ["filename,fold,target,category,esc10,src_file,take"]
    for cat, tgt in (("dog", 0), ("rain", 10)):
        for fold in range(1, 6):
            name = f"{fold}-{tgt}{fold}-A-{tgt}.wav"
            sf.write(str(esc / "audio" / name), (0.2 * np.sin(2 * np.pi * 500 * t)).astype("float32"), 44100)
            rows.append(f"{name},{fold},{tgt},{cat},False,{tgt}{fold},A")
    (esc / "meta" / "esc50.csv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    manifest = tmp / "manifest.csv"

    src = tmp / "images_src" / "dog"
    src.mkdir(parents=True)
    Image.new("RGB", (640, 480), (90, 140, 60)).save(src / "a.jpg", quality=95)
    return [
        ("playback --dry-run", [ML_DIR / "playback.py", tone, "--volume", "0.5", "--dry-run"]),
        ("prepare_esc50 (fake ESC-50)", [ML_DIR / "prepare_esc50.py", "--esc50-dir", esc, "--per-category", "5",
                                          "--out", manifest]),
        ("record_dataset --dry-run", [ML_DIR / "record_dataset.py", "--manifest", manifest, "--ambient", "3",
                                      "--out", tmp / "raw", "--dry-run"]),
        ("bench_eval --dry-run", [ML_DIR / "bench_eval.py", "--manifest", manifest, "--dry-run"]),
        ("make_label_images", [TOOLS_DIR / "make_label_images.py", "--src", tmp / "images_src", "--out",
                               tmp / "samples", "--credits", tmp / "credits.md"]),
        ("check_parity --dry-run (needs the host build)", [ML_DIR / "check_parity.py", "--dry-run"]),
    ]


class Results:
    def __init__(self):
        self.passed = 0
        self.failed = []

    def check(self, name: str, ok: bool, detail: str = "") -> None:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f"  ({detail})" if detail else ""))
        if ok:
            self.passed += 1
        else:
            self.failed.append(name)


def run(argv: list) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable] + [str(a) for a in argv], capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=120)


def last_line(text: str) -> str:
    lines = [ln for ln in text.strip().splitlines() if ln.strip()]
    return lines[-1] if lines else ""


def main() -> int:
    r = Results()
    print(f"platform: {platform.system()} {platform.release()} {platform.machine()}  "
          f"python {platform.python_version()}  ({sys.executable})")

    print("imports:")
    for mod, pkg in IMPORTS.items():
        try:
            m = importlib.import_module(mod)
            r.check(pkg, True, getattr(m, "__version__", ""))
        except Exception as e:  # sounddevice raises OSError if PortAudio is missing
            r.check(pkg, False, f"{type(e).__name__}: {e}")

    print("serial ports:")
    try:
        from serial.tools import list_ports
        ports = list(list_ports.comports())
        for p in ports:
            tag = "  <- Pico" if p.vid == PICO_VID else ""
            print(f"    {p.device}  {p.description}{tag}")
        n_pico = sum(1 for p in ports if p.vid == PICO_VID)
        r.check("list_ports", True, f"{len(ports)} port(s), {n_pico} Pico(s)")
    except Exception as e:
        r.check("list_ports", False, f"{type(e).__name__}: {e}")

    print("CLI --help:")
    for script in CLIS:
        path = script if isinstance(script, Path) else ML_DIR / script
        p = run([path, "--help"])
        r.check(path.name, p.returncode == 0 and "usage" in p.stdout.lower(),
                "" if p.returncode == 0 else last_line(p.stderr))

    print("dry runs:")
    with tempfile.TemporaryDirectory() as tmp:
        try:
            cases = dry_runs(Path(tmp))
        except Exception as e:
            cases = []
            r.check("prepare dry-run inputs", False, f"{type(e).__name__}: {e}")
        for name, argv in cases:
            p = run(argv)
            r.check(name, p.returncode == 0, last_line(p.stdout) if p.returncode == 0 else last_line(p.stderr))

    print(f"smoke test: {r.passed} passed, {len(r.failed)} failed"
          + (f"  -> {', '.join(r.failed)}" if r.failed else ""))
    return 1 if r.failed else 0


if __name__ == "__main__":
    sys.exit(main())
