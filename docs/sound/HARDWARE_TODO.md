# Hardware checks deferred during the autonomous session

Do these **in order**. `docs/sound/TONIGHT.md` is the timed script that walks through them; this
file is the checklist with the exact commands and the output each step must show. Paste the
"expect" lines into the chat (or `docs/sound/evidence/`) so the tasks can be ticked.

## Setup (once per terminal, repo root)

**macOS**
```sh
source ml/.venv/bin/activate
export PATH="$HOME/.pico-sdk/cmake/v4.3.4/bin:$HOME/.pico-sdk/ninja/v1.13.2:$HOME/.pico-sdk/toolchain/15_2_Rel1/bin:$PATH"
export PICO_SDK_PATH=$PWD/deps/pico-sdk FREERTOS_KERNEL_PATH=$PWD/deps/FreeRTOS-Kernel FATFS_PICO_PATH=$PWD/deps/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
```
**Windows (PowerShell)**
```powershell
ml\.venv\Scripts\Activate.ps1
$env:PICO_SDK_PATH="$PWD\deps\pico-sdk"; $env:FREERTOS_KERNEL_PATH="$PWD\deps\FreeRTOS-Kernel"; $env:FATFS_PICO_PATH="$PWD\deps\no-OS-FatFS-SD-SDIO-SPI-RPi-Pico"
```
With the venv active, every `python ml/...` command below is the same on both systems.

**Flash an image:** hold BOOTSEL, plug the Pico in, then
- macOS: `cp build-fw/<image>.uf2 /Volumes/RPI-RP2/`
- Windows: drag `build-fw\<image>.uf2` onto the **RPI-RP2** drive.

**Serial monitor** (close it before running any script):
- macOS: `python -m serial.tools.miniterm /dev/cu.usbmodem* 115200`
- Windows: `python -m serial.tools.miniterm COM5 115200` (find COMx: `python -m serial.tools.list_ports -v`)
- Quit with Ctrl+].

**Play a test clip** at the mic: `python ml/playback.py ml/data/esc50/audio/5-203128-A-0.wav --device <N>`
(`python ml/playback.py --list-devices` lists N). Test-fold clips: dog `5-203128-A-0.wav`,
rooster `5-194930-A-1.wav`, crow `5-213836-A-9.wav`, frog `5-156026-A-4.wav`, cat `5-169983-A-5.wav`,
rain `5-181766-A-10.wav`, laughing `5-171118-A-26.wav`.

## H1 (A0): baseline hardware facts

Flash `sensor_test.uf2`, open the monitor and press `q` three times in a quiet room.
**Expect:** `[AUDIO] ... mean ADC <N> ... 16000 samples, 0 lost`, followed by a `[SOUND] group=... lost=0 ...` line.
Write N, the RC values on GP26, the MAX9814 gain and AR pin, the GP14 module part and the speaker model and
distance into the table at the bottom of `docs/sound/baseline.md`.

## H2 (A1): audio_dump records the real mic

Flash `audio_dump.uf2`, close the monitor, then run `python ml/pico_audio.py --test 2`. Clap during the recording.
**Expect:** `length=32000 mean=<about H1's N> std=<bigger than a quiet take> ... crc=ok`.
Then `python ml/pico_audio.py --test 2 --features`: **expect** 25 numbers and a `class: {...}` dict.

## H3 (A4): parity, Pico features = laptop features

`python ml/check_parity.py --takes 5`. Make a different sound for each take (talk, clap, play a clip).
**Expect:** `parity: PASS over 5 takes (tolerance 0.001 relative)`. Paste the whole table.

## H4 (A2): record the dataset (about 45 min, unattended)

