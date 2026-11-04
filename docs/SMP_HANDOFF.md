# Dual-core (uT-Kernel 3 SMP) SmartRelay firmware - status and handoff

Branch: `curveball-integration`. Last hardware session: 2026-11-04.

This document is the single place to pick the dual-core work back up. It says what was built,
what was measured on real boards, what is half-done, and the exact next steps and commands.
`docs/curveball_results.md` has the earlier C0-C3 history; where the two disagree, this file is
newer.

---

## 1. One-paragraph summary

The full node firmware (same behaviour, console keys, SD layout and serial uplink format as the
FreeRTOS `firmware/app/main.c`) now builds and runs on the dual-core uT-Kernel 3 SMP kernel:
comms + console on core 0, sensors/audio/classifier alone on core 1. One image per node:
`uf2/csc09_smp_node_1.uf2` ... `csc09_smp_node_5.uf2`, next to (not replacing) the FreeRTOS
`csc09_node_N.uf2`. On real hardware, node 1 (bridge) and node 2 both ran it: SD card, WiFi mesh
(ESP-01), sensors, DHT22, audio + classifier, capture queuing, ALFP transfers node 2 -> node 1
over WiFi, and the bridge's `@ALFP` uplink to the laptop all worked. **One open problem blocks
calling it done:** on the bridge, finishing each received file takes ~11 s inside one
`alfp_service()` call, so the sender times out (`gave_up`) and resends, and the same image is
received several times. The most likely cause is node 1's SD card (a very full `inbox/`
directory), not the port; an A/B test to prove that was running when the session ended (section 5).

---

## 2. What state the boards were left in

| Board | USB port seen on the laptop | Firmware on it right now | Notes |
|---|---|---|---|
| Node 1 (bridge) | COM5 (FreeRTOS) / COM3 when on SMP | **FreeRTOS `uf2/csc09_node_1.uf2`** (flashed for the A/B test) | LoRa module dead on BOTH firmwares (section 6.2) |
| Node 2 | COM7 (SMP) / COM6 when on FreeRTOS | **SMP `csc09_smp_node_2.uf2`**, an earlier build than the committed one (no per-step timing line in `s`) | LoRa up, WiFi up, ~26 jobs queued on its SD card |

