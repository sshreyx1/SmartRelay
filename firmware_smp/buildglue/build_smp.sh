#!/usr/bin/env bash
# build_smp.sh - overlay SmartRelay's SMP app onto the vendored kernel and
# build it with its own Make (docs/curveball_findings.md Step 2 item 8: this
# port has no CMake integration, so this script is the "APP_USE_TWO_CORES
# build option" for this tree - set SMP=0 for the kernel's own single-core
# fallback, SMP=1 (default) for the pinned dual-core build).
#
# Requires (not available in the environment this PR was written in - see
# docs/curveball_findings.md "Toolchain note"): arm-none-eabi-gcc on PATH,
# PICO_SDK_PATH set (needed for the usb_cdc console profile's TinyUSB
# sources even though WIFI=none here).
#
# NOT YET WIRED IN (see README.md in this directory): third_party/lora_mesh,
# third_party/wifi_mesh, SD/FatFs storage. This build will compile but will
# not link until those are added to sources.mk the same way alfp/sensing/
# sensors are.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
KERNEL_DIR="$REPO_ROOT/third_party/mtk3smp_rp2040"
APP_PROGRAM_DIR="$KERNEL_DIR/app_program"
BUILD_MAKE_DIR="$KERNEL_DIR/build_make"

SMP="${SMP:-1}"
CONSOLE="${CONSOLE:-usb_cdc}"
WIFI="${WIFI:-none}"
NODE_ID="${NODE_ID:-2}"

if [ ! -d "$KERNEL_DIR/kernel" ]; then
    echo "error: $KERNEL_DIR looks empty - did you run 'git submodule update --init' first?" >&2
    exit 1
fi

echo "Overlaying SmartRelay app files into $APP_PROGRAM_DIR (never committed into the submodule)"
rm -f "$APP_PROGRAM_DIR/app_main.c" "$APP_PROGRAM_DIR/demo_tasks.c" "$APP_PROGRAM_DIR/demo_tasks.h"
cp "$SCRIPT_DIR/../app/main_smp.c" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/../app/log.c" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/../app/log.h" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/../app/usermain_smp.c" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/../rtos_port_mtk3.c" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/../rtos_port.h" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/sources.mk" "$BUILD_MAKE_DIR/sources.mk"

echo "make SMP=$SMP CONSOLE=$CONSOLE WIFI=$WIFI NODE_ID=$NODE_ID"
(cd "$BUILD_MAKE_DIR" && SMARTRELAY_ROOT="$REPO_ROOT" make SMP="$SMP" CONSOLE="$CONSOLE" WIFI="$WIFI" NODE_ID="$NODE_ID")
