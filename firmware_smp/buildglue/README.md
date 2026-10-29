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

## What is NOT wired in yet, and a bigger issue found in round 2

`sources.mk` compiles the kernel, `firmware_smp/app`, `alfp/`, `firmware/sensing`,
and the raw sensor drivers (`third_party/sensors`). `third_party/lora_mesh`,
`third_party/wifi_mesh`, `firmware/app/{radios.c,storage_fatfs.c,hw_config.c,
board.c}`, and the external `no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` dependency are
still not compiled in.

**Round 2 found something more fundamental than "these files aren't added yet",
and this PR deliberately stops short of wiring any of them in until it's
resolved.** This port "does not link the pico-sdk runtime" - it only compiles
the *headers* of a few Pico SDK modules plus a hand-picked handful of real
`.c` sources for its own USB-CDC console and (if `WIFI=cyw43`) its own
CYW43439 driver (`docs/GETTING_STARTED.md:30-42`). Every SmartRelay source
file wired in so far - `third_party/sensors/*.c`, `firmware/sensing/*.c` -
calls real Pico SDK runtime functions: `gpio_init`, `adc_init`/`adc_read`,
`sleep_ms`, `get_absolute_time`/`to_ms_since_boot`, `get_rand_32`, and (once
`radios.c`/`board.c`/`wifi_mesh.c`/`wst_link.c`/`lora_mesh`'s `gpio.c`/`spi.c`
are added) `spi_init`, `uart_init`, `dma_channel_claim_unused`, and more.
**None of these have a compiled implementation anywhere in this Make build.**
Adding more `-I` include paths (headers) does not fix this - it needs the
actual Pico SDK driver `.c` files (`hardware_gpio/gpio.c`,
`hardware_spi/spi.c`, `hardware_adc/adc.c`, `hardware_dma/dma.c`,
`hardware_uart/uart.c`, `pico_rand`, `pico_time`) compiled in too, the same
way `pico_rp2040.mk`'s `WIFI=cyw43` block already does for the three it
needs (`hardware_pio`/`hardware_dma`/`hardware_gpio`, pico_rp2040.mk:292-388).

That alone would very likely satisfy the linker. It is **not** necessarily
safe at runtime: this kernel does its own from-scratch clock/PLL setup with
direct register writes, independent of the Pico SDK's clock-tracking state
(`third_party/mtk3smp_rp2040/kernel/sysdepend/pico_rp2040/cpu_clock.c`'s
`init_pll()`). Pico SDK driver functions like `spi_init()`/`uart_init()`
call `clock_get_hz(clk_peri)` internally to compute baud-rate dividers from
the SDK's *own* clock bookkeeping, which this kernel's PLL setup never
populates - so even a build that links clean could configure SPI/UART at
the wrong baud rate silently, rather than failing loudly. This is a real
hardware-behaviour risk, not a style question, and not something to guess
past without a toolchain and a real board to check it against.

**What to run, and what to send back**, to make real (not guessed) progress
on this next:
```sh
export PATH="/path/to/arm-gnu-toolchain/bin:$PATH"
export PICO_SDK_PATH=/path/to/pico-sdk
git submodule update --init third_party/mtk3smp_rp2040
SMP=1 NODE_ID=2 firmware_smp/buildglue/build_smp.sh
```
Send back the **full** output, especially the linker's `undefined reference
to ...` list if it gets that far (it tells us exactly which Pico-SDK
functions are actually missing, rather than us guessing the full set) - and
if it links and flashes, send back a scope trace or logic analyzer capture
of the SPI or UART clock line next to what `board.h`/`SPI1_BAUD_HZ`/
`WST_BAUD` expect, since that is the only way to catch the clock-tracking
problem above before it causes a flaky LoRa/SD/WiFi-bridge link in the
field. Each missing `gpio_*`/`spi_*`/`adc_*`/`dma_*`/`uart_*` symbol can then
be resolved one at a time, citing the real Pico SDK source file for each,
rather than this PR adding ~40 more files on a foundation that was not
actually confirmed to work.

## Running the node images

The old build makes one `.uf2` per `NODE_ID` (1-5) via CMake's `foreach`. This
build makes one node at a time (`NODE_ID=<n> firmware_smp/buildglue/build_smp.sh`),
since the underlying Make project is not parameterised over multiple images the
way the CMake `foreach(N ${NODE_IDS})` loop is - run it once per node to flash.
