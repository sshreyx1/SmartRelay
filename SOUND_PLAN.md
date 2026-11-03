# SOUND_PLAN.md: Buddy 3 (Rhaylene), Sound Sensing & Analytics

> **For R:** put this file in the repo root. Then either add the line `@SOUND_PLAN.md` to `CLAUDE.md`, or start Claude Code with:
> *"Read SOUND_PLAN.md fully, then start at the first unchecked task in the Status Board."*
> Claude Code works through it one task at a time. After each task it tells you which files to stage and gives you the commit message, and you do the commit yourself.
> **Cross-platform:** the team uses **macOS (2) and Windows (3)**. Everything built from this plan must work on both (see rule 12).

**Guiding principle: the Pico does the work.** All sound processing, classification, species identification and decisions happen on the node. The backend and dashboard only **store and display** what the Pico decided, and never re-classify.

- **Phase A (2-day demo):** the Pico identifies the animal group and a set of animal sound types, then picks a matching image.
- **Phase B (submission):** the Pico identifies specific Singapore species. It also decides what is worth sending, sends compact activity summaries, and triggers acoustically. Everything is backed by field and hardware evidence.

---

## 0. RULES FOR CLAUDE CODE (read first, obey always)

1. **Never change git state.** Do NOT run `git add`, `git commit`, `git push`, `git checkout`, `git switch`, `git merge`, `git rebase`, `git stash`, `git reset`, `git tag` or `git branch <new>`. Read-only commands (`git status`, `git diff`, `git log`, `git branch --show-current`) are fine.
   When a task is done, print a **COMMIT BLOCK** exactly like this and stop:
   ```
   ── COMMIT BLOCK (run these yourself) ──
   git add <file1> <file2> ...
   git commit -m "<type>(<scope>): <summary>" -m "<body: what + why, 1-4 lines>"
   ──────────────────────────────────────
   ```
   Use Conventional Commits. Types: `feat`, `fix`, `test`, `docs`, `chore`, `refactor`, `perf`. Scopes: `sound`, `ml`, `backend`, `dashboard`, `tools`, `docs`. Do not add AI co-author lines unless R asks.
