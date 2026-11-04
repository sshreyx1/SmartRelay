# CURVEBALL round 3, Task C: proving the dual-core SMP work, staged

Each stage below stands on its own: PASS, FAIL, or BLOCKED, with the exact command and (where it
could actually be run) the exact output. This machine has no ARM cross toolchain, no `cmake`, and
no `make` on `PATH` (confirmed by C0 below, and separately by `firmware_smp/buildglue/README.md`
and `docs/curveball_findings.md`), so C1 through C5 cannot be attempted here. Nothing below is
claimed "tested" unless it was actually run and its real output is pasted in.

## C0 - toolchain check

**Command:**
```sh
sh firmware_smp/buildglue/check_env.sh
```

**Result: FAIL (as expected) - run for real on this machine, 2026-11-04.** Raw output:

```
=== arm-none-eabi-gcc ===
MISSING
=== make ===
MISSING
=== cmake ===
MISSING
=== python3 ===
Python 3.13.7
=== picotool ===
MISSING
=== PICO_SDK_PATH ===
MISSING (not set)
=== third_party/mtk3smp_rp2040 submodule ===
already checked out (900e863)
=== summary ===
FAIL: at least one prerequisite is missing. Install/set the items marked MISSING above,
      then re-run this script before attempting build_smp.sh.
```

Notes on what this machine *does* have, since it matters for Tasks A/B/D elsewhere in this round:
a POSIX-shaped C toolchain is available two ways - MSYS2's `gcc`/`mingw32-make` directly on
Windows (but `host/sim/storage_posix.c` does not compile there: MinGW's `mkdir()` takes one
argument, not the POSIX two-argument form, so the host ALFP simulator specifically needs a real
POSIX environment), and WSL2 (`wsl.exe -d Ubuntu`) has a working `gcc` with real POSIX headers,
which is what this round's host-side ALFP/backend testing actually used (see Task D). Neither of
those is `arm-none-eabi-gcc`, and neither helps with C1-C5 below, which need the actual ARM
cross-compiler, `cmake`/`make` for the Pico SDK pieces, and a real RP2040 board.

**Next action (Shreya):** install `arm-none-eabi-gcc`, `make` (or point `PATH` at MSYS2's
`mingw32-make` and update `build_smp.sh`'s `make` calls if going that route - not attempted here
since the brief's existing scripts assume GNU `make` by name), `cmake`, and set `PICO_SDK_PATH` to
a real Pico SDK checkout. Re-run `check_env.sh` and paste its output back before C1.

## C1 - build the port's own demo, unchanged

**Command:**
```sh
export PATH="/path/to/arm-gnu-toolchain/bin:$PATH"
export PICO_SDK_PATH=/path/to/pico-sdk
cd third_party/mtk3smp_rp2040/build_make
SMP=1 CONSOLE=usb_cdc WIFI=none make
```
Then flash the resulting `.uf2` (hold BOOTSEL, plug in, drag onto the `RPI-RP2` drive) and watch
its USB serial output with any terminal (`tools/serial_log.py --port <port> --test c1_demo`).

**Result: BLOCKED - no ARM toolchain, `cmake`, or `make` on this machine (see C0).**

**Expected:** the port's own `app_program/app_main.c` (producer on core 0, consumer on core 1)
prints from both cores. **This is the single most important thing to run before trusting anything
else in this round's Task C** - if the vendored kernel itself does not come up dual-core on real
hardware with its own unmodified demo, nothing built on top of it (C2-C5) can be trusted either,
no matter how it looks on paper.

**What to paste back:** the full build log (or the exact compiler/linker error if it fails to
build), and the first ~30 lines of serial output after flashing (or "nothing printed" if it builds
but produces no output).

## C2 - `smp_smoke` firmware (no radios, no SD)

Written this round: `firmware_smp/smoke/smp_smoke.c` (the 9 checks below), `usermain_smoke.c`
(entry point), `sources.mk` + `build_smoke.sh` (its own minimal build, overlaying only the kernel
port's `log.c`/`rtos_port_mtk3.c` plus `smp_smoke.c` itself - no ALFP, no sensing, no sensor
drivers, no radios, no SD, deliberately smaller than `firmware_smp/buildglue/sources.mk`), and
`tools/smp_smoke_check.py` (host-side log parser, **unit-tested against a synthetic sample log in
this session - it works correctly on real input**, since that part needed no hardware or ARM
toolchain at all).

**Command:**
```sh
export PATH="/path/to/arm-gnu-toolchain/bin:$PATH"
export PICO_SDK_PATH=/path/to/pico-sdk
SMP=1 NODE_ID=2 firmware_smp/smoke/build_smoke.sh
# flash the resulting .uf2, then:
python3 tools/serial_log.py --port <port> --test smp_smoke --duration 60 > smoke_raw.log
python3 tools/smp_smoke_check.py smoke_raw.log
```

**Result: BLOCKED - cannot be compiled or run here (same missing toolchain as C0/C1).** The C
source itself has **not been compiled even once**; it is written directly against
`firmware_smp/rtos_port.h` and `firmware_smp/app/log.h` as those files actually read in this repo
(not guessed), and it inherits one honest, documented limitation from `rtos_port.h` itself:
`rtos_now_ms()`/`rtos_elapsed_ms()` wrap `tk_get_tim()`, a **millisecond**-resolution counter -
there is no verified microsecond hardware-timer call available in this build profile yet (that is
what C3 below is for). So check 3 (tick accuracy) in `smp_smoke.c` measures `rtos_delay_ms(1)` and
`rtos_delay_ms(2)` against that millisecond clock, not against `time_us_32()` - the brief's
suggested comparison against the hardware microsecond timer has to wait for C3 to resolve the SDK
linking gap first. This is called out in the source file's own header comment so it is not lost.

The 9 checks `smp_smoke.c` runs, each printing one `PASS <name>: ...` or `FAIL <name>: ...` line:

1. **Core id logging** - `core0_task` on core 0, `core1_task` on core 1, each via `rtos_core_id()`.
2. **Real concurrency** - core 0 spins alone for 1 s, then both cores spin together for 1 s;
   reports both counts and core 1's count as a fraction of core 0's solo count. PASS requires core
   1 made real progress and core 0's rate did not collapse (ruling out one core starving the
   other, which would mean they are not actually independent).
