#!/bin/bash
# check_style.sh - the coding-standard check (docs/CODING_STANDARD.md): BARR-C:2018 layout with
# clang-format, the source-level rules with tools/check_barrc.py, and a warnings-as-errors compile
# of the files that build on a laptop.
#
# A tool that is not installed prints "SKIPPED" and is not counted as a pass or a failure.
#
# Run from anywhere: tools/check_style.sh
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
FAIL=0
PY="$(command -v python3 || command -v python || true)"

project_files() {
    git ls-files '*.c' '*.h' | grep -v -e '^third_party/lora_mesh/src/' -e '^third_party/mtk3smp_rp2040'
}

echo "=== clang-format (BARR-C layout, dry run) ==="
if command -v clang-format >/dev/null 2>&1; then
    BAD=0
    while IFS= read -r f; do
        clang-format -style=file --dry-run --Werror "$f" || BAD=1
    done < <(project_files)
    if [ "$BAD" -ne 0 ]; then FAIL=1; else echo "OK"; fi
else
    echo "SKIPPED: clang-format not installed (pip install clang-format)"
fi

echo "=== tools/check_barrc.py (BARR-C source rules) ==="
if [ -n "$PY" ]; then
    "$PY" tools/check_barrc.py || FAIL=1
else
    echo "SKIPPED: python not installed"
fi

echo "=== warnings-as-errors compile of the laptop-buildable files ==="
if command -v gcc >/dev/null 2>&1; then
    WARN="-std=c11 -Wall -Wextra -Wconversion -Wno-sign-conversion -Wshadow -Wundef -Werror"
    BAD=0
    for f in common/loopback_radio.c common/uplink.c alfp/src/*.c; do
        gcc $WARN -c -o /dev/null -I alfp/include -I mesh -I common "$f" || BAD=1
    done
    for f in audio_features sound_features sound_fft sound_classify sound_image_pick sound_tables; do
        gcc $WARN -c -o /dev/null -I firmware/sensing -I common -I third_party/sensors \
            "firmware/sensing/$f.c" || BAD=1
    done
    if [ "$BAD" -ne 0 ]; then FAIL=1; else echo "OK"; fi
else
    echo "SKIPPED: gcc not installed"
fi

if [ "$FAIL" -ne 0 ]; then
    echo "check_style.sh: FAILED"
    exit 1
fi
echo "check_style.sh: PASSED (see any SKIPPED lines for what was not checked)"
