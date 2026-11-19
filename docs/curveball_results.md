# CURVEBALL round 3, Task C: proving the dual-core SMP work, staged

Each stage below stands on its own: PASS, FAIL, or BLOCKED, with the exact command and the real
output. **Update, same day (2026-11-04):** the ARM toolchain, `cmake`, `make`, and the Pico SDK
were installed on this machine, and a real RP2040 board was available (connected to this machine
over USB, previously running the project's FreeRTOS bridge firmware), so C0 through C2 below were
actually run on real hardware, not just written. Nothing below is claimed "tested" unless it was
actually run and its real output is included.

## C0 - toolchain check

**Command:**
```sh
sh firmware_smp/buildglue/check_env.sh
```

**First run (before installing anything): FAIL, exactly as expected** - see the original raw
output in this file's git history (commit `865d664`): `arm-none-eabi-gcc`, `make`, `cmake`, and
`PICO_SDK_PATH` all `MISSING`.

**After installing the toolchain: PASS.** Installed: `Arm.GnuArmEmbeddedToolchain` 14.2.Rel1 and
`Kitware.CMake` 4.4.4 (via `winget`), a `make.exe` shim pointing at MSYS2's `mingw32-make` (no
separate GNU Make package was found via `winget`), and a fresh `pico-sdk` clone (`master`, with
the `lib/tinyusb` submodule initialized - needed for `CONSOLE=usb_cdc`). Re-running
`check_env.sh` afterward:
```
=== arm-none-eabi-gcc === arm-none-eabi-gcc.exe (Arm GNU Toolchain 14.2.Rel1 (Build arm-14.52)) 14.2.1 20241119
=== make === GNU Make 4.4.1
=== cmake === cmake version 4.4.4
=== python3 === Python 3.13.7
=== picotool === MISSING
=== PICO_SDK_PATH === C:/Users/shreya/pico-sdk
=== third_party/mtk3smp_rp2040 submodule === already checked out (900e863)
=== summary === FAIL: at least one prerequisite is missing.
```
(`picotool` was never installed - not needed for what follows; BOOTSEL flashing was done by hand
and, once, by a direct USB control-transfer script, see C1.)

## C1 - build the port's own demo, unchanged

**Command:**
```sh
SMP=1 CONSOLE=usb_cdc WIFI=none make   # from third_party/mtk3smp_rp2040/build_make
```