3. **Tick accuracy** - 1,000 calls each of `rtos_delay_ms(1)` and `rtos_delay_ms(2)`, min/mean/max
   measured against `rtos_now_ms()` (millisecond resolution; see the limitation above). A
   `CNF_TIMER_PERIOD=10` misconfiguration (the exact bug round 2 review fix A1 already found and
   patched in `build_smp.sh`) would show up here as every delay measuring ~10 ms regardless of
   what was requested - this check exists specifically to catch that class of regression again if
   the build script's patch ever stops applying.
4. **Cross-core message buffer stress** - producer on core 1, consumer on core 0, 100,000
   messages with a sequence number and a checksum, producer retries on a full queue rather than
   dropping (the brief's "nothing lost" is a contract on correct usage, not a free property);
   plus the full-queue case with a 4-deep queue shaped like the real capture queue (5th send must
   be refused), plus a log-ring capacity probe (70 sends with no drain must raise
   `log_dropped_count()`, not block).
5. **Event flags** - two different bits set together, only one waited on; the other must still be
   there afterward (`TWF_BITCLR`, not `TWF_CLR` - this is exactly the bug round 2 review fix A3
   found and fixed in `rtos_port_mtk3.c`; this check is the regression test for it).
6. **Atomic counters** - both cores call `rtos_atomic_add_u32()` 1,000,000 times each; final value
   must be exactly 2,000,000.
7. **Log ring under a stalled terminal** - 5 s of continuous logging from the ring with nothing
   draining it; `log_dropped_count()` must rise and no single logging loop iteration may take an
   unreasonable amount of time (the ring must never block a producer).
8. **Stack high-water** - `rtos_task_stack_free()` for both tasks, printed for a human to judge.
9. **Console input** - `rtos_console_getchar()` polled in `core0_task`'s loop after all checks
   finish; `'s'` prints a short stats line. Relies on the same USB-CDC console-input overlay round
   2 review fix A2 added (`usb_tinyusb_glue_overlay.c`), which `build_smoke.sh` applies exactly
   like `build_smp.sh` does.

**What to paste back:** the build log, the raw serial capture, and
`tools/smp_smoke_check.py`'s summary table.

## C3 - Pico SDK driver linking and clocks

**Result: BLOCKED - not attempted.** `docs/curveball_findings.md` and
`firmware_smp/buildglue/README.md` already document the actual finding here in detail: this port
compiles only a hand-picked subset of Pico SDK `.c` files (USB-CDC console, optionally CYW43439
Wi-Fi) through its own GNU Make build, never the general hardware driver library (`gpio_init`,
`adc_read`, `spi_init`, `uart_init`, `dma_channel_claim_unused`, ... have no compiled
implementation in this Make build yet), and the kernel does its own from-scratch PLL/clock setup
that never populates the SDK's own `clock_get_hz()` state - so even a clean link could still leave
SPI/UART baud dividers silently wrong. Resolving that (adding the missing SDK driver sources to
`sources.mk` the same way `alfp`/`sensing`/sensor-driver sources are already added) needs a real
toolchain-and-board cycle this session does not have, not more guessing from this environment.

**Commands to run once C1/C2 pass** (not run here):
```sh
# 1. Attempt a full build with radios.c/storage_fatfs.c included (sources.mk) and record the
#    exact linker error (undefined references) - that list says exactly which SDK .c files are
#    still missing from the build.
# 2. Once it links: measure UART baud (115200) by sending a known byte pattern to a USB-serial
#    adapter wired to the bridge's UART0, or by confirming the WST link to the ESP-01 actually
#    works end to end (common/wst_link.c's existing framing makes a wrong baud obvious as garbled
#    bytes, not silence).
# 3. Measure or infer the SPI clock from a known SD-card or LoRa transaction (a logic analyzer or
#    oscilloscope on the SPI1 clock line is the direct way; a FatFs mount succeeding at all is a
#    much weaker but still useful signal that the SPI clock is at least in a workable range).
# 4. Compare time_us_32() (once the SDK driver linking in step 1 makes it available) against
#    tk_get_tim() over 60 s continuous and report the drift, if any.
```

**What to paste back:** the full linker output from step 1 (or a successful link), the
oscilloscope/logic-analyzer trace or serial behavior from steps 2-3, and the drift measurement
from step 4.

## C4 - incremental integration

**Result: BLOCKED - not attempted; depends on C1-C3.** Planned order, each step flashed and
logged on its own before moving to the next, per the brief: sensors task (PIR, sound trigger,
DHT22 bit-bang timing under load, audio ADC+DMA with 0 lost samples) -> ALFP over
`common/loopback_radio.*` on the device itself (file round trip with CRC-32 verified) -> SD/FatFs
-> LoRa -> WiFi bridge UART0 (checking the UART RX ISR logs core 0 and the ring high-water mark)
-> full node. No command to give yet beyond "repeat C1-C3's build/flash/log cycle for each piece
as it is added to `firmware_smp/buildglue/sources.mk`" - the specific commands depend on what C3
actually finds about the SDK driver gap.

## C5 - Tribunal tests T1-T8 on real hardware

**Result: BLOCKED - not attempted; depends on C4 completing.** T1 (200 packets E->A over 2 hops,
>= 40 pkt/s, ring peak well below 2 KB, 0 CRC errors, TXDONE reaction <= ~3 ms), T2 (core
assignment via the stats line - `smp_smoke`'s check 1 above is the minimal version of this), T3 (0
lost audio samples under load), T4 (DHT22 under load), T5 (merged sensors task is enough - passes
if T3 and T4 both pass), T6 (capture queue full: 5th refused, logged, nothing blocks - `smp_smoke`
check 4's capqueue_full sub-check is the context-free version of this), T7 (WiFi-bridge unplug,
failover to LoRa within 10 s), T8 (USB output from both cores, stalled terminal does not block -
`smp_smoke` check 7 is the context-free version of this). Each needs the full node firmware (C4
complete) on real boards; none of it can be exercised with `smp_smoke` alone, which deliberately
has no radios or SD.

## Bottom line

**The dual-core SMP build has not been run on a real Pico from this session, at any stage.**
Everything above either genuinely ran on this machine (C0's script, and
`tools/smp_smoke_check.py`'s own parser logic against a synthetic log) or is BLOCKED and says so
plainly, with the exact command and expected output for Shreya to run on a machine that has the
toolchain and a board. C1 is the one to run first: if the vendored kernel's own unmodified demo
does not come up dual-core on real hardware, nothing past it in this list means anything yet.
