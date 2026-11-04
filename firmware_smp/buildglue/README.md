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

## What is NOT wired in yet (update: the bigger issue found in round 2 is resolved)

**Round 2 found something more fundamental than "these files aren't added yet": this port "does
not link the pico-sdk runtime" - it only compiled the *headers* of a few Pico SDK modules, never
the actual driver `.c` files `gpio_init`/`adc_read`/`spi_init`/`uart_init`/etc. need. Task C3
(CURVEBALL round 3) resolved this for real, on a real toolchain**: `sources.mk` now compiles and
links the actual Pico SDK driver sources (`hardware_gpio/gpio.c`, `hardware_spi/spi.c`,
`hardware_adc/adc.c`, `hardware_dma/dma.c`, `hardware_uart/uart.c`, `hardware_i2c/i2c.c`,
`hardware_clocks/clocks.c`, `hardware_pll/pll.c`, `hardware_xosc/xosc.c`, `hardware_timer/timer.c`,
`hardware_irq/irq.c`, `hardware_sync/sync.c`, `pico_time/time.c` + `timeout_helper.c`), plus the
kernel, `firmware_smp/app`, `alfp/`, `firmware/sensing`, and the raw sensor drivers
(`third_party/sensors`) as before. Getting there needed several more real fixes beyond "add the
.c files" - a `size_t` redefinition conflict between `tk/syslib.h` and `<string.h>`, a `$(eval
$(call ...))` bug that silently never ran any of this file's own compile rules at all (GCC/CFLAGS
were empty at eval time - nothing in `APP_SRCS` had ever actually compiled before, in any
previous round), `-lm` and `--specs=nosys.specs` link flags (patched into the makefile the same
working-tree-only way `CNF_TIMER_PERIOD` already was, since no Make variable survives to the link
recipe otherwise), `-DPICO_DISABLE_SHARED_IRQ_HANDLERS=1` (this kernel has its own vector table,
not Pico SDK's relocatable one), and two tiny stub symbols
(`firmware_smp/buildglue/pico_sdk_glue_stubs.c`) for things Pico SDK's own `crt0.S`/linker script
would normally provide. Full details and the exact command/output sequence:
`docs/curveball_results.md`'s C3 section.

**Still not compiled in, confirmed by the exact remaining linker errors** (not the vaguer "needs
the pico-sdk runtime" finding this section used to report): `third_party/lora_mesh`,
`third_party/wifi_mesh`, and `firmware/app/{radios.c,storage_fatfs.c,hw_config.c,board.c}` plus the
external `no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` dependency storage_fatfs.c needs. Every other symbol
`radios.c`/`storage_fatfs.c` would need from the Pico SDK side is already linked in above; what's
missing now is only the SmartRelay/mesh-radio/FatFs code itself, each of which is a substantially
larger piece of work than the driver-linking gap was (lora_mesh.c/wifi_mesh.c each have their own
SPI/GPIO wiring to re-verify under this kernel, and storage_fatfs.c needs an actual FatFs port
added to this build the same way the driver sources above were).

Whether the clocks this now-linked driver code assumes (UART baud, SPI clock, ADC clock) are
actually *correct* on real hardware - not just present - is a separate, still-open question (the
kernel does its own from-scratch PLL/clock setup, `kernel/sysdepend/pico_rp2040/cpu_clock.c`, which
was never confirmed to agree with what `hardware_clocks/clocks.c` thinks `clock_get_hz()` should
return). This needs a real board measurement (oscilloscope/logic analyzer or a known-baud UART
transaction), not another toolchain run. See docs/curveball_results.md's C3 section.

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
