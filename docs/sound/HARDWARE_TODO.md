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

## H12 (A7/A9, multi-node): several field nodes, one bridge, the dashboard

Real captures from more than one node through node 1. Run it after H7. About 30 min (40 with node 5).

> **Status 2026-11-04 00:05: a, b, c not run.** Only 3 boards tonight, and the third (flashed as node 3) has no ESP-01 and
> its LoRa did not start (`lora DOWN | wifi DOWN`, log `docs/sound/evidence/node3_lora_2026-11-04.log`), so it cannot send.
> Concurrent transfers stay covered only by the host integration tests and ALFP's 4 receive sessions, not by hardware.
> Part d can run with node 2 in place of node 3.

### Setup (once)

1. **Flash each board on its own** (only that Pico plugged in, so picotool cannot pick the wrong one), with the image
   matching its ESP-01 (WST-n):
   ```
   ~/.pico-sdk/picotool/2.3.1/picotool/picotool load -f -x build-fw/csc09_node_1.uf2     (bridge, WST-1)
   ~/.pico-sdk/picotool/2.3.1/picotool/picotool load -f -x build-fw/csc09_node_3.uf2     (WST-3)
   ~/.pico-sdk/picotool/2.3.1/picotool/picotool load -f -x build-fw/csc09_node_5.uf2     (WST-5, optional)
   ```
   Windows: the same with `$env:USERPROFILE\.pico-sdk\picotool\2.3.1\picotool\picotool.exe`, or BOOTSEL + drag the
   `.uf2` onto **RPI-RP2**. Every field node needs an SD card with `sd_card/samples/` copied on; node 1 needs a card too.
2. **Which port is which node.** Stop the backend and close every monitor, plug all boards in, then:
   ```
   python tools/serial_log.py --identify
   ```
   **Expect** one line per Pico, e.g. `/dev/cu.usbmodem1101  node 1 (BRIDGE)` and `/dev/cu.usbmodem1201  node 3`
   (Windows: `COM7  node 1 (BRIDGE)`). The names stay the same as long as each board keeps its USB socket.
   No answer from a board: open it with miniterm and press `h`.
3. **Backend on the bridge's port only** (otherwise it grabs the first Pico it finds). Clean slate first:
   - macOS: `cd backend && source .venv/bin/activate && rm -f smartrelay.db && SMARTRELAY_BRIDGE_PORT=/dev/cu.usbmodemBRIDGE uvicorn app:app --port 8000`
   - Windows: `cd backend; .venv\Scripts\Activate.ps1; Remove-Item smartrelay.db -ErrorAction SilentlyContinue; $env:SMARTRELAY_BRIDGE_PORT="COMb"; uvicorn app:app --port 8000`

   **Expect** `bridge: connecting to /dev/cu.usbmodemBRIDGE automatically`, then `connected to bridge on ...`.
   Open http://localhost:8000. If the bridge is unplugged later, the backend logs `lost/unavailable ... retrying` and
   reconnects by itself.
4. **One logger per field node** (each in its own terminal, ml venv; type a key + Enter to send it):
   `python tools/serial_log.py --port <NODE3> --test H12_node3` (and `<NODE2>`, `<NODE5>` the same way).
   Every node's boot line must say `wifi: ESP-01 MAC ... = WST-<its own number>`; press `h` if the boot lines were missed.

To list what the backend stored (both OS):
```
python -c "import json,urllib.request as u;[print(c['display_id'],c['sound_label'],c['radio'],c['size_bytes'],c['image_name']) for c in json.load(u.urlopen('http://localhost:8000/api/captures?limit=30&include_demo=false'))['captures']]"
```

### a) Node 3 → bridge → backend → dashboard

In node 3's logger: `s` (expect `bridge last heard on wifi ... ms ago`, under about 3000), then `c`.
**Expect** within about 15 s: `[SOUND] group=... lost=0 ...`, `capture N3-000001: heard ... -> samples/..._NN.jpg`,
`capture N3-000001 queued`, `tx samples/...: done via wifi`; the backend logs `capture stored: node 3 ...`; the
dashboard shows a card **`N3-000001`**, no SIMULATED badge, with that image.
**Paste:** node 3's lines from `[ALERT]` to `done via`, the backend's `capture stored` line, a screenshot of the card.
**Pass:** the ID on serial, backend and dashboard is the same `N3-…`; `lost=0`. **Fail:** no `done via` in 60 s
(press `s`: is the bridge heard?), or a different ID on the dashboard.

### b) Node 2 and node 3: one after the other, then at the same time

The reviewer's concern: two nodes sending at once could mix up fragments or leave half-received files on the bridge.
ALFP keys every incoming transfer by (source node, session) and the bridge keeps up to 4 at once
(`ALFP_RX_SESSIONS` 4, `alfp/include/alfp_config.h`); this part checks that on hardware.
1. **Alternating:** `c` on node 2, wait for its `done via`; `c` on node 3, wait; repeat once (4 captures).
2. **Simultaneous:** type `c` in node 2's logger and press Enter, then **immediately** the same in node 3's (under
   1 s apart). Both transfers now overlap. Wait for both `done via`. Repeat once (4 more captures).
