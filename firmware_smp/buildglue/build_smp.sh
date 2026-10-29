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

# Round 2 review fix A1: CNF_TIMER_PERIOD defaults to 10 ms
# (third_party/mtk3smp_rp2040/config/config.h:41) and nothing overrode it -
# the brief needs a 1 ms tick (section 3.5). Patched in the submodule's
# working tree only (never git-added/committed there, same rule as the
# app_program overlay above) so `git -C third_party/mtk3smp_rp2040 diff`
# shows this if anyone looks, but it never lands in our history.
CONFIG_H="$KERNEL_DIR/config/config.h"
if ! grep -q '^#define CNF_TIMER_PERIOD\s\+10\b' "$CONFIG_H"; then
    echo "error: $CONFIG_H's CNF_TIMER_PERIOD line did not match the expected '10' - the" >&2
    echo "       submodule may have moved since this was verified; check docs/curveball_findings.md" >&2
    echo "       Step 2 and update this script's sed pattern by hand rather than skipping it." >&2
    exit 1
fi
sed -i 's/^#define CNF_TIMER_PERIOD\s\+10\b/#define CNF_TIMER_PERIOD\t1/' "$CONFIG_H"

# Round 2 review fix A2: tm_getchar(0) always returns 0 on CONSOLE=usb_cdc
# (tm_com.c's tm_rcv_dat has no USB-CDC RX path at all, only UART) - this
# overlay adds one by redirecting the existing discard-everything RX drain
# into a small ring main_smp.c's usb_owner_task can read via
# rtos_console_getchar(). See the overlay file's own header comment.
cp "$SCRIPT_DIR/usb_tinyusb_glue_overlay.c" "$KERNEL_DIR/lib/libtm/sysdepend/pico_rp2040/usb/usb_tinyusb_glue.c"

echo "make SMP=$SMP CONSOLE=$CONSOLE WIFI=$WIFI NODE_ID=$NODE_ID"
(cd "$BUILD_MAKE_DIR" && SMARTRELAY_ROOT="$REPO_ROOT" make SMP="$SMP" CONSOLE="$CONSOLE" WIFI="$WIFI" NODE_ID="$NODE_ID")