COM numbers are what this laptop assigned; they can change. Both SMP boards report USB serial
number `MTK3PICO` (the kernel's fixed USB descriptor); Windows still gives them separate COM ports
(by USB location), but tools that pick "the Pico by serial number" cannot tell them apart.

First thing next session: reflash node 1 with `csc09_smp_node_1.uf2` and node 2 with the current
`csc09_smp_node_2.uf2` (section 4), unless you are finishing the A/B test first (section 5).

---

## 3. What was done this session (all committed on this branch)

### 3.1 Files

| File | What it is |
|---|---|
| `firmware_smp/app/main_smp.c` | Full port of `firmware/app/main.c`: persistent capture ids (`meta/next_id`), sound-based image pick (`sip_*`), telemetry/image split (Task A.2), size-ladder test payloads (`p`), queue hold (`H`), loss injection (`d`), topology filter (`t`), stats incl. per-task core/stack and slowest comms step, plus `x` (T6), `z` (T8) and `B` (reboot to BOOTSEL). Uses plain `printf` like main.c. |
| `firmware_smp/app/log.c/.h` | Log ring (64 x 256-byte lines). New `log_putc()`: per-task line assembly so lines from both cores never interleave; a finished line waits up to 500 ms for room (like Pico USB stdio). |
| `firmware_smp/app/usermain_smp.c` | Calls `smartrelay_runtime_init()` before anything else. |
| `firmware_smp/rtos_port.h`, `rtos_port_mtk3.c` | Added `rtos_msgq_send_wait`, `rtos_self_id`, `rtos_atomic_cas_i32`; task stacks now come from one 24 KB pool instead of 8 x 8 KB fixed slots; self-test scratch buffer fixed (was too small for 256-byte lines, printed garbage). |
| `firmware_smp/buildglue/pico_runtime_mtk3.c/.h` | The Pico SDK runtime pieces the kernel does not provide (details 3.2). |
| `firmware_smp/buildglue/usb_sdk_compat_overlay.c` | Replaces the kernel's single-handler `usb_sdk_compat.c` with a real per-IRQ handler table (USB path unchanged); needed for the ESP-01 UART0 RX interrupt. |
| `firmware_smp/buildglue/driver_glue_mtk3.c` | Stubs for SDIO entry points (SPI-only SD), `get_fattime`, carlk3 `capture_assert`, and an unused LR1121 HAL function. |
| `firmware_smp/buildglue/smartrelay_sections.ld` | Places `.mutex_array` / `.uninitialized_data` between `.data` and `.bss`. |
| `firmware_smp/buildglue/sources.mk` | Compiles LoRa mesh + LR1121 driver, WiFi mesh, `firmware/app/{radios,storage_fatfs,hw_config,board}.c`, carlk3 FatFs/SD-SPI, sensing, sensors, ALFP and the needed Pico SDK drivers into `libsmartrelay.a` (archive, so only referenced code links, like the CMake build). Same `-D` defines as `firmware/CMakeLists.txt`. DHT22.o's code is moved into RAM (3.2). |
| `firmware_smp/buildglue/build_smp.sh` | Builds all five nodes by default, restores the submodule files it patches first (re-runnable), writes `uf2/csc09_smp_node_N.uf2`. |
| `firmware_smp/buildglue/elf2uf2.py` | ELF to UF2 converter; used because Windows Smart App Control intermittently blocks the kernel's own freshly built `elf2uf2.exe`. |
| `firmware_smp/buildglue/flash_node.py`, `serial_tstamp.py` | Flash + console capture helper; timestamped console logger. |
| `uf2/csc09_smp_node_{1..5}.uf2` | The images. |
| removed `firmware_smp/buildglue/pico_sdk_glue_stubs.c` | Its two stubs were wrong for a real heap (`end[1]`) and are superseded. |

### 3.2 Why the runtime glue exists (each item was a real problem)

The kernel has its own reset handler, clock setup and vector table, so nothing from Pico SDK's
`crt0`/`runtime_init` runs. `pico_runtime_mtk3.c` supplies what the drivers silently depend on:

1. **TIMER never counted.** The kernel never starts the watchdog tick and leaves TIMER in reset,
   so `time_us_64()` was stuck and every SDK timeout loop (SD, LR1121 BUSY, WiFi frame timeout)
   would spin forever. Fixed: unreset DMA/TIMER/PWM/PIO, start the 1 MHz tick.
2. **SDK clock bookkeeping was empty**, so `spi_set_baudrate()`/`uart_set_baudrate()` would
   divide by 0. Fixed: measure each clock with the RP2040 frequency counter and report it
   (`clock_set_reported_hz`). Measured on hardware: sys 125.001 MHz, peri 125.001 MHz,
   usb 48 MHz, adc 48 MHz (printed at boot as `clocks (measured): ...`). WiFi UART at 115200
   works, which confirms peri is right.
3. **SDK `irq.c` cannot coexist with the kernel's IRQ shim** (duplicate symbols; also writes the
   wrong vector table). Replaced by `usb_sdk_compat_overlay.c` (IRQs via `tk_def_int`).
4. **`auto_init_mutex` / `mutex_*`** (carlk3 SD driver): implemented with lazy init, owner = task
   id, and a kernel delay while waiting.
5. **`sleep_ms`/`sleep_us`**: kernel delay in task context, busy-wait otherwise. SDK `time.c`
   (alarm pools) is not linked.
6. **`get_rand_32`**: xorshift seeded from ROSC + timer. `pico_rand` is not linked because its
   board-id entropy reads flash with XIP off, unsafe while the other core runs from flash.
7. **newlib**: bounded 24 KB `_sbrk` heap, cross-core `__malloc_lock` (FatFs long names use
   malloc); `printf`/`snprintf`/`puts`/`putchar` use Pico SDK's `pico_printf` (reentrant, float
   support, no malloc) and route every byte to `log_putc()`.
8. **DHT22 failed ~40% of reads on dual-core** (17 good / 14 fail; FreeRTOS on the same board:
   0 fails). Cause: core 0's flash-cache traffic stalls core 1's bit-timing loop. Fix: DHT22.o's
   `.text` is renamed to a `.data.*` section after compiling, so it runs from RAM. **Measured
   after the fix: 87 good / 0 fail.** `third_party/sensors/DHT22.c` itself is untouched.