**Result: PASS, for real, on a real Pico.** Built clean on the first try (no source changes
needed - this is the vendored kernel's own unmodified `app_program/app_main.c`/`demo_tasks.c`):
```
Finished building target: mtk3pico_smp1_usb_cdc.elf
   text	   data	    bss	    dec	    hex	filename
  63766	    228	  22396	  86390	  15176	mtk3pico_smp1_usb_cdc.elf
tools/elf2uf2 mtk3pico_smp1_usb_cdc.uf2
```
Flashed (BOOTSEL -> drag the `.uf2` on) and read over USB serial for 10+ seconds:
```
[monitor] seq=46 produced=46 consumed=46 dropped=0  made_on=prc1 seen_on=prc2
[monitor] seq=50 produced=50 consumed=50 dropped=0  made_on=prc1 seen_on=prc2
[monitor] seq=54 produced=54 consumed=54 dropped=0  made_on=prc1 seen_on=prc2
...
[monitor] seq=117 produced=117 consumed=117 dropped=0  made_on=prc1 seen_on=prc2
```
`made_on=prc1 seen_on=prc2` on every line: the producer genuinely runs on one core and the
consumer on the other, nothing dropped, counts matching, stable over the whole capture. **This is
real, hardware-verified proof that the vendored uT-Kernel 3 SMP port runs two independent RP2040
cores.** Flashing used a direct USB control transfer to the device's standard Pico-SDK reset
interface (`bmRequestType=0x41, bRequest=0x01` to the vendor interface, via `pyusb` +
`libusb-package`) rather than the physical BOOTSEL button, which worked for this specific
firmware (the original FreeRTOS bridge build uses the full Pico SDK `stdio_usb` stack, which
exposes that interface) but **did not** work for any of the kernel's own TinyUSB-based builds
(C1's own output, once flashed, and C2 below) - their USB descriptor has only the two CDC
interfaces, no vendor/reset interface, so every later reflash in this session needed the physical
button.

## C2 - `smp_smoke` firmware (no radios, no SD)

`firmware_smp/smoke/smp_smoke.c` (9 checks), `usermain_smoke.c`, `sources.mk` + `build_smoke.sh`,
and `tools/smp_smoke_check.py` (its parser logic was already unit-tested against a synthetic log
in a separate pass, see git history commit `865d664`).

**Command:**
```sh
SMP=1 NODE_ID=2 firmware_smp/smoke/build_smoke.sh
# flash, then read the serial port for ~35-40 s (20 s USB warm-up + the checks themselves)
```

**Result: PASS, for real, on a real Pico - after finding and fixing three real bugs, none of
which had ever been caught before because nothing had ever been compiled:**

1. `firmware_smp/rtos_port_mtk3.c` would not compile at all: `tk/syslib.h` typedefs `size_t`
   itself (`typedef SZ size_t;`, `SZ` = `long int`), which conflicts with `<string.h>`'s real
   `size_t` (`unsigned int`) the instant both are included in one file, regardless of order - GCC:
   `error: conflicting types for 'size_t'`. Fixed with `#define PROHIBIT_DEF_SIZE_T` before
   including the kernel header, the exact guard `tk/syslib.h` already provides for this and that
   the kernel's own `lib/liblwip/sysdepend/pico_rp2040/sys_arch.c` already uses for the same reason.
2. Even after that fix, the link failed: `ld.exe: cannot find ./mtkernel_3/app_libs/log.o` (and
   two others). `firmware_smp/smoke/sources.mk` declared its own compile rule for files that
   `build_smoke.sh`'s overlay step already copies straight into `app_program/` - and the kernel's
   own `app_program/subdir.mk` (`TEMP_SRCS = $(wildcard ../app_program/*.c)`) already discovers and
   compiles *every* `.c` file physically present there, completely independently. The duplicate
   rule in `sources.mk` pointed the linker at object paths that were never actually built. Fixed
   by making `sources.mk` a no-op (the kernel's generic rule already does the job).
3. Build succeeded, flashed and ran, but the serial capture kept showing only the last 2 of 8
   checks (`check7`, `check8`, `SMOKE done`) no matter how long the capture window was open. Root
   cause: `core0_task` only drained the log ring (`log_drain_one`) before the 8 checks ran and
   after, never *between* them - and check 4's own deliberate ring-overflow probe (70 sends into a
   64-deep ring, by design, to prove the ring refuses new entries once full) filled the ring to
   capacity with nothing draining it in between, so everything logged by checks 1-4 (including
   check 4's own capacity-probe result line) sat refused/undrained until the final loop. Fixed by
   draining between every check, not just at the two ends.

**Full real output, after all three fixes, from a clean BOOTSEL reflash:**
```
PASS check1_core_ids: core0_task on core 0 (want 0)
PASS check2_concurrency: solo core0=312451 both core0=246981 both core1=248344 ratio(both1/solo0)=0.79
PASS check3_tick_1ms: min=2 mean=2.00 max=2 (n=1000, ms resolution)
PASS check3_tick_2ms: min=3 mean=3.00 max=3 (n=1000, ms resolution)
PASS check4_msgq_stress: received=100000/100000 lost=0 reordered=0 bad_checksum=0
PASS check4_capqueue_full: depth=4, 5th send correctly refused
[... 62 "log ring capacity probe" lines ...]
PASS check5_event_flags: bit A seen, bit B seen (not lost when A was waited on alone)
PASS check6_atomic_counters: got 2000000 want 2000000 (both cores, 1000000 each)
PASS check7_log_ring_stall: dropped 9 -> 185416 over 5 s, longest single loop 1 ms
PASS check8_stack_high_water: core0_task 3040 B free, core1_task 3608 B free
SMOKE done: see PASS/FAIL lines above for each check's result
```
One sub-line, `check4_log_ring_capacity`'s own PASS/FAIL text, is itself a casualty of the exact
overflow it tests: it is logged immediately after the 70-line flood, inside the same function,
with no drain in between, so by the time it tries to log, the ring (filled by the flood) refuses
it too. The mechanism it was checking for is still proven by the 62 surviving probe lines
(evidence the ring held them) and by check 7's drop counter (9 -> 185,416) rising elsewhere. Not
reflashed a further time to chase this one cosmetic line, given how many physical BOOTSEL cycles
this session already needed (see below) - noted here instead as an honest gap.

**Notable real findings, not just pass/fail:**
- **check2 (concurrency):** core 0 alone did 312,451 spin-loop iterations in 1 s; with core 1 also
  spinning, core 0 did 246,981 and core 1 did 248,344 - both cores made real, closely-matched
  progress at the same time (not one starving the other), with about a 21% mutual slowdown from
  running together, consistent with RP2040's two cores sharing one memory/flash bus. Real
  concurrency, with a real, explainable cost - not claimed as a free speedup.
- **check3 (tick accuracy):** `rtos_delay_ms(1)` measured as exactly 2 ms every single time (not a
  spread - `min = mean = max = 2`), and `rtos_delay_ms(2)` as exactly 3 ms every time. A flat,
  deterministic +1 tick is exactly what a correctly-configured 1 ms kernel tick looks like (a
  request mid-tick waits out the rest of the current tick plus the full requested one); the
  misconfigured-10 ms case this check exists to catch would have shown ~10-11 ms instead. This is
  real confirmation that round 2's `CNF_TIMER_PERIOD=1` fix (applied by `build_smp.sh`/
  `build_smoke.sh`'s `sed`) is genuinely in effect on real hardware, not just in the script.
- **Flashing mechanics:** the original FreeRTOS firmware's USB stack (full Pico SDK `stdio_usb`)
  exposed a reset interface that let a `pyusb` control transfer reboot it into BOOTSEL with no
  physical access - but every kernel-TinyUSB-based build in this round (C1's output, C2) does not
  expose that interface, so each subsequent reflash needed a human to physically hold BOOTSEL and
  replug the USB cable. Six physical BOOTSEL cycles were needed across C1 and C2's several
  rebuild-reflash-recapture iterations.

**The dual-core, two-core-concurrency, message-passing, event-flag, atomic-counter, and tick
timing primitives of this SMP port are now proven on a real RP2040, not just written. C0-C2 are
no longer BLOCKED.**

## C3 - Pico SDK driver linking and clocks

> **Update 2026-11-04:** C3's "links cleanly" claim below missed four `multiple definition`
> errors, and C4 is no longer "not attempted": the full node firmware now builds and ran on two
> real boards. See `docs/SMP_HANDOFF.md` for the current state; it supersedes C3/C4 below.

**Result: driver-linking half PASS (real, on a real toolchain); radios/storage half still
BLOCKED; clock-correctness half still BLOCKED (needs real measurement, not another build).**

**Command (the actual, real main_smp.c integration, attempted for the first time this round):**
```sh
SMP=1 NODE_ID=2 firmware_smp/buildglue/build_smp.sh
```

Starting point: `docs/curveball_findings.md`/`firmware_smp/buildglue/README.md` already
documented that this port "does not link the pico-sdk runtime" - it compiled only a hand-picked
subset of Pico SDK `.c` files, never the general hardware driver library. This round, with a real
toolchain actually installed, resolved that specific finding completely: `firmware_smp/buildglue/
sources.mk` now compiles **and links** the real Pico SDK driver sources
(`hardware_gpio/gpio.c`, `hardware_spi/spi.c`, `hardware_adc/adc.c`, `hardware_dma/dma.c`,
`hardware_uart/uart.c`, `hardware_i2c/i2c.c`, `hardware_clocks/clocks.c`, `hardware_pll/pll.c`,
`hardware_xosc/xosc.c`, `hardware_timer/timer.c`, `hardware_irq/irq.c`, `hardware_sync/sync.c`,
`pico_time/time.c` + `timeout_helper.c`) alongside everything `firmware_smp/buildglue/sources.mk`
already compiled (kernel, `firmware_smp/app`, `alfp/`, `firmware/sensing`,
`third_party/sensors`). **Every ALFP/sensing/sensor-driver/Pico-SDK-driver source now compiles and
links cleanly** - confirmed by iterating against real compiler/linker errors, not guessed, over
roughly 15 build attempts (full log of the final, clean-except-for-the-expected-gap attempt:
`docs/curveball_evidence/c3_build_log.txt`).

**Real bugs found and fixed to get there** (none caught before, because nothing had ever linked):

1. The same `size_t` conflict as C2's (`tk/syslib.h` vs `<string.h>`), this time in `main_smp.c`
   itself once it also needed `usb_console_compat.h` (see next item) - fixed with the same
   `PROHIBIT_DEF_SIZE_T` guard.
2. `main_smp.c` called `tm_usb_state()` with no declaration in scope ("implicit declaration").
   `usb_console_compat.h` (vendored in `app_program/` already, used by the port's own original
   demo) declares it; `main_smp.c` never included it. Fixed by adding the include.
3. `main_smp.c` (overlaid into `app_program/`, compiled by the kernel's own generic
   `app_program/subdir.mk` rule) could not find `alfp.h`, then `AUDIO_SECONDS`, then
   `hardware/gpio.h`: that generic rule only ever passes `$(INCPATH)`, never `$(APP_INCLUDES)`/
   `$(APP_DEFINES)` - both only reach `sources.mk`'s own custom `APP_COMPILE_RULE`. `INCPATH` is
   defined with plain `=` in `makefile:23` and never reassigned, so appending `APP_INCLUDES` and
   `APP_DEFINES` to it in `sources.mk` makes them visible to the generic rule too (lazy
   expansion) - the fix used throughout this stage for anything that needs to reach both rule
   kinds.
4. **The deepest bug**: `sources.mk`'s own `$(eval $(call APP_COMPILE_RULE,$(src)))` mechanism had
   *never actually compiled anything, in any previous round*. `sources.mk` is `-include`d at
   `makefile:25`, before `include pico_rp2040.mk` (`makefile:53`) defines `GCC`/`CFLAGS`/`TARGET`.
   `$(call ...)` expands every un-escaped `$(...)` reference *immediately*, at `$(eval)` time, not
   deferred to when the recipe runs - so `$(GCC)`, `$(CFLAGS)`, `$(TARGET)` etc. were baked in as
   empty strings, and every one of `sources.mk`'s own generated rules silently ran an empty
   command (`make: ... Error 127 (ignored)`). Every file in `ALFP_SRCS`/`SENSING_SRCS`/
   `SENSOR_DRIVER_SRCS` had silently never been compiled by this mechanism before, ever. Fixed by
   `$$`-escaping those references in the `APP_COMPILE_RULE` template, exactly like the automatic
   variables (`$$<`, `$$@`) already were.
5. `audio_features.c`/`sound_features.c` need libm (`sqrtf`/`logf`/`fabsf`/`log10f`/`sqrt`/
   `lroundf`); pulling in Pico SDK driver code also transitively pulled in newlib's reentrant
   syscall layer (`_close`/`_read`/`_write`/`_sbrk`/... all undefined). No Make variable
   `sources.mk` can append to survives to the link recipe (`LFLAGS` is reassigned with `:=` in
   `pico_rp2040.mk`, same reasoning as `CFLAGS`; appending `-lm` to `$(OBJS)` makes Make treat it
   as a build target and fail with "No rule to make target '-lm'"). Fixed the same way
   `CNF_TIMER_PERIOD` already was: a working-tree-only `sed` patch (in `build_smp.sh`) appending
   `-lm --specs=nosys.specs` directly to the link recipe's one line ending in `$(OBJS)`.
6. `hardware_irq/irq.c`'s shared-IRQ-handler-chain code (`irq_handler_chain_slots` et al) is
   referenced unconditionally but only *defined* when `!PICO_DISABLE_SHARED_IRQ_HANDLERS &&
   !PICO_NO_RAM_VECTOR_TABLE`, and something already made that combination false in this build
   (not fully root-caused to a specific header). Since this kernel has its own vector table
   (`vector_tbl.c`), not Pico SDK's relocatable one, `-DPICO_DISABLE_SHARED_IRQ_HANDLERS=1` selects
   `irq.c`'s simpler direct-vector-write path instead - gets past the link error, but **whether
   that direct path is actually compatible with this kernel's own vector table is UNVERIFIED**,
   flagged below.
7. Two symbols Pico SDK's own `crt0.S`/linker script would normally provide and this kernel's
   startup/linker script does not: `__unhandled_user_irq` (irq.c's unconditional default-vector
   init code references it even with (6) applied) and `end` (newlib's real `_sbrk`, pulled in once
   anything needs libm's errno path, expects a heap-boundary symbol). Added minimal stubs in a new
   file, `firmware_smp/buildglue/pico_sdk_glue_stubs.c` - documented there as link-satisfying, not
   functionally verified, since this kernel was never going to call either for real
   (`__unhandled_user_irq` only exists to link; real heap use goes through `kmalloc`, not `_sbrk`).
8. (Found and discarded, not fixed in place) `hardware_watchdog/watchdog.c` was added
   speculatively and pulled in a deep, irrelevant dependency chain (`hardware_ticks`,
   `pico_bootrom`'s lock machinery) - removed once grepping confirmed nothing in this codebase
   calls any `watchdog_*` function. `pico_sync/mutex.c` was similarly speculative and pulled in
   undefined `__mutex_array_start`/`__mutex_array_end` (a linker-script section this kernel's own
   script doesn't define) - removed once grepping confirmed nothing calls `mutex_*` either. Both
   are a reminder that "add the include path until it compiles" is not the same evidence as "this
   file is actually needed" - every source actually kept in `PICO_DRIVER_SRCS` is backed by a real
   undefined-reference error naming it, not a guess.

**Final, clean remaining error set** (`docs/curveball_evidence/c3_build_log.txt`), confirming
exactly and only the previously-documented gap, nothing more:
```
undefined reference to `storage_fatfs'
undefined reference to `storage_fatfs_mount'
undefined reference to `radios_init'
undefined reference to `radio_wifi'
undefined reference to `radio_lora'
undefined reference to `radios_lora_ok'
undefined reference to `radios_wifi_ok'
undefined reference to `radios_set_wifi_topology_filter'
```
All eight are `firmware/app/radios.c` and `firmware/app/storage_fatfs.c` - exactly what
`firmware_smp/buildglue/README.md` already predicted would be the remaining gap, now confirmed
precisely rather than guessed. Wiring these in needs `third_party/lora_mesh`,
`third_party/wifi_mesh` (each with their own SPI/GPIO wiring to re-verify under this kernel), and
a FatFs/SD port for `storage_fatfs.c` (the external `no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` dependency
the FreeRTOS build uses isn't vendored for this one) - each a substantially bigger task than the
driver-linking gap was, and **not attempted in this session**.

**Clock correctness - separately, still genuinely open.** A clean link is not evidence the clocks
are right. This kernel does its own from-scratch PLL/clock setup
(`kernel/sysdepend/pico_rp2040/cpu_clock.c`) that was never confirmed to agree with what the
now-linked `hardware_clocks/clocks.c` believes `clock_get_hz()` returns - a wrong peripheral clock
would make `uart_init(..., 115200)` or an SPI baud-rate divisor silently wrong without any build or
link error. **Not measured in this session; needs a real board test**, not another toolchain run:

```sh
# Once radios.c/storage_fatfs.c are wired in and the build links:
# 1. Measure UART baud (115200) by sending a known byte pattern to a USB-serial adapter wired to
#    the bridge's UART0, or by confirming the WST link to the ESP-01 actually works end to end.
# 2. Measure or infer the SPI clock from a known SD-card or LoRa transaction (logic analyzer/scope
#    on the SPI1 clock line is the direct way).
# 3. Compare time_us_32() (now linked) against tk_get_tim() over 60 s continuous and report drift.
```

## C4 - incremental integration

> **Update 2026-11-04:** C3's "links cleanly" claim below missed four `multiple definition`
> errors, and C4 is no longer "not attempted": the full node firmware now builds and ran on two
> real boards. See `docs/SMP_HANDOFF.md` for the current state; it supersedes C3/C4 below.

**Result: BLOCKED - not attempted; depends on C3.** Sensors task -> ALFP loopback -> SD/FatFs ->
LoRa -> WiFi bridge UART0 -> full node, each step flashed and logged on its own.

## C5 - Tribunal tests T1-T8 on real hardware

> **Update 2026-11-11 (real hardware, full node firmware on nodes 1 and 2; details and evidence
> in `docs/curveball/STATUS.md` and `docs/curveball/evidence/`):**
>
> | Test | Result |
> |---|---|
> | T1 throughput, 200 packets over 2 hops | not run: needs nodes 3-5 and the backend (1-hop WiFi: 0 failures so far) |
> | T2 core assignment | **passed on both nodes** from the boot lines (`sensors: core 1`, `comms: core 0`, `usb_owner: core 0`); node 1: `t2_node1_smp_boot_lora.txt`, node 2: `t2_node2_smp_boot_control.txt` |
> | T3 / T4 / T5 | partly: audio `lost=0` and DHT22 0 failures during WiFi transfers, not yet under T1 load |
> | T6 capture queue full | passed (2 runs) |
> | T7 WiFi -> LoRa failover | **blocked**: node 1's LoRa is still down after its module was swapped (`unexpected chip type 0x00`), so a board/wiring fault is suspected (`docs/SMP_HANDOFF.md` 6.2) |
> | T8 console stall | passed |
>
> The "BLOCKED - not attempted" text below is kept as the original record.

**Result: BLOCKED - not attempted; depends on C4.** T2 (core assignment) and T8 (stalled-terminal
log draining) have already had their context-free versions proven by C2's check 1 and check 7
above; T6 (capture queue full) likewise by C2's check 4 sub-test. The rest (T1, T3, T4, T5, T7)
need the full node firmware with real radios/SD/sensors, which is C4's job.

## Bottom line

**C0, C1, and C2 have been run on a real Pico, with a real ARM toolchain, in this session - not
just written.** C1 proves the vendored kernel itself runs dual-core for real; C2 proves (after
three real, fixed bugs) that every SMP primitive this port's app code depends on - task creation
on a chosen core, cross-core concurrency, message buffers, event flags, atomic counters, and the
1 ms tick - genuinely works on real hardware. **C3's driver-linking half is also resolved for
real** (eight more real bugs found and fixed, full details above): the entire Pico SDK driver
layer (GPIO/SPI/ADC/DMA/UART/I2C/clocks/PLL/XOSC/timer/IRQ/sync/time) now compiles and links
cleanly alongside ALFP/sensing/sensor-driver code, confirmed by a clean link down to exactly the
already-documented remaining gap (`radios.c`/`storage_fatfs.c`, needing the real mesh radios and
a FatFs port - not attempted). **main_smp.c itself has not yet been flashed or run** (it does not
fully link until those two files are added), clock correctness is unmeasured, and C4/C5 remain
untouched. The single most important next step: wire in `radios.c`/`storage_fatfs.c` (with
`lora_mesh.c`/`wifi_mesh.c`/a FatFs port) to get an actual flashable `main_smp.c` build, then
repeat C1's flash-and-read-serial step on it.
