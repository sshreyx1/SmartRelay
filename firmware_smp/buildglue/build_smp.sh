#!/usr/bin/env bash
# build_smp.sh - overlay SmartRelay's node firmware onto the vendored uT-Kernel 3 SMP kernel and
# build it with the kernel's own Make (docs/MICROKERNEL.md: this port has no
# CMake integration). SMP=1 (default) is the pinned dual-core build, SMP=0 the kernel's own
# single-core fallback.
#
#   NODE_IDS="1 2 3 4 5" firmware_smp/buildglue/build_smp.sh    # all five nodes (default)
#   NODE_IDS=2 firmware_smp/buildglue/build_smp.sh              # one node
#
# Output: uf2/csc09_smp_node_<N>.uf2 (SMP=0: csc09_smp0_node_<N>.uf2) (next to the FreeRTOS build's csc09_node_<N>.uf2, never
# overwriting them).
#
# Requires: arm-none-eabi-gcc/ar on PATH, make, a host C++ compiler (the kernel builds its own
# elf2uf2 the first time), PICO_SDK_PATH (the SDK version firmware/ builds with, 2.1.1) and
# FATFS_PICO_PATH (tools/fetch_deps.sh) -- the same two dependencies firmware/CMakeLists.txt needs.
#
# Everything this script changes inside third_party/mtk3smp_rp2040 is working-tree only (never
# committed there, see third_party/MTK3SMP_PROVENANCE.md); each run first restores the files it
# patches, so it can be re-run any number of times.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
KERNEL_DIR="$REPO_ROOT/third_party/mtk3smp_rp2040"
APP_PROGRAM_DIR="$KERNEL_DIR/app_program"
BUILD_MAKE_DIR="$KERNEL_DIR/build_make"
USB_DIR="$KERNEL_DIR/lib/libtm/sysdepend/pico_rp2040/usb"
OUT_DIR="$REPO_ROOT/uf2"

SMP="${SMP:-1}"
CONSOLE="${CONSOLE:-usb_cdc}"
WIFI="${WIFI:-none}"
NODE_IDS="${NODE_IDS:-${NODE_ID:-1 2 3 4 5}}"

if [ ! -d "$KERNEL_DIR/kernel" ]; then
    echo "error: $KERNEL_DIR looks empty - did you run 'git submodule update --init' first?" >&2
    exit 1
fi
: "${PICO_SDK_PATH:?set PICO_SDK_PATH to the Pico SDK (2.1.1, as firmware/ uses)}"
: "${FATFS_PICO_PATH:?set FATFS_PICO_PATH to carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico (tools/fetch_deps.sh)}"
export PICO_SDK_PATH FATFS_PICO_PATH

# Start from the submodule's own versions of every file patched below.
git -C "$KERNEL_DIR" checkout -- config/config.h build_make/makefile \
    lib/libtm/sysdepend/pico_rp2040/usb/usb_sdk_compat.c \
    lib/libtm/sysdepend/pico_rp2040/usb/usb_tinyusb_glue.c

echo "Overlaying SmartRelay app files into $APP_PROGRAM_DIR (never committed into the submodule)"
# The port's demo and any other overlay (firmware_smp/smoke) define usermain() too.
rm -f "$APP_PROGRAM_DIR/app_main.c" "$APP_PROGRAM_DIR/demo_tasks.c" "$APP_PROGRAM_DIR/demo_tasks.h" \
      "$APP_PROGRAM_DIR/smp_smoke.c" "$APP_PROGRAM_DIR/usermain_smoke.c"
cp "$SCRIPT_DIR/../app/main_smp.c" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/../app/log.c" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/../app/log.h" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/../app/usermain_smp.c" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/../rtos_port_mtk3.c" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/../rtos_port.h" "$APP_PROGRAM_DIR/"
cp "$SCRIPT_DIR/sources.mk" "$BUILD_MAKE_DIR/sources.mk"

# Round 2 review fix A1: 1 ms kernel tick (brief section 3.5); config.h defaults to 10 ms.
CONFIG_H="$KERNEL_DIR/config/config.h"
if ! grep -Eq '^#define CNF_TIMER_PERIOD[[:space:]]+10([^0-9]|$)' "$CONFIG_H"; then
    echo "error: $CONFIG_H's CNF_TIMER_PERIOD line did not match the expected '10' - the" >&2
    echo "       submodule may have moved; update this script's sed pattern by hand." >&2
    exit 1
