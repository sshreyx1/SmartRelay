#!/bin/sh
# check_env.sh - does this machine have what build_smp.sh needs?
#
# Prints the versions of every tool/variable build_smp.sh (and the Pico SDK driver work in C3)
# depends on, and whether the mtk3smp_rp2040 submodule is actually checked out. Run it before
# attempting a build.
#
#   sh firmware_smp/buildglue/check_env.sh
#
# A line printed as "MISSING" means that stage of the build cannot be attempted at all. This
# script changes nothing on disk except (if missing) initializing the submodule, which is
# read-only from the repo's point of view (the submodule's own working tree, not this repo).
set -u

ok=1

check_version() {
    name="$1"
    shift
    if command -v "$name" >/dev/null 2>&1; then
        "$@" 2>&1 | head -1
    else
        echo "MISSING"
        ok=0
    fi
}

echo "=== arm-none-eabi-gcc ==="
check_version arm-none-eabi-gcc arm-none-eabi-gcc --version

echo "=== make ==="
check_version make make --version

echo "=== cmake ==="
check_version cmake cmake --version

echo "=== python3 ==="
check_version python3 python3 --version

echo "=== picotool ==="
check_version picotool picotool version

echo "=== PICO_SDK_PATH ==="
if [ -n "${PICO_SDK_PATH:-}" ]; then
    echo "$PICO_SDK_PATH"
    if [ ! -f "$PICO_SDK_PATH/pico_sdk_init.cmake" ]; then
        echo "  WARNING: pico_sdk_init.cmake not found under PICO_SDK_PATH -- is this really the SDK checkout?"
        ok=0
    fi
else
    echo "MISSING (not set)"
    ok=0
fi

echo "=== third_party/mtk3smp_rp2040 submodule ==="
cd "$(dirname "$0")/../.." || exit 1
if [ -f third_party/mtk3smp_rp2040/app_program/app_main.c ]; then
    echo "already checked out ($(git -C third_party/mtk3smp_rp2040 rev-parse --short HEAD 2>/dev/null || echo 'commit unknown'))"
else
    echo "not checked out, attempting: git submodule update --init third_party/mtk3smp_rp2040"
    if git submodule update --init third_party/mtk3smp_rp2040 2>&1; then
        if [ -f third_party/mtk3smp_rp2040/app_program/app_main.c ]; then
            echo "  OK: checked out after init"
        else
            echo "  FAILED: still missing app_program/app_main.c after init"
            ok=0
        fi
    else
        echo "  FAILED: git submodule update --init did not succeed"
        ok=0
    fi
fi

echo "=== summary ==="
if [ "$ok" -eq 1 ]; then
    echo "PASS: everything needed for a build attempt is present. Try build_smp.sh next (C1)."
else
    echo "FAIL: at least one prerequisite is missing. Install/set the items marked MISSING above,"
    echo "      then re-run this script before attempting build_smp.sh."
fi
