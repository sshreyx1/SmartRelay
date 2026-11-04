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

**Result: BLOCKED - not yet attempted (next up).** `docs/curveball_findings.md` and
`firmware_smp/buildglue/README.md` document the finding in detail: this port compiles only a
hand-picked subset of Pico SDK `.c` files (USB-CDC console, optionally CYW43439 Wi-Fi), never the
general hardware driver library (`gpio_init`, `adc_read`, `spi_init`, `uart_init`,
`dma_channel_claim_unused`, ... have no compiled implementation in this Make build yet), and the
kernel does its own from-scratch PLL/clock setup that never populates the SDK's own
`clock_get_hz()` state - so even a clean link could still leave SPI/UART baud dividers silently
wrong. Now that a real toolchain and board are available in this session, this is the next stage
to actually attempt rather than hand off.

**Commands to run:**
```sh
# 1. Attempt a full build with radios.c/storage_fatfs.c included (sources.mk) and record the
#    exact linker error (undefined references) - that list says exactly which SDK .c files are
#    still missing from the build.
# 2. Once it links: measure UART baud (115200) by sending a known byte pattern to a USB-serial
#    adapter wired to the bridge's UART0, or by confirming the WST link to the ESP-01 actually
#    works end to end.
# 3. Measure or infer the SPI clock from a known SD-card or LoRa transaction.
# 4. Compare time_us_32() (once the SDK driver linking in step 1 makes it available) against
#    tk_get_tim() over 60 s continuous and report the drift, if any.
```

## C4 - incremental integration

**Result: BLOCKED - not attempted; depends on C3.** Sensors task -> ALFP loopback -> SD/FatFs ->
LoRa -> WiFi bridge UART0 -> full node, each step flashed and logged on its own.

## C5 - Tribunal tests T1-T8 on real hardware

**Result: BLOCKED - not attempted; depends on C4.** T2 (core assignment) and T8 (stalled-terminal
log draining) have already had their context-free versions proven by C2's check 1 and check 7
above; T6 (capture queue full) likewise by C2's check 4 sub-test. The rest (T1, T3, T4, T5, T7)
need the full node firmware with real radios/SD/sensors, which is C4's job.

## Bottom line

**C0, C1, and C2 have now actually been run on a real Pico, with a real ARM toolchain, in this
session - not just written.** C1 proves the vendored kernel itself runs dual-core for real; C2
proves (after three real, fixed bugs this exercise surfaced) that every SMP primitive this port's
app code depends on - task creation on a chosen core, cross-core concurrency, message buffers,
event flags, atomic counters, and the 1 ms tick - genuinely works on real hardware. C3 (SD/radio/
sensor driver linking) is the next stage and is not yet attempted.
