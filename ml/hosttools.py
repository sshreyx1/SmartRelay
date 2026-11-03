#!/usr/bin/env python3
"""
hosttools.py - find the host C tools built from host/CMakeLists.txt (e.g. sound_featx).

Works on macOS/Linux (build/sound_featx) and Windows (build\\sound_featx.exe, or
build\\Debug|Release\\sound_featx.exe with the MSVC multi-config generators).

    from hosttools import find_host_tool
    exe = find_host_tool("sound_featx")          # Path, or raises FileNotFoundError

Run:      python ml/hosttools.py sound_featx     (prints the path it found)
"""
import argparse
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD_DIR = REPO_ROOT / "build"
CONFIG_SUBDIRS = ("", "Release", "RelWithDebInfo", "Debug", "MinSizeRel")


def candidates(name: str, build_dir: Path = DEFAULT_BUILD_DIR) -> list:
    """Every path a built host tool may live at, in the order they are tried."""
    out = []
    for sub in CONFIG_SUBDIRS:
        d = build_dir / sub if sub else build_dir
        out.append(d / name)
        out.append(d / (name + ".exe"))
    return out


def find_host_tool(name: str, build_dir: Path = DEFAULT_BUILD_DIR) -> Path:
    """Return the built tool under build_dir, else one on PATH; raise if neither exists."""
    for p in candidates(name, Path(build_dir)):
        if p.is_file():
            return p
    on_path = shutil.which(name)
    if on_path:
        return Path(on_path)
    raise FileNotFoundError(
        f"{name} not found under {build_dir} or on PATH. Build it first:\n"
        "  cmake -S host -B build && cmake --build build -j")


def main() -> int:
    ap = argparse.ArgumentParser(description="Locate a built host tool (sound_featx, ...).")
    ap.add_argument("name", help="tool name without .exe, e.g. sound_featx")
    ap.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    args = ap.parse_args()
    try:
        print(find_host_tool(args.name, args.build_dir))
    except FileNotFoundError as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
