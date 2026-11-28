#!/usr/bin/env bash
# build_smoke.sh - CURVEBALL round 3, Task C2: build the smp_smoke app (no radios, no SD, no
# ALFP) the same way firmware_smp/buildglue/build_smp.sh builds the real node firmware, so C2
# proves dual-core SMP works before anything else SmartRelay-specific is in the picture.
#
# Requires (not available in the environment this script was written in -- see
# docs/MICROKERNEL.md and firmware_smp/buildglue/check_env.sh): arm-none-eabi-gcc on
# PATH, PICO_SDK_PATH set (CONSOLE=usb_cdc needs TinyUSB sources from it).
#
#   SMP=1 NODE_ID=2 firmware_smp/smoke/build_smoke.sh
#
# Flash the resulting .uf2 the same way as any other build_make output (hold BOOTSEL, drag on).
# Then watch with tools/serial_log.py (or any terminal) and run tools/smp_smoke_check.py against
# the captured log to get a one-page PASS/FAIL summary.
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

echo "Overlaying the smp_smoke app into $APP_PROGRAM_DIR (never committed into the submodule)"
rm -f "$APP_PROGRAM_DIR/app_main.c" "$APP_PROGRAM_DIR/demo_tasks.c" "$APP_PROGRAM_DIR/demo_tasks.h"
cp "$SCRIPT_DIR/smp_smoke.c" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/usermain_smoke.c" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/../app/log.c" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/../app/log.h" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/../rtos_port_mtk3.c" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/../rtos_port.h" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/sources.mk" "$BUILD_MAKE_DIR/sources.mk"

# Same 1 ms tick fix as build_smp.sh (round 2 review fix A1) -- the smoke app's check 3 (tick
# accuracy) is specifically what would catch this being wrong if it were skipped.
CONFIG_H="$KERNEL_DIR/config/config.h"
if ! grep -Eq '^#define CNF_TIMER_PERIOD[[:space:]]+10([^0-9]|$)' "$CONFIG_H"; then
    echo "error: $CONFIG_H's CNF_TIMER_PERIOD line did not match the expected '10' - the" >&2
    echo "       submodule may have moved since this was verified; check docs/MICROKERNEL.md" >&2
    echo "       and update this script's sed pattern by hand rather than skipping it." >&2
    exit 1
fi
perl -pi -e 's/^#define CNF_TIMER_PERIOD\s+10\b/#define CNF_TIMER_PERIOD\t1/' "$CONFIG_H"   # portable: BSD/GNU sed -i differ

# Same USB CDC console-input fix as build_smp.sh (round 2 review fix A2) -- check 9 needs it.
cp "$SCRIPT_DIR/../buildglue/usb_tinyusb_glue_overlay.c" \
   "$KERNEL_DIR/lib/libtm/sysdepend/pico_rp2040/usb/usb_tinyusb_glue.c"

echo "make SMP=$SMP CONSOLE=$CONSOLE WIFI=$WIFI NODE_ID=$NODE_ID"
(cd "$BUILD_MAKE_DIR" && SMARTRELAY_ROOT="$REPO_ROOT" make SMP="$SMP" CONSOLE="$CONSOLE" WIFI="$WIFI" NODE_ID="$NODE_ID")