2. **One task at a time.** Work in Status Board order unless R says otherwise. When a task ends, show three things: what changed, how it was verified, and the COMMIT BLOCK. Then **wait for R to say "next"**.
3. **Hardware steps:** you cannot flash or touch the board. Give R the exact commands **for macOS and for Windows (PowerShell)**, and say exactly what output to paste back. Serial ports are `/dev/tty.usbmodem*` on macOS and `COMx` on Windows. Do not mark a hardware task done until R has pasted output that passes its acceptance criteria.
4. **Keep everything green.** Before every COMMIT BLOCK, run whichever of these apply:
   - `cmake -S host -B build && cmake --build build -j && ctest --test-dir build --output-on-failure`
   - `cmake --build build-fw -j4` (firmware; R's SDK env vars must be set; see A0)
   - `cd ml && python -m pytest -q` (run with the venv's Python: `.venv/bin/python` on macOS, `.venv\Scripts\python` on Windows)
   - `cd backend && pytest -q`
   - `cd dashboard && npm run build`
5. **Do not edit these:**
   - `third_party/sensors/mic.c`, `mic.h` and `firmware/sensing/audio_features.*`. These are the legacy reference, and `host/tests/test_audio.c` must keep passing.
   - Any radio, mesh or ALFP protocol code (`alfp/src`, `mesh/`, `third_party/lora_mesh`, `third_party/wifi_mesh`).
6. **Teammates' files you MAY edit (R approved), but log every edit in `docs/sound/HANDOFF.md`:**
   - `common/capture_meta.h`, `firmware/app/main.c`, `firmware/sensortest/sensortest_main.c` and `firmware/CMakeLists.txt` (Shreya, shared)
   - `tools/bridge_receiver.py`, `simulator/demo_feed.py`, `backend/*` and `dashboard/src/*` (Shreya)
   - `firmware/sensing/sensing.c/.h` (shared with Maryam)
   - `host/CMakeLists.txt`
   - **Anything that changes the trigger rule, ADC ownership or `trigger_trust` needs Maryam's OK first. Ask R before starting.**
7. **Embedded constraints (RP2040, no FPU, 264 kB RAM):**
   - New firmware code must not use `malloc`. Use static buffers only.
   - Keep large arrays off the sensors task stack, which is 1024 words.
   - Sound processing must be **streaming** over the 16 kB DMA ring. The node firmware can never hold the whole 5 s of audio.
   - Do not add third-party C libraries. The µT-Kernel build (`firmware_smp/buildglue/sources.mk`) wildcards `firmware/sensing/*.c`, so put new C files in `firmware/sensing/` and keep them pure C (no Pico headers) wherever possible.
   - After every firmware change, report `arm-none-eabi-size` for `csc09_node_2.elf` and keep **≥ 40 kB RAM free** (static + FreeRTOS heap counted).
8. **Never let a failure turn into a fake value.** A missing reading becomes `CM_MISSING_*` or `CM_UNKNOWN_U8`, never 0. The Pico says "unknown" rather than guessing.
9. **The backend never classifies.** It decodes, stores and displays. If a feature needs a decision, the decision happens on the Pico.
10. **AI evidence log.** The course marks *critical evaluation of AI*. Whenever you (Claude) propose something and R rejects or changes it, or you find a flaw in an earlier AI suggestion, append one row to `docs/sound/AI_LOG.md`:
    `| date | what AI suggested | problem found / how verified | what R decided |`
    Keep it honest and short. Seed it with this one: *AI first proposed backend species ID (BirdNET on a sent audio clip). R rejected it because it moves the main analytics off the Pico, against the brief, and costs airtime and RAM. Species ID was redesigned to run on-device.*
11. **Keep the Status Board current.** Tick `[x]` and add a one-line result (e.g. `macro-F1 0.81, lost=0`) to the board as tasks finish. Editing this file is allowed; it gets committed with the task.
12. **Cross-platform (macOS + Windows), mandatory for every tool, script and doc:**
    - **Python only for tooling.** No new `.sh` / bash-only scripts and no Makefiles as the *only* entry point. Every workflow gets a Python entry point (`python ml/pipeline.py`, `python tools/serial_log.py`, …). Existing `.sh` files may stay, but anything new that the team must run has a `.py` equivalent.
    - **No OS-specific tools inside scripts:** no `afplay`, `sips`, `screen`, `brew`, `open`, `say` or `cp`/`rm` shell calls. Use these instead:
      - audio playback: `sounddevice` + `soundfile` (`ml/playback.py`)
      - images: Pillow
      - mp3 decoding: `soundfile`, whose wheels bundle libsndfile with MP3 support; fall back to ffmpeg only if it's present on PATH
      - serial monitor: `python -m serial.tools.miniterm`
      - file ops: `shutil` / `pathlib`
    - **Serial ports:** auto-detect with `serial.tools.list_ports` by the Pico's USB VID `0x2E8A`. Accept `--port COM5` or `--port /dev/tty.usbmodem1101`. Never hard-code a path.
    - **Paths:** `pathlib.Path` everywhere, with no hard-coded `/`. The SD card is passed as `--sd <path>` (`E:\` or `/Volumes/<card>`).
    - **Files:** always `open(..., encoding="utf-8", newline="")` for text/CSV, because Windows defaults to cp1252. Write binary data in binary mode.
    - **Executables:** find built host tools with `shutil.which` or by trying both `sound_featx` and `sound_featx.exe` under `build/` (plus `build/Debug`/`Release` for MSVC multi-config generators). Put this in one helper, `ml/hosttools.py`.
    - **Host C code** (`sound_featx`, tests) must stay portable C99: no `mmap`, `getopt`, `unistd.h` or POSIX-only calls. Guard GCC/Clang-only flags such as sanitizers in `host/CMakeLists.txt` with `if(NOT MSVC)`. It must build with gcc/clang (macOS, Linux, WSL) **and** MSVC or MinGW (Windows).
    - **Line endings:** add or extend `.gitattributes`:
      - `* text=auto`
      - `*.sh text eol=lf`
      - `*.py text eol=lf`
      - `*.uf2 *.npy *.scl *.jpg *.png *.wav binary`
    - **Serial protocol parsers** must accept both `\n` and `\r\n`.
    - **Docs:** every command block shows **macOS** and **Windows (PowerShell)** variants. Set environment variables with `export X=...` on macOS and `$env:X="..."` in PowerShell.
    - **Firmware builds on Windows:** teammates use the official **Raspberry Pi Pico VS Code extension** or WSL (see `docs/BRINGUP.md`). Do not assume one; document both.
    - **Verification:** you only run on one OS. For every task that adds a Python tool, also add or extend `ml/smoke_test.py`, which runs every CLI with `--help` plus a no-hardware dry run. Then **ask R to have a Windows teammate run it** at the checkpoints marked 🪟 in the Status Board, and paste the result.

---

## 1. STATUS BOARD

Priorities: **P0** = must have, **P1** = should have, **P2** = stretch.

### Phase A: 2-day demo

| ✓ | ID | Pri | Task | Result |
|---|----|-----|------|--------|
| [x] | A0  | P0 | Baseline: branch, builds, Python env, mic facts | host 7/7, 9 fw images, node_2 135.3 kB RAM free, smoke 15/15; hw facts TBD (H1) |
| [ ] | A1  | P0 | `audio_dump` firmware: stream raw 5 s audio to the Mac | code + build done (audio_dump.uf2, pico_audio.py, 7 parser tests); HW check H2 pending |
| [ ] | A2  | P0 | Dataset: ESC-50 → label map → record through the Pico mic 🪟 | scripts done, ESC-50 downloaded, dry-run 270 takes ≈ 43 min; recording H4 pending |
| [x] | A3  | P0 | Streaming spectral features v1 (C) + host CLI + tests | 25 features v1, sf_t 1856 B, 6 host tests + sound_featx (chunk-size invariant) |
| [ ] | A4  | P0 | Feature extraction + Pico/host parity check | extract/parity/plots done, parity dry-run PASS; live parity H3 pending |
| [ ] | A5  | P0 | Two-stage model (group → animal) + C export + parity test | BACKUP model (ESC-50 direct, full grid): test group F1 0.613, coverage 0.24, 48 kB, C=Python 200/200; mic retrain H5 pending |
| [ ] | A6  | P0 | Classifier on the node + bench confusion matrix | on-node classify + [SOUND] line + bench_eval.py; 10 images build, node_2 131.7 kB RAM free; H6 pending |
| [ ] | A7  | P0 | Persistent capture ID on SD + `N2-000042` naming everywhere | meta/next_id + outbox/clips scan, N2-000042 C/Py + tests, FF_USE_LFN=3; reboot check H7 pending |
| [ ] | A8  | P0 | Pico picks the image from the sound label | sound_image_pick.c + test, 35 credited images ≤10 kB in sd_card/samples; H8 pending |
| [x] | A9  | P1 | bridge_receiver / backend / dashboard / demo_feed: show the Pico's labels + capture ID | labels+display_id in bridge/backend (/api/species, 5 pytest), dashboard badge/chip/species, npm build OK; not browser-checked (D8) |
| [ ] | A10 | P0 | Hardware test T3 (audio under load) + T6 (queue full) | |
| [ ] | A11 | P0 | Docs v1 + demo script 🪟 | |

### Phase B: submission

| ✓ | ID | Pri | Task | Result |
|---|----|-----|------|--------|
| [ ] | B1  | P0 | Singapore species list + xeno-canto dataset (recordist split) | |
| [ ] | B2  | P0 | Features v2: mel bands, call rhythm, pitch contour | |
| [ ] | B3  | P0 | On-device species model with "unknown" rejection | |
| [ ] | B4  | P0 | Resource + latency table, T3 rerun on v2 | |
| [ ] | B5  | P0 | Sound-gated sending: the Pico decides what is worth transmitting (with Maryam) | |
| [ ] | B6  | P1 | On-device activity summaries (per-species counts) sent as compact files | |
| [ ] | B7  | P1 | Acoustic trigger on the ADC + pre-trigger buffer (with Maryam) | |
| [ ] | B8  | P0 | Field validation vs Merlin Bird ID | |
| [ ] | B9  | P1 | Evaluation: before/after, ablation, rejection curve | |
| [ ] | B10 | P2 | Tiny int8 CNN vs forest comparison | |
| [ ] | B11 | P2 | Spectrogram fingerprint sent from the Pico | |
| [ ] | B12 | P1 | Dashboard for Phase B: species, activity chart, gate stats, fingerprint | |
| [ ] | B13 | P1 | T5 merged vs split sensor task (with Maryam) | |
| [ ] | B14 | P0 | One-command retrain pipeline + final regression 🪟 | |
| [ ] | B15 | P0 | Final docs, report snippets, AI evidence row, handoff | |
| [ ] | B16 | P0 | Full 5 s ADPCM clip streamed to SD in 1 kB chunks (sensors → comms) | |
| [ ] | B17 | P0 | Comms cuts the loudest 1.5 s highlight from the SD file | |
| [ ] | B18 | P0 | Send the highlight as a 2nd ALFP file (gated), delete after delivery | |
| [ ] | B19 | P0 | Backend: decode clip → WAV, serve it, flags table + API + CSV export | |
| [ ] | B20 | P0 | Dashboard: ID badge, audio player + spectrogram, flag/review panel, Flagged page | |
| [ ] | B21 | P1 | `tools/scl2wav.py` + field workflow doc for pulling flagged clips off SD 🪟 | |

**2-day schedule (Phase A):**
- **Day 1:** A0 → A6. Start the A2 recording run early (it runs unattended) and write A3 in parallel.
- **Day 2:** A7 → A11.
- **Demo:** play a dog bark and wave at the PIR. The serial line shows `capture N2-000042: MAMMAL › DOG 82%`, the node sends `dog_00.jpg`, and the dashboard shows it with the label and ID.

**Suggested Phase B order:** B1 → B2 → B3 → B4 → B5 → B16 → B17 → B18 → B19 → B20 → B8 (book a field session early) → B6 → B7 → B9 → B12 → B21 → B13 → B14 → B15. B10 and B11 only if time remains.

---

## 2. CONTEXT: WHAT EXISTS (verified from the repo on 2 Nov)

**Hardware (node).** Maker Pi Pico (RP2040).
- MAX9814 mic AO goes through an RC filter to **GP26 / ADC0**.
- A separate sound-module digital out on **GP14** is the trigger (`sound.c`).
- Other sensors: PIR on GP0, DHT22 on GP4, light on GP27 (ADC1, the **same ADC** as the mic).

**Firmware.** FreeRTOS with 3 tasks:
- `comms` (prio 3) owns both radios, the SD card and ALFP.
- `sensors` (prio 2) polls every 10 ms.
- `console` (prio 1).

**Sound path today:**
- **Trigger.** `sensing_dual_trigger()` fires on PIR && GP14 in the same 10 ms poll, with a 5 s cooldown.
- **Capture.** `sensing_capture()` calls `audio_capture(5 s)`. The ADC free-runs at 16 kHz, and DMA fills a 16 kB ring (8192 samples, about 512 ms of slack). The sensors task drains it every ~4 ms into `audio_feat_add()`.
- **Classification.** `audio_features.c` mirrors `mic.c`: energy and zcr, then **two hand-set thresholds**:
  - energy > 50000 → 1 `LOUD_ANIMAL_CALL`
  - zcr > 0.15 → 2 `HIGH_FREQ_RUSTLE`
  - otherwise → 0 `AMBIENT_NOISE`
- **Output.** `capture_meta_t` gets `sound_class`, `sound_rms` and `sound_zcr`. `sound_conf` is never filled.

**Image choice today.** `main.c handle_capture()` (comms task) cycles `samples/img_NN.jpg` round-robin. The image has no link to what was heard.

**Known problems:**
- The thresholds were never trained. All 5 hardware captures came out as RUSTLE (assumption A3).
- The mean ADC reading is 2083–2117, not 2048, so the DC offset leaks into energy.
- The MAX9814 has **AGC**, so absolute loudness is unreliable. Use *spectral shape*.
- FR1 short-peak risk (A4b) is open.
- PostTri1 tests where R is owner are open:
  - **T3:** audio under load, lost = 0 for all classes, FreeRTOS + µT-Kernel
  - **T5:** merged vs split sensor task
  - **T6:** capture queue full

**Pipeline:**
- `app_request_capture(meta)` → FreeRTOS queue (depth 4) → comms `handle_capture()` → `alfp_outbox_add(path, meta)` → ALFP → bridge node 1.
- The bridge streams `@ALFP FILE … meta=<hex>` to the laptop (`common/uplink.h`), then `tools/bridge_receiver.decode_meta()` → `backend/` (SQLite, WebSocket) → React dashboard.
- Bytes after the 27-byte meta record are an **optional original file name** (already supported in `decode_meta`, total ≤ 64 B).
- The dashboard has placeholders: "species ID: TBD" (`CaptureFeed.tsx`, `CaptureModal.tsx`), `SpeciesPage.tsx`, and HomePage "Species seen".

**Limits:**
- ALFP meta ≤ 64 B; files ≤ 512 fragments.
- Sample images are 3–10 kB.
- Static RAM is about 110 kB of 264 kB.

---

## 3. TARGET DESIGN

```
 FIELD NODE (Pico): ALL decisions here                            BASE STATION: display only
 ─────────────────────────────────────────────────────            ─────────────────────────────
 trigger: PIR && sound (GP14 now; ADC acoustic trigger in B7)
     │
 GP26 ADC 16 kHz ─► DMA ring ─► sound_features (streaming, 256-pt FFT frames)
                                   │
                         stage 1: GROUP model  ──► AMBIENT/BIRD/MAMMAL/INSECT_FROG/HUMAN/VEHICLE/WEATHER
                                   │ (animal groups only)
                         stage 2: per-group ANIMAL/SPECIES model + "unknown" rejection
                                   │
                         label + confidence ─► capture_meta (sound_class=label id, sound_conf)
                                   │
              ┌────────────────────┼────────────────────────┐
     B5 gate: worth sending?   A8: pick image for label   B6: count in activity summary
              │                    │                          │
          skip / send ───────► ALFP image + meta ─────────► bridge ─► backend stores ─► dashboard shows
                                                  summary file ───────► activity chart
```

### Label table: ONE source of truth

Create `common/sound_classes.h` and its Python mirror `common/sound_classes.py`, with a host test that parses the header and diffs it against the Python file. Each entry has: `id` (u8, stable forever, never reused), `name`, `group_id`, `kind` (GROUP / ANIMAL / SPECIES), and `image_prefix`.

| id range | meaning | entries |
|---|---|---|
| 0–6 | group only (no finer ID or rejected) | 0 AMBIENT, 1 BIRD, 2 MAMMAL, 3 INSECT_FROG, 4 HUMAN, 5 VEHICLE, 6 WEATHER |
| 16–47 | Phase A animal sound types (ESC-50) | 16 ROOSTER, 17 HEN, 18 CROW, 19 SONGBIRD, 20 DOG, 21 CAT, 22 COW, 23 PIG, 24 SHEEP, 25 FROG, 26 CRICKET, 27 INSECT_BUZZ |
| 64–127 | Phase B Singapore species | filled in B1 (e.g. 64 ASIAN_KOEL …) |
| 0xFF | unknown / capture failed | |

- `sound_class` on the wire carries the **label id**. The group comes from the table, so the 27-byte layout stays v1.
- Old values 1 and 2 (LOUD_ANIMAL_CALL / RUSTLE) are retired. **All nodes must be reflashed together** and the dev DB cleared. Note both in HANDOFF.md.
- `sound_conf`: if a stage-2 label is given, it is the stage-2 confidence; otherwise it is the stage-1 confidence (0–100).

### Phase A ESC-50 mapping (`ml/label_map.yaml`)

| group | ESC-50 categories → label |
|---|---|
| BIRD | rooster→ROOSTER, hen→HEN, crow→CROW, chirping_birds→SONGBIRD |
| MAMMAL | dog→DOG, cat→CAT, cow→COW, pig→PIG, sheep→SHEEP |
| INSECT_FROG | frog→FROG, crickets→CRICKET, insects→INSECT_BUZZ |
| HUMAN | footsteps, laughing, coughing, clapping (group only) |
| VEHICLE | car_horn, engine, airplane, chainsaw (group only) |
| WEATHER | rain, wind, thunderstorm, sea_waves (group only) |
| AMBIENT | device-recorded quiet (`--ambient`) |

---

# PHASE A: 2-DAY DEMO

### A0: Baseline (P0)

1. Run `git status`, `git branch --show-current` and `git log --oneline -5`. If R is not on a fresh branch, **print** (do not run):
   `git switch main && git pull && git switch -c feature/sound-classifier`
   Wait for R to confirm.
2. Check whether `firmware_smp/` exists on this branch. If it only exists on `curveball`, the µT-Kernel half of T3 is run there later.
3. Build and run the host tests and the firmware (rule 4). Record the baseline `arm-none-eabi-size build-fw/csc09_node_2.elf` in `docs/sound/baseline.md`.
4. Create `ml/` with these files:
   - `ml/requirements.txt`: numpy, scipy, pandas, scikit-learn, soundfile, sounddevice, pyserial, pyyaml, matplotlib, pillow, pytest. Pin versions that have wheels for macOS arm64/x86_64 **and** Windows x64, Python 3.11.
   - `ml/.gitignore`: `.venv/`, `data/raw/`, `data/esc50/`, `data/xc/`, `*.wav`, `*.mp3`, `*.zip`, `*.npy`
   - `ml/README.md`
   Print the setup for both OSes:
   - **macOS:** `cd ml && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt`
   - **Windows (PowerShell):** `cd ml; py -3.11 -m venv .venv; .venv\Scripts\pip install -r requirements.txt`
   - No ffmpeg is needed (rule 12).
   Also add `.gitattributes` (rule 12), `ml/hosttools.py`, `ml/playback.py` (`play(path, volume)`, blocking, via sounddevice) and `ml/smoke_test.py` (initially it checks imports + port listing).
5. **Ask R** for each of these and write the answers into `docs/sound/baseline.md`:
   - the RC filter values on GP26 (cutoff = 1/(2πRC); it should be < 8 kHz)
   - the MAX9814 gain setting (40/50/60 dB) and whether the AR pin is wired
   - the exact GP14 module (e.g. KY-038 / LM393)
   - the playback speaker and its distance to the mic
   - the `sensor_test.uf2` → `q` mean ADC value
6. Create `docs/sound/AI_LOG.md` (header + the seed row from rule 10) and `docs/sound/HANDOFF.md` (sections: Shreya, Maryam, Goutham, Mridula, all).

**Done when:** host tests pass, the firmware builds, the baseline file is filled in and the venv works.
**Commit:** `chore(sound): add ml workspace, baseline measurements and sound docs skeleton`

---

### A1: `audio_dump` firmware tool (P0)

Goal: exact, hardware-true 16 kHz recordings from the real mic, so the model learns what the node actually hears (AGC, bias, RC filter, noise).

- New target `audio_dump` in `firmware/CMakeLists.txt`. Copy the `sensor_test` pattern: no FreeRTOS, no radios, no SD, USB stdio only.
- `firmware/audiodump/audiodump_main.c`:
  - Static `uint16_t buf[16000*5]` (160 kB fits in this build). Use the same ADC setup as `audio_capture.c`, but DMA **straight into `buf`** so nothing can be lost.
  - Command protocol over USB serial:
    - `r<sec>\n` (1–5): record, then send:
      ```
      @AUD BEGIN rate=16000 n=<N> mean=<mean>
      @A <base64, 64 chars/line>
      ...
      @AUD END n=<N> crc=<crc32 hex>
      ```
    - `f\n`: print the on-device feature vector for the last recording as `@FEAT v,v,...` (stub until A3).
    - `c\n`: print the on-device classification for the last recording as `@CLS id=.. name=.. conf=..` (stub until A5).
    - `p\n`: ping, which answers `@PONG`.
  - Use base64 text, not raw binary, because Pico stdio may turn `\n` into `\r\n`.
- `ml/pico_audio.py`: `PicoAudio(port=None).record(seconds) -> np.ndarray[uint16]`, plus `.features()` and `.classify()`. It checks CRC and length and auto-detects the port by USB VID `0x2E8A` (works for `COMx` and `/dev/tty.usbmodem*`).
- `ml/tests/test_pico_audio.py`: test the parser on a synthetic transcript.

**Hardware check (R):** flash `audio_dump.uf2`, close any serial monitor, then run `python ml/pico_audio.py --test 2` (a CLI entry point that prints length, mean and std) from the venv. Clap during the recording.
**Done when:** length = 32000, the CRC passes, the mean matches A0, and the std rises with the clap.
**Commit:** `feat(tools): add audio_dump firmware and Mac receiver for raw 16 kHz mic recordings`

---

### A2: Dataset: ESC-50 through the Pico mic (P0)

1. `ml/prepare_esc50.py`:
   - Takes `--esc50-dir`. R downloads `https://github.com/karolpiczak/ESC-50` into `ml/data/esc50/`; print the instructions.
   - Reads `meta/esc50.csv` and `ml/label_map.yaml` (§3).
   - `--per-category N` (default 20), stratified across ESC folds.
   - Writes `ml/data/manifest.csv`: clip_id, source, category, label_id, group_id, split_key (= ESC fold), src_path.
2. `ml/record_dataset.py` (generic; Phase B reuses it):
   - For each manifest row × each `--volumes` value (default `1.0 0.5`): start `PicoAudio.record(5)` in a thread, then after 150 ms play with `playback.play(src_path, volume=vol)` (sounddevice, cross-platform). `--list-devices` / `--device` choose the output (speaker).
   - Save the raw uint16 samples as `ml/data/raw/<label>/<clip_id>_v<vol>_t<take>.npy`; this is the source of truth. Save a listenable `.wav` next to it (int16 of `(x − mean)·16`).
   - `--ambient K`: K × 5 s takes with no playback. Ask R for a mix of quiet room, outdoors and fan noise.
   - Resumable (skips existing files), prints ETA, and appends to `ml/data/recordings.csv` (file, clip_id, label_id, group_id, split_key, volume, take, mean_adc, crc_ok, ts). Has `--dry-run`.
3. Time estimate for R: 25 categories × 20 clips × 2 volumes × ~6 s ≈ **1 h 40 min** unattended, plus about 10 min ambient. Use a quiet room, a fixed speaker position, and keep the laptop charging.

**Split rule (holds everywhere):** split by `split_key`. For ESC-50, fold 5 = test, fold 4 = val, folds 1–3 = train. Every take of one source clip stays in one split. Ambient is assigned round-robin.
**Done when:** ≥ 40 recordings per label, ambient ≥ 30, all `crc_ok`. Print the counts.
**Commit:** `feat(ml): ESC-50 label mapping and automated Pico-mic dataset recorder`

---

### A3: Streaming spectral features v1 (C) (P0)

New files are pure C, in `firmware/sensing/`:

- **`sound_fft.c/.h`:** in-place radix-2 FFT, N = 256, int16 Q15 with a ≫1 scale per stage (document the 1/256 scaling). `static const` twiddle and Hann tables, generated by `tools/gen_sound_tables.py` and checked in.
- **`sound_features.c/.h`:**
  - `sf_reset`, `sf_add(sf_t*, const uint16_t*, uint32_t n)` (any chunk size; internal 256-sample frame buffer; hop 256), `sf_finish(const sf_t*, float out[SF_N_FEATURES])`.
  - `SF_VERSION 1` and a `SF_FEATURE_NAMES[]` table.
  - All state lives in `sf_t`; document its size.
  - **Per frame (16 ms):**
    - Remove the frame mean, apply Hann, then FFT and power.
    - **8 bands**: 62–250, 250–500, 500–1k, 1–2k, 2–3k, 3–4k, 4–5.5k, 5.5–8k Hz.
    - Total energy, centroid, ZCR, spectral flux.
    - Shape bands: `b_i = log(E_i+ε) − log(E_tot+ε)`.
  - **Over the capture** (Welford running mean/var only), **25 features**:
    - `band_mean[8]`, `band_std[8]`
    - `centroid_mean/std`
    - `zcr_mean/std`
    - `loud_std`, `loud_range`
    - `active_frac` (frames more than 6 dB above the quietest 10%, via a 32-bucket histogram)
    - `flux_mean`
    - `log_rms` (DC-corrected; document that it is AGC-affected)
  - Helpers for metadata: `sf_rms_dc_corrected()`, `sf_zcr_per_s()`.
- **`host/tools/sound_featx.c`:** CLI, `sound_featx [--header] file.npy …` → CSV. Add it to `host/CMakeLists.txt`.
- **`host/tests/test_sound_features.c`** (add to ctest):
  1. A 1 kHz sine → the 1–2k band dominates and the centroid is within ±100 Hz.
  2. A 6 kHz sine → the 5.5–8k band dominates.
  3. Block sizes 1, 7, 256, 1000 or 8192 give **identical** output.
  4. A +60 count DC offset changes no shape feature by more than 1e-3.
  5. Silence and clipped input give finite output.
  6. FFT vs a double naive DFT: peak bin matches, and power error < 5%.

**Commit:** `feat(sound): streaming 256-pt FFT spectral feature extractor with host CLI and tests`

---

### A4: Feature extraction + parity (P0)

1. `ml/extract_features.py` runs `build/sound_featx` over every `.npy` → `ml/data/features_v1.csv`. Use the **C binary**, never a Python re-implementation.
2. Finish the `audio_dump` `f` command.
3. `ml/check_parity.py`: 5 live takes. The `@FEAT` values must match `sound_featx` on the same samples within **1e-3 relative**. Print the max diff per feature.
4. `ml/plots.py` → `docs/sound/figures/feature_boxplots_v1.png` (per group, the 6 most informative features).

**Hardware check (R):** run `check_parity.py` and paste the output.
**Commit:** `feat(ml): feature extraction via C extractor and Pico/host parity check`

---

### A5: Two-stage model + C export (P0)

`ml/train.py` (deterministic, `random_state=0`, config `ml/train_config.yaml` with `phase: A|B`):

- **Splits:** per A2 (train folds 1–3, tune on 4, report on 5); final refit on folds 1–4.
- **Baseline:** apply the legacy `mic.c` thresholds to the same recordings (animal vs not-animal accuracy). This is the "before" number.
- **Stage 1 (group, 7 classes) candidates:**
  - DecisionTree, depth ∈ {4, 6, 8, 10}, min_samples_leaf ∈ {2, 5}
  - RandomForest, n ∈ {5, 10, 20}, depth ∈ {6, 8}
  - ExtraTrees with the same grid
  - Use class_weight="balanced".
- **Stage 2:** one model per animal group: BIRD (4 labels), MAMMAL (5) and INSECT_FROG (3). Train it only on that group's recordings, using the same candidate grid.
- **Selection rule:** for each model, pick the smallest (by total nodes) within 0.02 macro-F1 of the best on the val fold. All models together must stay < 60 kB flash.
- **Rejection threshold** `SOUND_STAGE2_MIN_CONF`: choose it on the val fold so wrong stage-2 labels are ≤ 15% of the labels it emits. Below the threshold, output the group id.
- **Report, end to end on the test fold:**
  - group accuracy and macro-F1
  - label accuracy and macro-F1
  - coverage (the % that got a finer label)
  - confusion matrices: `docs/sound/figures/confusion_group_test.png` and `confusion_label_test.png`
- **Outputs:** `ml/models/metrics_A.json` and `docs/sound/model_card.md` (data, labels, split, candidates table, chosen models, test metrics, limitations).
- **`ml/export_c.py`** is a self-written exporter in the style of Maryam's `weather_tree.h` (no emlearn or micromlgen). It writes `firmware/sensing/sound_model.h` containing:
  - flat `static const` arrays per tree: feature idx (int8), threshold (float), left/right (int16), and leaf distributions (uint8 %)
  - one predict function per model, plus `sound_classify(const float* f, uint8_t* label_id, uint8_t* conf)`, which implements stage 1 → stage 2 → rejection
  - a header comment with the training date, params, test metrics, `SF_VERSION`, and a compile-time check that the feature count matches `SF_N_FEATURES`
- **`host/tests/test_sound_model.c`** + `export_c.py --vectors` (generates `host/tests/sound_model_vectors.h` from 200 test rows with sklearn's end-to-end outputs). The C output must match **100%**.

**Stop rule:** if test group macro-F1 < 0.6, stop and discuss with R before A6. Options then: merge confusable groups, record more, or drop VEHICLE/WEATHER.
**Commit:** `feat(ml): two-stage on-device sound classifier (group then animal) exported to C with parity test`

---

### A6: Classifier on the node + bench evaluation (P0)

1. `common/sound_classes.h` + `.py` + the diff test (§3).
2. `audio_capture.c`: feed each chunk to **both** `audio_feat_add` (legacy) and `sf_add`. `sf_t` is static. Add timing for the total µs in feature code and the max µs per call.
3. `sensing.c` → `sensing_capture()`:
   - Run `sf_finish` then `sound_classify`.
   - Fill `sound_class` = label id, `sound_conf`, `sound_rms` (DC-corrected) and `sound_zcr`.
   - On failure, set class and conf to 0xFF and rms/zcr to MISSING.
   - Update the field comments in `capture_meta.h`.
4. Serial line in `main.c` and `sensortest` `m`:
   `[SOUND] group=MAMMAL label=DOG id=20 conf=82 rms=.. zcr=.. lost=0 feat_us=.. maxcall_us=..`
5. Finish the `audio_dump` `c` command.
6. Build all images and report the RAM/flash delta. **CPU budget:** total feature + model time < 25% of 5 s. Warn if any per-call time > 100 ms, since it eats into the 512 ms ring slack.
7. **`ml/bench_eval.py`** (with `sensor_test.uf2` or `audio_dump`): play every test-fold clip and parse the Pico's label. Output:
   - `docs/sound/figures/confusion_bench_*.png`
   - `docs/sound/evidence/bench_eval_A_<date>.csv`
   - on-device metrics next to offline metrics

**Hardware check (R):** run bench_eval and paste the summary. Then flash `csc09_node_2.uf2`, do one dual-trigger capture with a played dog bark, and paste the `[SOUND]` line (`lost=0` required).
**Done when:** on-device macro-F1 is within ~0.1 of offline.
**Commit:** `feat(sound): run two-stage classifier on-node, fill sound_class/sound_conf, bench evaluation`

---

### A7: Persistent capture ID + naming (P0)

**Bug:** `g_capture_id` in `main.c` lives in RAM and restarts at 0 on every reboot. After a reboot, capture #42 collides with the old #42 on the SD card and in the dashboard.

- **Persistent counter (comms only, since it owns the SD card):**
  - At boot, read `meta/next_id` (a u32 text file), scan `clips/` (from B16 on) and `outbox/` for the highest ID seen, and take whichever is larger.
  - On every capture, increment the counter and rewrite `meta/next_id`. Write a temp file and rename it, so a power cut can't corrupt it.
  - The wire field `capture_id` (u32) is unchanged.
- **Single format, put in `common/capture_id.h` + `.py`:** `N<node>-<id:06>`, e.g. `N2-000042`. Use it in the following places:
  - serial logs (`capture N2-000042: …`)
  - SD file names (`N2_000042_*`, since the dash is avoided in file names)
  - the backend `display_id` column
  - the dashboard badge
  - CSV exports
- Check that FatFs long file names are enabled (`FF_USE_LFN`) in the carlk3 config, and tell R if they're not.
- **Host test:** the ID format/parse round-trip.
- **Hardware check (R):** take 3 captures, reboot, and take 2 more. The IDs continue without restarting at 1.

**Commit:** `fix(sound): persist capture id on SD across reboots and unify N<node>-<id> naming`

---

### A8: The Pico picks the image from the sound label (P0)

- **SD layout** (document it in BRINGUP.md + HANDOFF): `samples/<prefix>_NN.jpg`, where the prefix comes from the label table. Examples: `dog_00.jpg`, `rooster_00.jpg`, `bird_00.jpg` (group fallback) and `img_00.jpg` (generic fallback, already on the cards).
- **`main.c`:**
  - At boot, list `samples/` and count images per prefix into a small static table (≤ 64 prefixes).
  - `handle_capture()` reads `m.sound_class` and walks label → group → generic, picking round-robin within the first prefix that has images.
  - Set `image_index` and append the chosen file name to the meta (the existing optional-name mechanism, ≤ 64 B total).
  - Log `capture N2-000042: heard DOG (82%) → samples/dog_01.jpg`.
- **`tools/make_label_images.py`:** takes a folder of R's own downloaded photos (`ml/images_src/<label>/*.jpg`) and writes small JPEGs to `sd_card/samples/` named `<prefix>_NN.jpg` using Pillow (cross-platform). Each must be ≤ 10 kB (max 320 px, quality stepped down until it fits). It also writes `docs/sound/IMAGE_CREDITS.md`. Tell R to use free-licence photos (Wikimedia Commons / Unsplash / Pixabay) and record the source + licence in the credits file.
- **Host test:** pure-C selection logic in `firmware/sensing/sound_image_pick.c` so it can be tested: label → group → generic fallback and per-prefix round-robin.

**Hardware check (R):** copy the images to node 2's SD card, then play a dog clip + PIR. The bridge receives `dog_XX.jpg`. Then play a rain clip and check it falls back to `weather_00.jpg`, or to `img_NN.jpg` if there is none.
**Commit:** `feat(sound): node selects capture image from on-device sound label with fallback`

---

### A9: Bridge receiver, backend, dashboard, simulator (P1)

1. `tools/bridge_receiver.py`: import names from `common/sound_classes.py`. `decode_meta` adds `sound_label`, `sound_group`, `sound_kind` and `display_id` (via `common/capture_id.py`).
2. `backend/storage.py`: add columns if missing (`ALTER TABLE … ADD COLUMN`): `sound_conf`, `sound_label`, `sound_group`, `image_name` and `display_id`, plus a **unique index on (node_id, capture_id)**. Add `/api/species` (tally by label, plus last seen and group, computed from stored captures). **No classification code in the backend.**
3. `dashboard/src`:
   - types
   - a large **`N2-000042` ID badge** on every capture card and modal
   - `CaptureFeed`/`CaptureModal`: replace "species ID: TBD" with a label chip (e.g. `DOG · 82% · MAMMAL`), or group only (e.g. `BIRD (unidentified)`), and a caption **"identified on-node"**
   - `SpeciesPage` + HomePage "Species seen" use `/api/species`
4. `simulator/demo_feed.py`: new label ids. It must stay marked SIMULATED.
5. Tests: backend pytest for decode and the species tally. `npm run build` must pass.

**Commit:** `feat(dashboard): display on-node sound labels and species tally`

---

### A10: Hardware tests T3 + T6 (P0)

Create `tools/serial_log.py`: timestamped serial logger → `docs/sound/evidence/<test>_<date>.log`, with `--send` for scripted keys. Record the results in `docs/sound/TESTS.md` (ID, setup, expected, measured, pass/fail, evidence file).

- **T3: Audio under load.**
  - Setup: node 2 captures while node 3 runs back-to-back transfers relayed via node 2 (`c` repeatedly, plus `d` for 30% loss to force retries), WiFi + LoRa + SD active.
  - Run ≥ 3 captures per group via playback.
  - Pass: `lost=0` on every capture, the same label as the idle bench run, and max per-call time ≪ 512 ms.
  - µT-Kernel half: run it on the branch that has `firmware_smp` and check that `sources.mk` picks up the new `firmware/sensing/*.c` and `common/sound_classes.h`.
- **T6: Capture queue full.**
  - Add console key `H` to `main.c` to toggle `g_hold_comms` (comms skips draining `g_capture_q`; radios still poll).
  - Force 6 captures ≥ 10 s apart.
  - Pass: 4 queued, `capture queue full, dropped` ×2, and `[ENV]` timestamps keep advancing (the sensors task never blocks). Releasing hold sends all 4.

**Commit:** `test(sound): T3 audio-under-load and T6 queue-full evidence` (+ `feat(tools): serial_log.py` separately)

---

### A11: Docs v1 + demo script (P0)

- **`docs/sound/SOUND.md` v1:**
  - the Pico-first design and diagram
  - why spectral shape (AGC, DC)
  - why trees (no FPU, flash, explainable; matches the weather tree)
  - two stages + rejection
  - dataset/split, model selection table
  - offline vs bench confusion matrices
  - CPU/RAM/flash numbers
  - image selection
  - limitations: ESC-50 ≠ Singapore wildlife; playback ≠ field
- **`docs/sound/DEMO.md`:** a 3-minute script with fallbacks (simulator, recorded evidence logs).
- **README:** replace "it does not identify wildlife species" with an accurate one-liner + a link.

**Commit:** `docs(sound): Phase A design, results and demo script`

---

# PHASE B: SUBMISSION

### B1: Singapore species list + dataset (P0)

1. **Species list:** propose 6–10 candidates to R and **ask R to confirm**. Criteria: loud, distinctive, common on Singapore trails, and energy mostly below about 6 kHz (16 kHz sampling). Suggested starting list:
   - **birds:** Asian koel, spotted dove, yellow-vented bulbul, collared kingfisher, Oriental magpie-robin, house crow, red junglefowl
   - **frogs:** banded bullfrog, Asian common toad
   - **optional:** cicada, long-tailed macaque
   Add them to the label table as ids 64+ with group, image prefix and scientific name.
2. **`ml/fetch_xc.py`:**
   - Download from xeno-canto (check the current API; v3 needs a free API key, so ask R to register and put it in `ml/.env`, which is gitignored).
   - Query per species, quality A/B preferred, ≤ 40 recordings, **≥ 8 distinct recordists** where possible.
   - Save the mp3 and metadata (id, recordist, licence, country, length) to `ml/data/xc/`.
   - Prefer Singapore/Malaysia recordings; note the country mix.
   - Fallback if the API is blocked: R downloads manually and the script ingests a folder + CSV.
3. **`ml/segment_xc.py`:**
   - mp3 → 16 kHz mono WAV (`soundfile` + `scipy.signal.resample_poly`; ffmpeg only as an optional fallback).
   - Pick up to 3 non-overlapping **5 s windows with the highest call energy** per recording (band-limited 0.5–6 kHz).
   - Write `ml/data/xc_manifest.csv` with `split_key = recordist`.
   - Write a listening sheet `ml/data/xc_review.csv` and ask R to spot-check about 10% by ear and mark bad windows. Excluded windows are skipped.
4. **"OTHER" class:** non-target sounds (ESC-50 non-animal + other animals + ambient). Stage 2 learns to say "not one of my species".
5. Record everything through the Pico mic with `record_dataset.py`, at volumes 1.0 and 0.5, and print the ETA.
6. **Split by recordist** (`GroupShuffleSplit` / `GroupKFold`): no recordist appears in both train and test. Explain in the model card why this matters (otherwise the model learns each recordist's microphone instead of the animal).
7. Licences → `docs/sound/DATA_CREDITS.md`.

**Commit:** `feat(ml): Singapore species list, xeno-canto fetch/segment pipeline with recordist split`

---

### B2: Features v2 (P0)

Extend `sound_features` to `SF_VERSION 2` (v1 output stays available behind a flag so Phase A results remain reproducible).

- **24 mel-spaced bands** from the 128 FFT bins (static mapping table). Keep per-band mean/std of the normalised log energy.
- **Call rhythm:**
  - Store the per-frame log-energy envelope as a uint8 (312 frames ≈ 312 B).
  - At the end, compute the envelope autocorrelation over lags 32 ms–1.5 s → `rhythm_period_ms`, `rhythm_strength`, and `onset_rate` (onsets per second).
- **Pitch contour:** dominant bin per active frame → `pitch_mean_hz`, `pitch_std`, `pitch_slope_mean` (rising/falling) and `pitch_range`.
- **Texture:** spectral flatness mean/std, and band-to-band temporal correlation of 2–3 key bands.
- Keep the total around 60–80 features. Update `sound_featx`, the parity check and the host tests, adding:
  - a synthetic repeating chirp at 2 Hz → `rhythm_period_ms` ≈ 500 ± 32
  - a rising sweep → positive pitch slope
- Rerun `check_parity.py` on hardware.

**Commit:** `feat(sound): features v2 with mel bands, call rhythm and pitch contour`

---

### B3: On-device species model with rejection (P0)

- **Stage 1:** unchanged groups. **Stage 2 (BIRD and INSECT_FROG):** the Singapore species + the Phase A animal labels + OTHER.
- **Candidates:** the same tree/forest grid, plus forests up to n = 30 and depth 10 if flash allows (total model budget < 120 kB flash).
- **Rejection, calibrated on the val split:**
  - choose `SOUND_STAGE2_MIN_CONF` for a target species precision ≥ 0.8
  - report the coverage at that threshold
  - plot the precision–coverage curve → `docs/sound/figures/rejection_curve.png`
- **Metrics on the recordist-held-out test:**
  - per-species precision, recall and F1
  - macro-F1
  - the confusion matrix
  - **also on-device via `bench_eval.py`**
- Export, parity test (100% match) and node integration, the same as A5/A6. Update the model card (Phase B section).

**Commit:** `feat(ml): on-device Singapore species classifier with calibrated unknown rejection`

---

### B4: Resource + latency table (P0)

Measure on hardware and fill a table in `docs/sound/RESOURCES.md`:
- flash (code + model) and RAM (static + stack high-water of the sensors task via `uxTaskGetStackHighWaterMark`)
- feature µs per frame (mean/max), model µs per inference, and CPU % during capture
- lost samples (idle and under load)
- before/after Phase A → B

Also rerun T3 with v2, and use `perf(sound):` commits for any optimisation. If the CPU budget is exceeded, optimise in this order:
1. fixed-point mel accumulation
2. skipping the rhythm autocorrelation for quiet captures
3. a lower FFT scaling cost

**Commit:** `test(sound): resource/latency table and T3 rerun on features v2`

---

### B5: Sound-gated sending (P0, needs Maryam's OK)

The Pico decides whether a capture is worth radio airtime.

- **Policy table** in `firmware/sensing/sound_gate.c` (pure C, host-tested). Inputs: the label, its group and confidence, plus the existing trigger flags. Output: `SEND_IMAGE`, `SEND_META_ONLY` or `SKIP_COUNT_ONLY`. Default policy (compile-time table, documented):

  | heard | action | 1.5 s highlight clip (B18) |
  |---|---|---|
  | target species / animal label, conf ≥ threshold | SEND_IMAGE | sent |
  | animal group, unidentified | SEND_IMAGE (lower priority, P2: send after others) | sent |
  | HUMAN / VEHICLE / WEATHER / AMBIENT | SKIP_COUNT_ONLY (counted in B6 summaries) | not sent (the full 5 s still stays on SD) |

  `SOUND_GATE_ENABLE` defaults to 1 and can be switched off for comparison.
- **META_ONLY:** use the smallest legal ALFP file. Comms writes `events/eNNNNN.evt` (the 27-byte meta as the file body) and queues it. Confirm with R that this needs no ALFP changes (it shouldn't, since it's just a tiny file).
- **Optional (with Maryam):** fill `trigger_trust` from sound confidence × PIR. Only if she agrees, since it's her field.
- **Experiment** `ml/gate_experiment.py`:
  - Play a scripted mixed sequence (e.g. 40 events: 50% animal, 50% human/vehicle/weather) with PIR held, gate ON vs OFF.
  - Log transfers, bytes and fragments, and estimate LoRa airtime using the method in `docs/results/e6_size.csv`.
  - Output `docs/sound/evidence/gate_<date>.csv` and `docs/sound/figures/gate_airtime.png`.
  - Report the % airtime saved and how many animal events were wrongly skipped (cost of the gate).

**Commit:** `feat(sound): on-device sound-gated transmission policy with airtime experiment`

---

### B6: On-device activity summaries (P1)

- `firmware/sensing/sound_activity.c` (pure C): counters per label id per period, plus trigger stats (pir_only, sound_only, both, gated_skipped).
- Every `SOUND_SUMMARY_PERIOD_MIN` (default 15), comms writes `summary/sNNNNN.bin` and queues it.
  - Format v1: `"SUM1" | u8 node | u32 period_start_uptime_s | u16 period_s | u8 n | n × (u8 label, u16 count) | trigger stats`.
  - Document it in `common/sound_summary.h` + a Python decoder in `common/sound_summary.py`, with a host test that checks they agree.
- There is no RTC, so periods are uptime-based. The backend maps them to wall-clock using the arrival time and records the approximation in the docs.
- Backend: store summaries (new table) + `/api/activity?node=&hours=`. Decode only.
- Measure: bytes sent per hour, summaries vs one image per event.

**Commit:** `feat(sound): on-device per-species activity summaries sent as compact files`

---

### B7: Acoustic trigger on the ADC + pre-trigger (P1, needs Maryam's OK)

- **Refactor `audio_capture` into a continuous stream:**
  - `audio_stream_start()`, `audio_stream_poll()` (called every 10 ms) and `audio_capture_from_stream(seconds, pretrigger_ms, …)`.
  - The DMA ring runs continuously. Each poll computes the energy of the newest 160 samples.
  - The capture begins from up to `pretrigger_ms` (default 300, must be ≤ ring slack − margin) **before** the trigger.
- **Adaptive trigger:**
  - noise floor = slow EMA of log energy (τ ≈ 10 s)
  - fires when energy > floor + `K` dB (default 12) for ≥ `M` consecutive polls (default 2)
  - with a hold-off
- **ADC sharing with the light sensor:** the sensors task stays the only ADC owner. For each light read it pauses the stream (`adc_run(false)` → select ADC1 → read → reselect ADC0 → resume) and counts the gap in samples. Document the gap.
- **`SOUND_TRIGGER_SOURCE`** = `GPIO14` (current), `ADC` or `EITHER`, default unchanged until Maryam agrees.
- **Experiments** (`ml/trigger_experiment.py`):
  - **Short-sound hit rate:** play 100/200/300 ms bursts with PIR held; GPIO14 vs ADC. This is the evidence for FR1/A4b.
  - **False triggers:** 10 min ambient each, indoor and outdoor.
  - **Onset kept:** with pretrigger the first 300 ms of the call is in the features; show classification accuracy with vs without pretrigger on short calls.
  - **Cost:** CPU % idle. Power is optional: measure with a USB power meter if R has one, otherwise report CPU/DMA duty only. **Do not ask R to buy hardware.**

**Commit:** `feat(sound): adaptive acoustic trigger on the mic ADC with pre-trigger buffer`

---

### B8: Field validation (P0)

- **Plan session(s) with R:** 30–60 min at a Singapore park/reserve (e.g. Singapore Botanic Gardens, Bukit Timah, Sungei Buloh). Use one node on a power bank, running `sensor_test` or the node firmware with a logging build.
  - `field_log` firmware option: every capture appends one CSV line to the SD card (`field/log.csv`: uptime, label, conf, top-3 stage-2 scores, rms) and, if `FIELD_KEEP_AUDIO=1`, the raw 5 s audio to `field/aNNNN.raw`. The SD card is only touched when comms is idle, or this runs in the `sensor_test`-style build with no radios.
- **Ground truth:** R runs the **Merlin Bird ID** app (Sound ID) next to the node, takes a phone screen recording, and notes timestamps. Then `ml/field_compare.py`:
  - aligns both logs by time (R syncs the clocks at the start by clapping at a known phone time; the clap is visible in both)
  - for raw audio kept, R can also label by ear
  - outputs per-species detections Pico vs Merlin, precision/recall, and examples of misses and confusions
- **Report honestly:** field results will be lower than bench results. Explain why (distance, overlapping calls, wind, AGC).

**Commit:** `test(sound): field validation against Merlin Bird ID with alignment tooling`

---

### B9: Evaluation package (P1)

`ml/evaluate.py` → `docs/sound/EVALUATION.md` with figures:
- **Before vs after:** legacy thresholds vs Phase A vs Phase B, on the same test set.
- **Offline vs bench vs field.**
- **Feature ablation:** retrain without each feature family (bands, rhythm, pitch, texture) and report the macro-F1 drop.
- **Rejection curve** (from B3) and **volume robustness** (accuracy at 0.5 vs 1.0 playback, showing AGC).
- **Recordist split vs random split:** show the inflated random-split score to justify the method.

**Commit:** `docs(ml): evaluation package with ablation, before/after and robustness analysis`

---

### B10: Tiny int8 CNN vs forest (P2)

- **Input:** a 24-mel × ~78-frame log-mel patch (64 ms hop, from the v2 frame pipeline; it fits in about 2 kB int8 if kept).
- **Model:** 2 conv blocks + global average pool + dense, trained in Keras on the Mac (`ml/cnn/`), int8 post-training quantisation, ≤ 20 kB weights.
- **Inference:** **hand-written int8 C kernels** in `firmware/sensing/sound_cnn.c` (rule 7: no TFLM). Host parity vs the TFLite interpreter on 200 vectors (argmax agreement ≥ 99%).
- **Compare with the forest:** macro-F1, flash, RAM, latency and CPU %. Pick the deployed model by the evidence, and document the decision either way.

**Commit:** `feat(sound): int8 CNN classifier with hand-written kernels and forest comparison`

---

### B11: Spectrogram fingerprint (P2)

- The Pico pools the capture into a **32 time × 16 band × 4-bit** fingerprint (256 B) while streaming.
- Send it as a tiny `fp/fNNNNN.bin` file **only when the gate says SEND_IMAGE**. It is 2 fragments; quantify the airtime.
- Backend: decode only. Dashboard: render it as a small heatmap next to the image, captioned "what the node heard".

**Commit:** `feat(sound): on-node spectrogram fingerprint sent with captures`

---

### B12: Dashboard for Phase B (P1)

- Species page:
  - per-species cards with Singapore common + scientific name, count, last seen and node
  - "identified on-node" badge
  - confidence distribution
- Activity chart from `/api/activity` (B6).
- Gate stats panel: sent / skipped / airtime saved (B5).
- Fingerprint heatmap (B11).
- `npm run build` must pass; demo_feed stays SIMULATED-marked.

**Commit:** `feat(dashboard): species, activity and gating views for on-node analytics`

---

### B13: T5 merged vs split sensor task (P1, with Maryam)

- **Instrument the merged design:** max gap between `sensing_env_poll` calls, trigger → capture latency and audio loss. With B7, also measure poll jitter of the stream.
- The **split prototype** (`SENSING_SPLIT_TASK`) gives the ADC/DMA stream its own task, and the light read requests ADC time via a FreeRTOS mutex or message. Build it only if the merged data shows a real problem; otherwise argue from the data.

**Commit:** `test(sound): T5 merged vs split sensor task measurements`

---

### B14: One-command retrain + final regression (P0)

- `ml/pipeline.py` (Python, cross-platform) runs: features → train → export → vectors → host tests, for `PHASE=A|B`. Re-running it on the same data must give identical `sound_model.h` (check with `git diff --exit-code` printed for R, not run as a commit).
- Full regression (rule 4) + firmware build of all targets + the SMP build if available + a final bench_eval.

**Commit:** `chore(ml): reproducible one-command retrain pipeline and final regression`

---

### B15: Final docs + report material (P0)

- **`docs/sound/SOUND.md` final:** everything from A11 + Phase B:
  - species, features v2, rejection
  - gate, summaries, acoustic trigger
  - **audio clips**: full 5 s on SD + 1.5 s highlight sent; RAM, airtime and comms-loop numbers
  - **flag/review loop** and the shared ID scheme
  - field results, resources, evaluation, CNN decision
  - limitations and future work. Future work includes sending the full clip over WiFi (needs an outbox change) and a downlink so a flag can request the full clip.
- **`docs/sound/REPORT_SNIPPETS.md`:** paste-ready text for the team report:
  - **FR1** (if B7 adopted): acoustic trigger
  - **FR2**: image chosen from the on-device label
  - **FR8**: on-device species ID with rejection
  - new **FR**: sound-gated sending and activity summaries
  - **NFR9** update
  - assumption **A3** + **A4b** updates
  - test rows T3/T5/T6 + the new experiments
  - **fault table rows**: no DMA channel, lost samples > 0, model output below threshold, SD image missing for label, summary write fail, ADC contention
  - ring/queue sizes + overflow behaviour
  - the **Buddy 3 AI-use evidence row** built from `AI_LOG.md`
- **`docs/sound/HANDOFF.md`** final:
  - **all nodes:** reflash together; SD cards need `samples/<prefix>_NN.jpg`
  - **Shreya:** DB clear, new tables, `main.c` changes
  - **Maryam:** trigger source, gate, `trigger_trust`
  - **Goutham/Mridula:** airtime reduction numbers
- Final summary for R: done / not done / skipped P2 items and why.

**Commit:** `docs(sound): final design, evaluation, report snippets and teammate handoff`

---

### B16: Full 5 s clip streamed to SD (P0)

Only comms may touch the SD card (shared SPI1), so the sensors task **streams** the compressed audio to comms in small chunks.

- **`firmware/sensing/sound_clip.c/.h`** (pure C): IMA-ADPCM 4-bit encoder fed from the same `sf_add` chunks (16 kHz → 8 kB/s → ~40 kB per 5 s).
  - Every **0.5 s block boundary**, record `{byte_offset, predictor, step_idx, block_energy}` in a 10-entry table (a few bytes each).
  - **`.scl` format** (document it in the header):
    ```
    "SCL1" | u8 ver | u8 node | u32 capture_id | u16 rate | u32 n_samples | i16 predictor | u8 step_idx | u8 flags(bit0 incomplete, bit1 highlight) | nibbles…
    ```
- **Chunk pool:** `SOUND_CLIP_CHUNKS` × 1 kB (default 4, so ~4 kB RAM) with a FreeRTOS queue of chunk indices (sensors → comms) and a free-list (comms → sensors).
  - Sensors **never blocks**: if no free chunk is available, it sets `incomplete`, stops clip encoding for this capture, and counts `clip_overflow`. Classification continues unaffected.
  - Comms appends each chunk to `clips/N<node>_<id>_full.scl` (header written first, `n_samples` + flags patched at the end).
- **Rules:** `SOUND_CLIP_ENABLE` (default 1). A cap of `SOUND_CLIP_MAX_FILES` (default 2000) deletes the oldest full clips (P2).
- **Measure:**
  - extra comms work per capture: the **worst-case comms loop time with clips on vs off**, logged against the ~175 ms UART and ~31 ms LoRa margins (reviewer's key risk 1)
  - `clip_overflow` count
  - RAM delta
- **Tests:**
  - `host/tests/test_sound_clip.c`: encode → C decode, SNR > 20 dB on tones; block table correct with arbitrary chunk sizes; overflow sets `incomplete`
  - `ml/tests/test_scl.py`: the Python decoder matches the C decoder on a vectors file

**Hardware check (R):** take 3 captures, copy `clips/` off the SD card, run `tools/scl2wav.py` (stub it in this task if B21 isn't done), and listen. Paste the comms-loop timing.
**Commit:** `feat(sound): stream full 5 s ADPCM clip from sensors task to SD via comms chunk pool`

---

### B17: Loudest 1.5 s highlight (P0)

- At the end of a capture, sensors picks the **3 consecutive 0.5 s blocks with the highest summed energy**. It sends comms `{start_byte, n_bytes, predictor, step_idx}` with the capture request (extend the request struct; `app_request_capture()` stays compatible for Maryam).
- Comms copies that byte range from the full file into `clips/N<node>_<id>_hl.scl`, with its own header (`highlight` flag + its own predictor/step), in 512 B pieces to keep each SD op short. **No extra RAM buffer.**
- **Host test:** the highlight decoded on its own matches the same samples decoded from the full clip, and the loudest window is chosen correctly.

**Commit:** `feat(sound): cut loudest 1.5 s highlight from the stored clip without extra RAM`

---

### B18: Send the highlight (P0)

- After queueing the image job (B5 decides), comms queues the highlight with `alfp_outbox_add("clips/…_hl.scl", meta27 + name)`. The name ends in `_hl.scl`, which marks it as a clip. **No ALFP or outbox changes**: the outbox picks whichever radio currently reaches the bridge.
- In `on_tx_done`, if the result is OK and the path ends with `_hl.scl`, delete it. **The full clip is kept.** If `alfp_storage_t` has no remove op, add the smallest possible one and note it in HANDOFF for Shreya.
- **Integration:** extend `host/integration/run_integration.sh` (and add a `run_integration.py` equivalent per rule 12) so one capture → 2 verified files at the bridge (image + highlight).
- **Airtime:** measure highlight fragments/airtime on WiFi and LoRa (same method as `docs/results/e6_size.csv`), with clips on vs off and with the gate on vs off.

**Hardware check (R):** node 2 + bridge. One capture → the bridge log shows 2 `rx from node 2` lines; `_hl.scl` disappears from node 2's SD card and `_full.scl` remains.
**Commit:** `feat(sound): send 1.5 s highlight as second ALFP file and clean up after delivery`

---

### B19: Backend: clips, flags, export (P0)

1. `tools/bridge_receiver.decode_meta`: add `kind` = `clip_highlight` when the name ends with `_hl.scl`, otherwise `image`. Fill `display_id`.
2. **`backend/sound_clip.py`:** `decode_scl(bytes) -> (header, int16 np.ndarray)`, `to_wav_bytes()`, and `spectrogram_png()` (matplotlib, cached to disk).
3. **Storage:**
   - `clips` table (node_id, capture_id, display_id, wav_path, seconds, full_clip_sd_path, received_at). Join to the capture by (node_id, capture_id) in **either arrival order** (a pending row if the clip arrives first).
   - `flags` table (display_id, flagged bool, note, tags json, reviewed_label_id nullable, reviewer, updated_at).
4. **API:**
   - `GET /api/captures/{id}/clip.wav` and `/spectrogram.png`
   - `PUT /api/captures/{id}/flag` {flagged, note, tags}
   - `PUT /api/captures/{id}/review` {label_id}, validated against `sound_classes.py`
   - `GET /api/flagged?node=&label=&tag=&from=&to=`
   - `GET /api/flagged.csv` with columns display_id, node, time, pico_label, pico_conf, reviewed_label, note, tags, full_clip_sd_path
   - `GET /api/reviewed/export` writes `ml/data/reviewed/<display_id>.wav` + `labels.csv` for retraining (the B14 pipeline can include it with `--with-reviewed`)
   - WS events `capture_updated` and `flag_updated`
5. **Rule 9:** the backend never classifies. A review is a human label, stored separately from the Pico's label.
6. **pytest:** decode, joins in both orders, flag/review endpoints, CSV columns, and an invalid label id rejected.

**Commit:** `feat(backend): store and serve sound clips, flag/review API and CSV export`

---

### B20: Dashboard: listen, flag, review (P0)

- **Every capture card/modal:**
  - **`N2-000042` badge**, image, and the Pico's label + confidence ("identified on-node")
  - `<audio controls>` + spectrogram, with the caption *"1.5 s highlight · full 5 s on node 2 SD: `clips/N2_000042_full.scl`"*
- **Flag panel:**
  - ⭐ flag toggle, note, tags (preset: unusual call, possible new species, wrong label, poor audio; plus free text)
  - "Correct label" dropdown (from the label table)
- **Flagged page** (reuse or replace `ReviewPage.tsx`):
  - filters (node, label, tag, date) and the list with ID badges and players
  - **Export CSV** button and a "Copy SD paths" button
- Live updates via WS. `npm run build` passes. demo_feed may attach a synthetic clip, but it must stay marked SIMULATED.

**Commit:** `feat(dashboard): clip playback, spectrogram, flag and review workflow with matching node IDs`

---

### B21: SD tools + field workflow (P1)

- **`tools/scl2wav.py`:**
  - `scl2wav.py <file|folder> [--out DIR]` converts `.scl` → `.wav`, keeping the `N2_000042` names
  - `--from-csv flagged.csv --sd <card path>` (`/Volumes/<card>` or `E:\`) converts only the flagged recordings
  - `--index` writes a CSV of everything on a card (ID, seconds, incomplete flag)
- **`docs/sound/FIELD_WORKFLOW.md`:** export the flagged CSV → go to the node → copy `clips/` off the SD card → run `scl2wav.py --from-csv` → listen and analyse. Include both macOS and Windows commands.

**Commit:** `feat(tools): scl2wav converter and field workflow for retrieving flagged recordings`

---

## 5. QUICK REFERENCE (macOS + Windows)

| Task | macOS | Windows (PowerShell) |
|---|---|---|
| Find Pico port | `python -m serial.tools.list_ports -v` | same |
| Serial monitor (close before running scripts) | `python -m serial.tools.miniterm /dev/tty.usbmodemXXXX 115200` | `python -m serial.tools.miniterm COM5 115200` |
| Activate venv | `source ml/.venv/bin/activate` | `ml\.venv\Scripts\Activate.ps1` |
| Set SDK env vars | `export PICO_SDK_PATH=$PWD/deps/pico-sdk` | `$env:PICO_SDK_PATH="$PWD\deps\pico-sdk"` |
| Build firmware | `cmake -S firmware -B build-fw && cmake --build build-fw -j4` | Pico VS Code extension (Compile), or the same command in WSL |
| Flash | hold BOOTSEL, plug in, `cp build-fw/<t>.uf2 /Volumes/RPI-RP2/` | hold BOOTSEL, plug in, drag the `.uf2` onto the **RPI-RP2** drive |
| Host tests | `cmake -S host -B build && cmake --build build -j && ctest --test-dir build` | same (MSVC/MinGW), or in WSL |
| Smoke test the tools | `python ml/smoke_test.py` | `python ml\smoke_test.py` |
| Backend bridge port | `SMARTRELAY_BRIDGE_PORT=/dev/tty.usbmodemXXXX uvicorn app:app` | `$env:SMARTRELAY_BRIDGE_PORT="COM5"; uvicorn app:app` |
| SD card path for tools | `--sd /Volumes/<card>` | `--sd E:\` |