9. `B` key: reboots into BOOTSEL through the boot ROM (the kernel's USB stack has no Pico SDK
   reset interface, so `picotool`/1200-baud tricks do not work on the SMP firmware).

### 3.3 Correction to earlier docs

`docs/curveball_results.md` C3 and the old `firmware_smp/buildglue/README.md` said the Pico SDK
driver layer "links cleanly" apart from the radios/storage references. That missed four
`multiple definition` errors (SDK `irq.c` vs the kernel shim; a stale `usermain` from the smoke
overlay). Both are fixed as described above; the old wording should be read as superseded.

---

## 4. How to build and flash

### 4.1 Prerequisites (Windows laptop used in this session)

- Arm GNU Toolchain 14.2 (`arm-none-eabi-gcc`, `-ar`, `-objcopy`) on PATH, GNU make, a host C++
  compiler (MSYS2 ucrt64), Python 3 with `pyserial`.
- `PICO_SDK_PATH` = Pico SDK **2.1.1** (the version `firmware/` builds with), e.g.
  `~/smartrelay-deps/pico-sdk`.
- `FATFS_PICO_PATH` = carlk3 `no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` (`tools/fetch_deps.sh`).
- `git submodule update --init third_party/mtk3smp_rp2040`.

```sh
export PATH="/c/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin:/c/msys64/ucrt64/bin:$PATH"
export PICO_SDK_PATH="$HOME/smartrelay-deps/pico-sdk"
export FATFS_PICO_PATH="$HOME/smartrelay-deps/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico"
```

### 4.2 Build

```sh
firmware_smp/buildglue/build_smp.sh                 # all five nodes (~3 min per node)
NODE_IDS="1 2" firmware_smp/buildglue/build_smp.sh  # just these
SMP=0 NODE_IDS=2 firmware_smp/buildglue/build_smp.sh  # single-core fallback (NOT yet built/tested)
```

Everything it changes inside `third_party/mtk3smp_rp2040` is working-tree only and is reset at
the start of each run; never commit inside the submodule. If you switch Pico SDK versions, first
delete stale kernel objects:
`find third_party/mtk3smp_rp2040/build_make/mtkernel_3 \( -name '*.o' -o -name '*.d' -o -name '*.a' \) -delete`
(never `rm -rf` that directory: its `subdir.mk` files are tracked by the submodule).

### 4.3 Getting a board into BOOTSEL

| Board is running | How |
|---|---|
| SMP firmware | send `B` on its console (`flash_node.py --bootsel-key --port COMx`) |
| FreeRTOS firmware | open its COM port at 1200 baud: `python -c "import serial; serial.Serial('COM6',1200).close()"` (an error "device not functioning" is normal: it rebooted) |
| Anything / stuck | hold BOOTSEL, unplug and replug USB, release BOOTSEL |

### 4.4 Flash and watch

```sh
python firmware_smp/buildglue/flash_node.py uf2/csc09_smp_node_1.uf2 --bootsel-key --port COM3 --secs 40 --keys "30:s"
python firmware_smp/buildglue/flash_node.py --no-flash --port COM3 --secs 20 --keys "1:s,5:e"
```

Healthy SMP boot log (node 1, real capture):

```
CSC09 trail mesh, node 1, ALFP v1 (uT-Kernel 3 SMP build, dual core)
msgq selftest: wanted 4, sent 4, extra accepted 0, drained 4 - ok
sensors: core 1
comms: core 0
sd: mounted, 0 sample images in 0 groups:
capture id: next is N1-000001 (saved next 0, highest on card 0)
lora_radio: unexpected chip type 0x00 (SPI wiring?)      <- node 1 hardware, see 6.2
wifi: ESP-01 MAC AC:0B:FB:C7:7F:E9 = WST-1
node 1 ready (bridge), outbox has 0 queued, radio size threshold 1024 B
```

---

## 5. HALF-DONE: the bridge's 11-second receive finish (do this first)

### What was seen (real logs, node 1 SMP bridge, node 2 sending a 9894-byte image over WiFi)

- Node 2: `tx samples/weather_00.jpg: gave_up via wifi, 42 frags, 1 rounds, 0 resent, 11.0 s`
- Node 1: the file DID arrive intact (`rx ok`, CRC matches, `@ALFP FILE ... END` uplinked), about
  14 s after node 2 started, and then again for each resend (`dup` copies of the same image).
