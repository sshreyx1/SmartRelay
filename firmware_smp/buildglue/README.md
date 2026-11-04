# Building the CURVEBALL (uT-Kernel 3 SMP) firmware

This is a **separate build system** from `firmware/` (CMake + FreeRTOS, untouched
by this PR), because `third_party/mtk3smp_rp2040` is a standalone GNU Make project
with no CMake integration - see `docs/curveball_findings.md` Step 2 item 8 for why.
The old build remains the default, working, single-core firmware; this is the new,
additive SMP path.

## What this maps to from the brief

The brief (CURVEBALL section 3.5/Step 5) asks for one `APP_USE_TWO_CORES` CMake
option toggling single-core vs. pinned-dual-core in the same build. That exact
mechanism does not exist here (two different build systems). What does exist, and
does the same job with no app-code fork: the kernel's own `SMP=0`/`SMP=1` Make flag
(`third_party/mtk3smp_rp2040/build_make/pico_rp2040.mk:1-25`) already "collapses"
the SMP build to the single-core equivalent at build time, and the app code
(`firmware_smp/app/main_smp.c`, `firmware_smp/rtos_port_mtk3.c`) uses the same
`#if TK_SUPPORT_SMP` pattern the kernel's own demo uses, so one source tree builds
either way.

```sh
# Prerequisites (none were available in the environment this was originally written in;
# installed and actually used for Task C3, round 3 - see docs/curveball_results.md):
export PATH="/path/to/arm-gnu-toolchain/bin:$PATH"   # arm-none-eabi-gcc etc.
export PICO_SDK_PATH=/path/to/pico-sdk               # Pico SDK 2.1.1, as firmware/ uses
export FATFS_PICO_PATH=/path/to/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico   # tools/fetch_deps.sh

git submodule update --init third_party/mtk3smp_rp2040

# Dual core, pinned (comms+console on core 0, sensors on core 1):
SMP=1 firmware_smp/buildglue/build_smp.sh          # all five nodes

# Single-core fallback (brief 3.1):
SMP=0 NODE_IDS=2 firmware_smp/buildglue/build_smp.sh
```

The script overlays `firmware_smp/app/*` and `firmware_smp/rtos_port*` into
`third_party/mtk3smp_rp2040/app_program/` and `sources.mk` into its `build_make/`
(replacing the port's own demo, which defines the same `usermain()` symbol) -
nothing is committed into the submodule itself (`../../MTK3SMP_PROVENANCE.md`).

## Status (2026-11-04): full node firmware builds and runs

Everything that used to be listed here as "not wired in" (LoRa mesh, WiFi mesh, radios.c,
storage_fatfs.c, FatFs/SD) is now compiled, linked and was run on real boards, together with a
Pico SDK runtime layer for this kernel (`pico_runtime_mtk3.c`, `usb_sdk_compat_overlay.c`).
The earlier text in this section (and docs/curveball_results.md's C3 claim that the driver layer
linked cleanly) is superseded: it missed duplicate IRQ symbols, and the kernel never started the
TIMER or populated the SDK clock table. **See `docs/SMP_HANDOFF.md`** for what was built, what
was measured on hardware, the one open problem, and how to build/flash/test.

## Running the node images

`build_smp.sh` builds all five nodes by default (`NODE_IDS="1 2 3 4 5"`), writing
`uf2/csc09_smp_node_<N>.uf2` next to the FreeRTOS `csc09_node_<N>.uf2`. Flashing and console
helpers: `flash_node.py`, `serial_tstamp.py` (usage in docs/SMP_HANDOFF.md section 4).