3. Stop the backend (Ctrl+C), open the bridge: `python -m serial.tools.miniterm <BRIDGE> 115200`, press `s`.
   Restart the backend afterwards.

**Paste:** the backend list (command above), the bridge's `s` lines, and each node's `done via` lines.
**Pass:**
- all **8** captures listed: node 2 and node 3 IDs each continuous (e.g. `N2-000004..07`, `N3-000002..05`), no ID
  twice, each `image_name` matching its `sound_label`'s group;
- the bridge's `s`: `rx ok` grew by 8, `crcfail 0`, **`expired 0`** (an expired transfer = a half-received file
  abandoned in `rx/`);
- no `REJECTED transfer` in the backend log.

**Fail:** a missing capture, a duplicated or skipped ID, `expired` > 0, or a REJECTED line. Note the time and both nodes'
`s` output.

### c) Node 5 → node 3 → node 1, a real two-hop (only if node 5 is available)

The WiFi topology filter is **ON at boot** on every node: E (5) cannot hear A (1) and must go through C (3) or D (4).
`t` *toggles* it and prints the new state; if anyone pressed `t`, press it again on that node until it prints
`wifi topology filter ON`. Node 4 must be **off** (or the path may be 5 → 4 → 1).
1. Node 3's logger: `s`, note `wifi: ... relayed R`.
2. Node 5's logger: `s` → `bridge last heard on wifi ... ms ago` (the bridge's heartbeat arrived via node 3). Then `c`.
3. Wait for node 5's `done via wifi`, then `s` on node 3 again.

**Paste:** node 5's `[SOUND]` .. `done via` lines, node 3's two `wifi:` stats lines, the dashboard card.
**Pass:** `done via wifi`; node 3's `relayed` went up by at least the transfer's fragment count (about 40-50 for a 10 kB
image); an `N5-…` card without SIMULATED. **Not a pass:** `done via lora` (LoRa has no filter, so that went straight
to the bridge): check node 5 hears the bridge on WiFi, and that node 3 is up and is WST-3.

### d) Store and forward: the bridge is off while node 3 captures

1. Unplug node 1's USB (the bridge is now off; the backend logs `retrying`).
2. Node 3: `c`, wait for `capture N3-… queued (...), outbox 1`; `c` again, `outbox 2`. `s` shows the bridge last heard
   longer and longer ago.
3. Plug node 1 back in (same socket). Wait up to 2 minutes.

**Paste:** node 3's `queued` lines and its `done via` lines after the bridge came back, the backend's `connected to
bridge` and `capture stored` lines.
**Pass:** both captures arrive by themselves after the bridge returns, in order, and node 3's `s` ends with `outbox 0`.
**Fail:** they never leave (`outbox` stays 2 after 2 min): note node 3's `s` output.

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
Without hardware (unplug every Pico so the backend does not auto-detect one): in a second terminal with the
**backend's** venv active (the simulator needs `requests`, which is in neither requirements file:
`pip install requests` once), run `python simulator/demo_feed.py --burst 5 --label-images sd_card/samples`
(tagged SIMULATED).

## H10 🪟 (A2/A11 checkpoint): a Windows teammate runs the tooling

```powershell
cd ml; py -3.11 -m venv .venv; .venv\Scripts\pip install -r requirements.txt; cd ..
ml\.venv\Scripts\python ml\smoke_test.py
cmake -S host -B build -DSOUND_ONLY=ON; cmake --build build --config Release; ctest --test-dir build -C Release
```
**Expect:** `smoke test: 34 passed, 0 failed` (check_parity's dry run needs the cmake build first) and the sound
tests passing under MSVC. **This is the first time the sound C code meets MSVC.**

## H11 (demo backup): screen recording of the real 2-node run

After H8 passes (and before the demo run-through), record segment 1 of `docs/sound/DEMO.md` once, with
W1 (dashboard) and W3 (node 2's monitor) side by side and the speaker audible.
- macOS: Cmd+Shift+5 → "Record Selected Portion" (Options → Microphone: MacBook microphone, so the clip is heard),
  or QuickTime → File → New Screen Recording. Stop with the ⏹ in the menu bar.
- Windows: Snipping Tool → Record (Win+Shift+R), or Xbox Game Bar (Win+Alt+R; records one window only).

**Expect** one video of about 60–90 s showing: the clip playing, `c` pressed, the `[SOUND] ... lost=0` line,
`capture N2-...: heard ... -> samples/...jpg`, `done via wifi`, and the dashboard card with the same ID.
Play it back once, start to end. Keep it **outside the repo** (e.g. Desktop; videos are large and are not
committed) and copy it to the team drive. This is the "nothing works" fallback in DEMO.md.

## Not started (out of tonight's scope)

- **A10:** T3 (audio under load) and T6 (queue full). `tools/serial_log.py` and the `H` console key are not written yet.
- **A11:** docs v1 (the demo script is `docs/sound/DEMO.md`).
