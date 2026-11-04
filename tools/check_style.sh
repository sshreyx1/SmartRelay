#!/bin/bash
# check_style.sh - format check + lint + warnings-as-errors compile check over the BARR-C-scoped
# C files (see docs/CODING_STANDARD.md for the scope and the exact commands this wraps).
#
# Degrades gracefully: a tool that is not installed prints "SKIPPED: <tool> not installed" and is
# not counted as a failure. Exits non-zero only for an actual violation a tool could check.
#
# Run from the repo root: tools/check_style.sh
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

FAIL=0

# In-scope C/H files: alfp/**, common/**, firmware/app/**, firmware/sensing/**, firmware_smp/**
# excluding the vendored kernel fork.
IN_SCOPE_FILES() {
    find alfp common firmware/app firmware/sensing firmware_smp \
        -path firmware_smp/buildglue -prune -o \
        \( -name '*.c' -o -name '*.h' \) -print 2>/dev/null
    find firmware_smp/buildglue -name '*.c' -o -name '*.h' 2>/dev/null
}

echo "=== clang-format (dry run) ==="
if command -v clang-format >/dev/null 2>&1; then
    BAD=0
    while IFS= read -r f; do
        if ! clang-format --dry-run --Werror "$f" >/dev/null 2>/tmp/check_style_cf.$$; then
            echo "FORMAT VIOLATION: $f"
            cat /tmp/check_style_cf.$$
            BAD=1
        fi
        rm -f /tmp/check_style_cf.$$
    done < <(IN_SCOPE_FILES)
    if [ "$BAD" -ne 0 ]; then
        FAIL=1
    else
        echo "OK: no format violations in scope"
    fi
else
    echo "SKIPPED: clang-format not installed (see docs/CODING_STANDARD.md for the install command)"
fi

echo "=== cppcheck ==="
if command -v cppcheck >/dev/null 2>&1; then
    if ! cppcheck --enable=warning,style,performance,portability --std=c11 --language=c --error-exitcode=1 \
        --suppressions-list=tools/cppcheck_suppressions.txt \
        -I alfp/include -I common -I mesh -I firmware/app -I firmware/sensing \
        alfp/src common/*.c firmware/app/*.c firmware/sensing/*.c 2>&1; then
        FAIL=1
    fi
else
    echo "SKIPPED: cppcheck not installed (see docs/CODING_STANDARD.md for the install command)"
fi

echo "=== clang-tidy ==="
if command -v clang-tidy >/dev/null 2>&1; then
    BAD=0
    for f in common/loopback_radio.c common/uplink.c; do
        if ! clang-tidy "$f" -- -std=c11 -I alfp/include -I mesh -I common; then
            BAD=1
        fi
    done
    if [ "$BAD" -ne 0 ]; then
        FAIL=1
    fi
else
    echo "SKIPPED: clang-tidy not installed (see docs/CODING_STANDARD.md for the install command)"
fi

echo "=== warnings-as-errors compile check (host-compilable in-scope files only) ==="
if command -v gcc >/dev/null 2>&1; then
    WARN="-std=c11 -Wall -Wextra -Wconversion -Wshadow -Wundef -Wstrict-prototypes -Werror"
    BAD=0
    check() {
        if ! gcc $WARN -c -o /dev/null "$2" -I "$1" 2>&1; then
            echo "WARNING/ERROR: $2"
            BAD=1
        fi
    }
    gcc $WARN -c -o /dev/null -I mesh -I common common/loopback_radio.c 2>&1 || BAD=1
    gcc $WARN -c -o /dev/null -I alfp/include -I mesh -I common common/uplink.c 2>&1 || BAD=1
    for f in audio_features sound_features sound_fft sound_classify sound_image_pick sound_tables; do
        gcc $WARN -c -o /dev/null -I firmware/sensing -I common -I third_party/sensors \
            "firmware/sensing/$f.c" 2>&1 || BAD=1
    done
    if [ "$BAD" -ne 0 ]; then
        FAIL=1
    else
        echo "OK: host-compilable in-scope files are warning-free"
    fi
    echo "NOTE: firmware/sensing/sensing.c and every firmware/app/*.c and firmware_smp/**/*.c file"
    echo "      need the real Pico SDK / FreeRTOS / uT-Kernel headers and are not checked here."
else
    echo "SKIPPED: gcc not installed"
fi

if [ "$FAIL" -ne 0 ]; then
    echo "check_style.sh: FAILED"
    exit 1
fi
echo "check_style.sh: PASSED (see SKIPPED lines above for anything not actually checked)"
exit 0
