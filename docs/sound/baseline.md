# Sound baseline (A0)

Measured on 2026-11-02 before any sound-classifier change, on branch `sound` at `2e89e98`
(the same commit as `origin/curveball`, 12 commits ahead of `main`).

## Build environment

| | |
|---|---|
| Host compiler | Apple clang 21.0.0 (macOS 27, arm64) |
| Firmware toolchain | Arm GNU Toolchain 15.2.Rel1 (`arm-none-eabi-gcc` 15.2.1), from the Raspberry Pi Pico VS Code extension (`~/.pico-sdk/toolchain/15_2_Rel1`) |
| CMake / Ninja | 4.3.4 / 1.13.2 (also from the extension, `~/.pico-sdk/cmake`, `~/.pico-sdk/ninja`) |
| Firmware deps | pinned by `tools/fetch_deps.sh` into `deps/`: pico-sdk 2.1.1, FreeRTOS-Kernel `8be86d4`, carlk3 FatFS `d5e4534` |
| Python (ml/) | 3.12.15 venv, `ml/requirements.txt`; `ml/smoke_test.py` 15 passed, 0 failed |

The extension installs its own SDK (2.3.x), but the repo builds against the pinned 2.1.1 in `deps/`.
To build with the extension's tools from a terminal:

**macOS**
```sh
export PATH="$HOME/.pico-sdk/cmake/v4.3.4/bin:$HOME/.pico-sdk/ninja/v1.13.2:$HOME/.pico-sdk/toolchain/15_2_Rel1/bin:$PATH"
export PICO_SDK_PATH=$PWD/deps/pico-sdk
export FREERTOS_KERNEL_PATH=$PWD/deps/FreeRTOS-Kernel
export FATFS_PICO_PATH=$PWD/deps/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
cmake -S firmware -B build-fw && cmake --build build-fw -j4
```