- Node 1 `s`: `longest comms loop 11508 ms` and
  `slowest comms step (ms): wifi poll 2 | lora poll 0 | alfp service 11508 | captures 0 | outbox 0`
- Host-side timestamps (`serial_tstamp.py`): all 207 uplink lines of a file reach the PC within
  0.23 s, so **USB/console output is not the bottleneck.**
- Node 1 WiFi `rxerr` climbs (64-204): the UART receive ring overflows *because* comms is stuck
  for 11 s, not the other way round (115200 baud is far within interrupt latency).

### Where the time goes

Inside one `alfp_service()` call on the bridge, i.e. `alfp/src/alfp.c` receive completion:
`rx_finish_ok()` does `close` + `rename(rx/... -> inbox/nX_SSSSS.bin)` (storage_fatfs's rename
also `f_unlink`s the target first) + create/write/close `inbox/nX_SSSSS.meta`. Each of those makes
FatFs scan the whole `inbox/` directory over SPI at 1 MHz. Node 1's card has received every test
transfer for many sessions (session ids are in the 24 000s), so `inbox/` likely has thousands of
entries (`.meta` names are long-file-name entries, 2-3 directory slots each).

### Hypothesis to confirm: SD card state, not the dual-core port

The A/B test in progress when the session stopped: **node 1 on FreeRTOS, node 2 on SMP.** If the
FreeRTOS bridge also takes ~11 s (node 2 still logs `gave_up` after ~11 s), it is the card.

```sh
# node 1 is on FreeRTOS right now (COM5); node 2 on SMP (COM7). Record both for 150 s:
python firmware_smp/buildglue/serial_tstamp.py COM5 150 > ab_n1.log &
python firmware_smp/buildglue/serial_tstamp.py COM7 150 > ab_n2.log
grep "tx \|gave_up\|done" ab_n2.log      # sender view
grep -v "@D\|\[ENV\]" ab_n1.log          # bridge view
```

Then:

1. **If FreeRTOS also gives up** -> card state. Copy node 1's SD card contents somewhere safe,
   then empty `inbox/` and `rx/` (or reformat FAT32 and copy back `samples/`), reflash
   `csc09_smp_node_1.uf2`, retest. Expect `gave_up` to become `done` and `alfp service` < ~1 s.
   Worth raising with the ALFP owner either way: a bridge that slows down linearly with files
   received will eventually fail in the field too (e.g. one sub-directory per day, or have the
   backend acknowledge and the bridge delete uplinked files).
2. **If FreeRTOS is fine** -> something SMP-specific makes SD operations slow. Next step: time
   the individual storage calls in `storage_fatfs.c` callers (wrap `s_rename`/`s_open_write`/
   `s_close` with `rtos_now_ms()` in a local debug build) and check `my_spi.c` DMA waits and
   `sd_card_spi.c` ready-waits under the glue's `millis()`/`busy_wait`. Things already ruled
   out: SPI/peri clock (measured correct), USB output speed (measured), UART interrupt routing
   (WiFi packets arrive and decode).

---

## 6. Other open items

### 6.1 Not yet run on hardware

- Nodes 3, 4, 5 (images built from the same source, only `NODE_ID` differs; never flashed).
- LoRa path end to end on SMP. Node 2's LR1121 initialises fine on SMP (`lora up`), but node 1's
  LoRa is broken (6.2), so no LoRa transfer to the bridge was possible. Telemetry (small files)
  therefore also went over WiFi via the fallback rule.
- Backend: uplink lines are byte-for-byte the same format as the FreeRTOS bridge (`common/uplink.c`
  is shared), but `backend/app.py` / `tools/bridge_receiver.py` was not run against the SMP bridge
  this session. Run: `SMARTRELAY_BRIDGE_PORT=COM3 uvicorn app:app --port 8000` in `backend/`
  (or `python tools/bridge_receiver.py --port COM3`) and check `backend/received/`.
  Both SMP boards share USB serial `MTK3PICO`, so set the port explicitly; autodetect only
  checks the VID.
- `SMP=0` single-core build of the full node: not built.
- `firmware_smp/smoke` (C2 smoke test): `log.c`/`rtos_port_mtk3.c` changed under it; not
  rebuilt. It should still build (new functions are additive), but confirm with
  `firmware_smp/smoke/build_smoke.sh` before relying on it.