```
python ml/prepare_esc50.py --per-category 10
python ml/record_dataset.py --list-devices
python ml/record_dataset.py --device <N> --volumes 1.0
python ml/record_dataset.py --ambient 10 --ambient-only --note quiet
python ml/record_dataset.py --ambient 10 --ambient-only --note fan
python ml/record_dataset.py --ambient 10 --ambient-only --note outdoor
```
**Expect:** the last line of each run shows every take `ok`, with no `FAILED`. Re-running resumes.
Done when there are ≥ 10 takes per category (it was ≥ 40 in the plan at 20 per category × 2 volumes;
tonight's demo plan halves both).

## H5 (A5): retrain on the mic recordings (about 5 min, no hardware)

```
cmake -S host -B build && cmake --build build -j
python ml/extract_features.py
python ml/train.py --features data/features_v1.csv
python ml/export_c.py --vectors
cmake --build build -j && ctest --test-dir build --output-on-failure
```
**Expect:** a `test:` line with group macro-F1 ≥ 0.6 (otherwise see TONIGHT.md, "if the mic model is worse"),
then `sound_model: 200/200 vectors match` and `100% tests passed`.

## H6 (A6): on-device bench evaluation and the [SOUND] line

1. Rebuild the firmware: macOS `cmake --build build-fw -j4`; Windows: the VS Code extension's Compile, or WSL.
2. Flash `audio_dump.uf2`, then run `python ml/bench_eval.py --device <N>`.
   **Expect:** a table where on-device group macro-F1 is within about 0.1 of offline, and `model time on the Pico`.
3. Flash `csc09_node_2.uf2` and open the monitor. Play the dog clip and wave at the PIR (or press `c` while it
   plays). **Expect:**
   `[SOUND] group=MAMMAL label=DOG id=20 conf=.. rms=.. zcr=.. lost=0 feat_us=<under 1250000> maxcall_us=<far under 100000> model_us=..`
   **Also expect no** `warning: ... over the 25% CPU budget` line. Paste the line: the feat_us and model_us values
   go into `docs/sound/baseline.md`.

## H7 (A7): capture ids survive a reboot

Node 2 with its SD card, monitor open. Press `c` three times, about 10 s apart. Unplug, replug, then press `c` twice.
**Expect:** the boot line `capture id: next is N2-000004 ...`, then `N2-000004`, `N2-000005` (they do not
restart at 1).

## H8 (A8): the image follows the sound

Copy the repo's `sd_card/samples/` folder onto node 2's card, keeping the old `img_NN.jpg`.
**Expect at boot:** `sd: mounted, ... sample images in .. groups: ambient 2 bird 2 ... img ..`.
Play the dog clip + PIR. **Expect:** `capture N2-...: heard DOG (..%) -> samples/dog_0X.jpg`, and the bridge
receives that file. Play the rain clip. **Expect:** `heard WEATHER (..%) -> samples/weather_00.jpg`.

## H9 (A9): end to end with the bridge, backend and dashboard

Flash `csc09_node_1.uf2` on the bridge. Then, in `backend/`:
- macOS: `SMARTRELAY_BRIDGE_PORT=/dev/cu.usbmodemXXXX uvicorn app:app`
- Windows: `$env:SMARTRELAY_BRIDGE_PORT="COM5"; uvicorn app:app`

Open http://localhost:8000. **Expect:** each capture card shows the `N2-000042` badge, a `DOG · 82% · MAMMAL` chip
and "identified on-node"; the Species page counts it. These pages were **never looked at in a browser during the
autonomous session** (DECISIONS D8), so check that they render.
Without hardware: `python simulator/demo_feed.py --burst 5 --label-images sd_card/samples` (tagged SIMULATED).

## H10 🪟 (A2/A11 checkpoint): a Windows teammate runs the tooling

```powershell
cd ml; py -3.11 -m venv .venv; .venv\Scripts\pip install -r requirements.txt; cd ..
ml\.venv\Scripts\python ml\smoke_test.py
cmake -S host -B build -DSOUND_ONLY=ON; cmake --build build --config Release; ctest --test-dir build -C Release
```
**Expect:** `smoke test: 34 passed, 0 failed` (check_parity's dry run needs the cmake build first) and the sound
tests passing under MSVC. **This is the first time the sound C code meets MSVC.**

## Not started (out of tonight's scope)

- **A10:** T3 (audio under load) and T6 (queue full). `tools/serial_log.py` and the `H` console key are not written yet.
- **A11:** docs v1 and the demo script (`TONIGHT.md` covers tonight's run-through).