**Windows (PowerShell)**
```powershell
$env:PATH = "$env:USERPROFILE\.pico-sdk\cmake\v4.3.4\bin;$env:USERPROFILE\.pico-sdk\ninja\v1.13.2;$env:USERPROFILE\.pico-sdk\toolchain\15_2_Rel1\bin;$env:PATH"
$env:PICO_SDK_PATH = "$PWD\deps\pico-sdk"
$env:FREERTOS_KERNEL_PATH = "$PWD\deps\FreeRTOS-Kernel"
$env:FATFS_PICO_PATH = "$PWD\deps\no-OS-FatFS-SD-SDIO-SPI-RPi-Pico"
cmake -S firmware -B build-fw -G Ninja; cmake --build build-fw -j4
```
(`tools/fetch_deps.sh` needs Git Bash or WSL on Windows; the version folders may differ, so check `~/.pico-sdk`.
The VS Code extension's Compile button or WSL also work; see `docs/BRINGUP.md`.)

## Results

- **Host tests:** 7/7 pass (`units`, `audio_features`, `transfer`, `uplink`, `uplink_python`,
  `lora_mesh_integration`, `wifi_mesh_integration`).
- **Firmware:** all 9 images build (`alfp_selftest`, `csc09_node_1..5`, `lora_photo_rx/tx`, `sensor_test`).
  The only warnings come from vendor code (the `lr11xx_radio.c` `#warning` and a linker note about duplicate libraries).

### Size: `arm-none-eabi-size`

| image | text | data | bss | dec |
|---|---:|---:|---:|---:|
| `csc09_node_2.elf` | 208572 | 0 | 109576 | 318148 |
| `sensor_test.elf` | 36416 | 0 | 20444 | 56860 |

(`text` includes the 9,184 B `.data` load image; `size` reports it as text on this link script.)

### RAM: `csc09_node_2`

| item | bytes |
|---|---:|
| Main SRAM (`0x20000000`–`0x20040000`) | 262,144 |
| Used statically (vectors + `.data` + `.bss`, ends at `0x2001e2b0`) | 123,568 (120.7 kB) |
| of which the FreeRTOS heap `ucHeap` (`configTOTAL_HEAP_SIZE` 48 kB, task stacks + queues) | 49,152 |
| of which the audio DMA ring `s_ring` | 16,384 |
| **Free main SRAM** | **138,576 (135.3 kB)** |
| Core 0 and core 1 main stacks | 2 kB each, in `scratch_y` / `scratch_x` (outside main SRAM) |

Rule 7 budget: ≥ 40 kB free, so about **95 kB** is available for sound work. The tasks run on FreeRTOS
stacks inside `ucHeap`, so new task-side arrays must be `static`, not on the 1024-word sensors stack.

### After A6-A8 (on-node classifier, persistent ids, image choice; 2026-11-03)

| image | text | bss | free main SRAM | vs baseline |
|---|---:|---:|---:|---|
| `csc09_node_2.elf` | 267276 | 113248 | 134,904 B (131.7 kB) | +58.7 kB flash (48.4 kB is the model's trees), +3.7 kB RAM |
| `sensor_test.elf` | 92592 | 22324 | | +56.2 kB flash (model) |
| `audio_dump.elf` (new) | 146088 | 166276 | 89,780 B | 160 kB raw-audio buffer, image runs alone |

New static RAM on the node: `sf_t` 1,856 B (sensing.c), `sip_table_t` 1,489 B and the outbox scan list 288 B
(main.c), result struct 24 B. Rule 7 (>= 40 kB free) holds with ~92 kB to spare. CPU time per capture
(`feat_us`, `model_us`) can only be measured on the board: HARDWARE_TODO H4.

## Notes found during A0

- **`firmware_smp/` exists on this branch.** `firmware_smp/buildglue/sources.mk` wildcards
  `firmware/sensing/*.c` and adds `-I common -I firmware/sensing`, so new sound C files are picked up.
  However, `docs/curveball_findings.md` ("Round 2") records that the µT-Kernel Make build does not link the
  Pico SDK runtime yet, so the µT-Kernel half of T3 (A10) depends on that being resolved first.
- **`host/CMakeLists.txt` was not MSVC-safe.** It passes GCC/Clang flags unconditionally and turns on
  ASan/UBSan by default. A3 fixed this for the sound part: the sanitizers are now off under MSVC, the sound
  targets live in `host/sound.cmake` with an MSVC branch, and `-DSOUND_ONLY=ON` (automatic under MSVC) builds
  only them. The ALFP simulator half stays POSIX-only (WSL on Windows). Not yet compiled with MSVC here:
  that is the 🪟 checkpoint.
- **The default `python3` on macOS can be too new.** On this Mac it is 3.14, which has no wheels for the pinned
  numpy/scipy, so `ml/README.md` names `python3.11`/`python3.12` explicitly.

## Hardware facts (from R)

| fact | value |
|---|---|
| RC filter on GP26 (R, C, cutoff = 1/(2πRC), must be < 8 kHz) | _TBD_ |
| MAX9814 gain (40/50/60 dB) and whether the AR pin is wired | _TBD_ |
| GP14 sound module (exact part, e.g. KY-038 / LM393) | _TBD_ |
| Playback speaker (model) and distance to the mic | **MacBook Pro built-in speakers** (pass it by name, `--device "MacBook Pro Speakers"`: macOS renumbers outputs when an iPhone microphone connects), about **8 cm** from the MAX9814 (H4, 2026-11-03). The first 19 H4 takes used the Dell S3225QS monitor speaker (output 1) at 100% and were re-recorded (too loud for the house); the first re-record went to output 4 (Microsoft Teams Audio, silent) and was redone on output 3 |
| `sensor_test.uf2` → `q` mean ADC value (quiet room) | **2083–2085** (first power-up, old sensor_test) and **2112–2114** (second power-up, tonight's build): it moves by about 30 counts between power-ups, inside the 2083–2117 seen before. 16000 samples, 0 lost every time (H1, 2026-11-03) |
| H1 `[SOUND]` timing, tonight's `sensor_test.uf2` (backup model) | 1 s takes: feat_us **81.5 ms** (8.2% of the recording), maxcall_us **1.25–1.42 ms**, model_us **0.27–0.50 ms**; the 5 s dual-trigger capture: feat_us **409.7 ms** (8.2%). No CPU-budget warning |
| H1 `[SOUND]` labels | quiet room: AMBIENT, conf 40, 72, 73, 79; claps: HUMAN 24 and BIRD 26 (group only, low confidence: claps are not a class). DC-corrected rms 16–18 quiet, 98–115 clap (the legacy rms 67–132 includes the 2112 − 2048 = 65 offset) |
| H1 note | quiet-room `zcr` is about 7800 crossings/s: around the true mean, the ADC's 1–2 count noise crosses almost every other sample. It is the same on Pico and host, so the classifier sees it consistently; the mic recordings (H4) train on it |