- Tribunal tests T1-T8 (`docs/curveball_results.md` C5) on the full node firmware.
- Long soak (hours) with both nodes, watching `s`: stack free, heap used, log dropped, usb dropped.

### 6.2 Node 1 LoRa hardware fault (not a firmware issue)

`lora_radio: unexpected chip type 0x00 (SPI wiring?)` on node 1 with the dual-core image **and**
with the FreeRTOS `csc09_node_1.uf2` (stats: `lora DOWN` on both). The SD card on the same SPI1
bus works, so check the Core1121 module itself: seated, 3V3, its CS on GP13, RESET GP5, BUSY GP6.
Node 2's LoRa is fine on both firmwares.

### 6.3 Known limitations of the port (documented, accepted for now)

- Kernel USB descriptor has a fixed serial number (`MTK3PICO`) and no reset interface (hence the
  `B` key).
- `get_fattime()` returns 0, exactly what carlk3's `my_rtc.c` returns on these boards (RTC never
  set), so file timestamps match the FreeRTOS build.
- Interrupt priorities are not configurable (`irq_set_priority` is a no-op): every device IRQ
  runs at the kernel's default level, as USB already did.
- The SDIO half of the carlk3 driver is not built (these boards use SPI).
- `pico_rand` replaced by a xorshift generator (jitter/backoff only, not crypto).

---

## 7. Measured results so far (real hardware, 2026-11-04)

| Check | FreeRTOS (same boards) | Dual-core SMP |
|---|---|---|
| Boots, both cores used | n/a | yes: `comms: core 0`, `sensors: core 1`, `usb_owner: core 0` |
| SD mount (FatFs over SPI1) | yes | yes |
| WiFi (ESP-01, UART0 IRQ) | up | up, MAC check OK (`WST-1`) |
| LoRa node 2 / node 1 | up / DOWN | up / DOWN (node 1 hardware) |
| DHT22 good/fail | 15/0 | 87/0 after the RAM fix (17/14 before) |
| Audio capture 5 s + classifier on core 1 | yes | yes: `lost=0`, `model_us` ~1 ms, `feat_us` ~0.9 s |
| Capture -> telemetry + image jobs on SD | yes | yes (`capture N2-000338 telemetry queued`, `... queued`) |
| Node 2 -> node 1 file over WiFi, CRC OK | yes (FreeRTOS node 2 -> SMP bridge: 6 files) | yes, but bridge finish ~11 s -> sender `gave_up` + duplicates (section 5) |
| Bridge `@ALFP` uplink to PC | yes | yes, 207 lines in 0.23 s, no drops (`log dropped 0`, `usb dropped 0`) |
| Stack free (comms/usb_owner/sensors) | n/a | 5.2-5.6 KB / 2.2 KB / 7.4 KB of 8/3/8 KB |
| Heap used | n/a | 1.2 KB bridge, 21.7 KB node 2 (of 24 KB: watch this, raise `SMARTRELAY_HEAP_BYTES` if it grows) |

Note the node 2 heap figure: 21.7 KB of the 24 KB newlib heap after a few captures. It is
probably FatFs long-file-name buffers plus newlib's own allocation granularity and may plateau,
but if `heap used` keeps rising in a soak test, raise `SMARTRELAY_HEAP_BYTES` in
`pico_runtime_mtk3.c` (RAM is available: static use is ~163 KB of 264 KB).

---

## 8. Suggested order for the next session

1. Finish the A/B test (section 5) and fix or clear node 1's SD card accordingly.
2. Reflash both nodes with the committed `csc09_smp_node_1/2.uf2`; repeat the two-node test:
   `c` on node 2 a few times, expect `tx ...: done via wifi` on node 2 and one `rx from node 2`
   per image on node 1, `s` on node 1 shows `alfp service` well under a second.
3. Run the backend against node 1 and confirm images + telemetry rows appear (pairing by
   `capture_id`).
4. Fix or swap node 1's LoRa module, then test small telemetry over LoRa (size rule) and
   WiFi-down failover (unplug the ESP-01 on node 2).
5. Flash nodes 3-5 and do a multi-hop run (topology filter on, `t`).
6. Soak for an hour, then update `docs/curveball_results.md` C4/C5 with the real results.