fi
# perl -pi, not sed -i: BSD sed (macOS) and GNU sed disagree on -i and on \s / \+.
perl -pi -e 's/^#define CNF_TIMER_PERIOD\s+10\b/#define CNF_TIMER_PERIOD\t1/' "$CONFIG_H"

# Link recipe (makefile:72-75): add sources.mk's SmartRelay archive + section script after the
# kernel objects, libm, and newlib's no-OS syscall stubs; and make the image depend on the
# archive. No Make variable sources.mk could append to reaches that recipe otherwise (see the
# comment above SMARTRELAY_LINK_EXTRA in sources.mk).
MAKEFILE="$BUILD_MAKE_DIR/makefile"
if ! grep -q '^\$(EXE_FILE).elf: \$(OBJS) \$(E2U)$' "$MAKEFILE" || ! grep -q '\$(OBJS)$' "$MAKEFILE"; then
    echo "error: $MAKEFILE's link rule is not the one this script was written against - the" >&2
    echo "       submodule may have moved; update this script by hand rather than skipping it." >&2
    exit 1
fi
perl -pi -e 's/^\$\(EXE_FILE\)\.elf: \$\(OBJS\) \$\(E2U\)$/\$(EXE_FILE).elf: \$(OBJS) \$(E2U) \$(SMARTRELAY_LINK_DEPS)/' "$MAKEFILE"
perl -pi -e 's/\$\(OBJS\)$/\$(OBJS) \$(SMARTRELAY_LINK_EXTRA) -lm --specs=nosys.specs/' "$MAKEFILE"

# Round 2 review fix A2: USB-CDC console input (see the overlay's header comment).
cp "$SCRIPT_DIR/usb_tinyusb_glue_overlay.c" "$USB_DIR/usb_tinyusb_glue.c"
# Generic Pico SDK IRQ API on the kernel's vector table (see the overlay's header comment).
cp "$SCRIPT_DIR/usb_sdk_compat_overlay.c" "$USB_DIR/usb_sdk_compat.c"

# The kernel's subdir.mk rules write each object next to a mirror of its source path but never
# create those directories (a fresh clone only has the ones holding a tracked subdir.mk), so make
# them all first. Empty directories are invisible to git: the submodule stays clean.
(cd "$KERNEL_DIR" && find lib kernel device app_program -type d) | while IFS= read -r d; do
    mkdir -p "$BUILD_MAKE_DIR/mtkernel_3/$d"
done

mkdir -p "$OUT_DIR"
EXE="mtk3pico_smp${SMP}_${CONSOLE}"
for N in $NODE_IDS; do
    echo "=== node $N: make SMP=$SMP CONSOLE=$CONSOLE WIFI=$WIFI NODE_ID=$N"
    # NODE_ID is a compile definition of every app object and make does not track flags:
    # rebuild all app objects for each node (the kernel's own objects are reused).
    # (app_program/subdir.mk next to those objects is a tracked submodule file: objects only.)
    rm -rf "$BUILD_MAKE_DIR/mtkernel_3/app_libs"
    rm -f "$BUILD_MAKE_DIR"/mtkernel_3/app_program/*.o "$BUILD_MAKE_DIR"/mtkernel_3/app_program/*.d
    rm -f "$BUILD_MAKE_DIR/$EXE".*
    # The makefile's last step runs the kernel's own host elf2uf2.exe, which Windows Smart App
    # Control sometimes refuses to start; so a make failure is only accepted if the ELF was
    # produced (it is deleted above, so a stale one can never pass), and the UF2 is always made
    # by elf2uf2.py from that ELF.
    if ! (cd "$BUILD_MAKE_DIR" && SMARTRELAY_ROOT="$REPO_ROOT" make SMP="$SMP" CONSOLE="$CONSOLE" WIFI="$WIFI" NODE_ID="$N"); then
        if [ ! -f "$BUILD_MAKE_DIR/$EXE.elf" ]; then
            echo "error: node $N did not build" >&2
            exit 1
        fi
        echo "(make failed after linking, most likely the elf2uf2.exe step; continuing with elf2uf2.py)"
    fi
    # Follow-up F4: the single-core build gets its own name so it never replaces a dual-core image.
    UF2_NAME="csc09_smp_node_$N.uf2"
    if [ "$SMP" = "0" ]; then
        UF2_NAME="csc09_smp0_node_$N.uf2"
    fi
    "${PYTHON:-python3}" "$SCRIPT_DIR/elf2uf2.py" "$BUILD_MAKE_DIR/$EXE.elf" "$OUT_DIR/$UF2_NAME"
    echo "=== node $N: $OUT_DIR/$UF2_NAME"
done
