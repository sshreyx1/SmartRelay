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
# Prerequisites (none are available in the environment this PR was written in -
# nothing here has been compiled, see docs/curveball_findings.md):
export PATH="/path/to/arm-gnu-toolchain/bin:$PATH"   # arm-none-eabi-gcc etc.
export PICO_SDK_PATH=/path/to/pico-sdk               # needed for CONSOLE=usb_cdc's TinyUSB sources

git submodule update --init third_party/mtk3smp_rp2040

# Dual core, pinned (comms+console on core 0, sensors on core 1):
SMP=1 NODE_ID=2 firmware_smp/buildglue/build_smp.sh

# Single-core fallback (brief 3.1):
SMP=0 NODE_ID=2 firmware_smp/buildglue/build_smp.sh
```

The script overlays `firmware_smp/app/*` and `firmware_smp/rtos_port*` into
`third_party/mtk3smp_rp2040/app_program/` and `sources.mk` into its `build_make/`
(replacing the port's own demo, which defines the same `usermain()` symbol) -
nothing is committed into the submodule itself (`../../MTK3SMP_PROVENANCE.md`).

## What is NOT wired in yet

`sources.mk` compiles the kernel, `firmware_smp/app`, `alfp/`, `firmware/sensing`,
and the raw sensor drivers (`third_party/sensors`). It does **not** yet compile:

- `third_party/lora_mesh` (the full LR1121 driver + GPIO/SPI glue) - `radios.c`'s
  `radio_lora()` will fail to link without it.
- `third_party/wifi_mesh` (`wifi_mesh.c`, `wst_link.c`) - `radios.c`'s
  `radio_wifi()` will fail to link without it.
- `firmware/app/{radios.c,storage_fatfs.c,hw_config.c,board.c}` themselves, and the
  external `no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` dependency (`tools/fetch_deps.sh`).

Each is a mechanical addition to `sources.mk` in the same wildcard-per-directory
style already used for `alfp`/`sensing`/`sensors` - the reason this PR stops here
is the sheer file count (the LR1121 driver tree alone is ~40 files) combined with
having no toolchain in this environment to verify any of it compiles, which makes
hand-wiring that many paths blind higher-risk than valuable. This is the next step
for whoever picks this up with real hardware and a working toolchain.

## Running the node images

The old build makes one `.uf2` per `NODE_ID` (1-5) via CMake's `foreach`. This
build makes one node at a time (`NODE_ID=<n> firmware_smp/buildglue/build_smp.sh`),
since the underlying Make project is not parameterised over multiple images the
way the CMake `foreach(N ${NODE_IDS})` loop is - run it once per node to flash.
