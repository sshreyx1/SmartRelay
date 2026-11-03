# Tribunal impact: the `sound` branch vs curveball (2e89e98)

What this branch changes for the rest of the system, checked on 2026-11-03 at `f0d020a`. The aim is to show
the reviewers that the on-node sound classifier went in without breaking the radio, mesh, transfer or curveball
work. Every claim below was checked with a command, listed at the end.

## 1. Summary

- **Radio, mesh, ALFP, uplink and the SMP port are byte-for-byte unchanged.**
- **The 27-byte capture record keeps its wire layout.** Two fields change *meaning*
  (`sound_class`, `sound_rms`), so **every node must be reflashed** (section 4).
- **The FreeRTOS console keys are unchanged** (`c e s d t h`); one test key, `H`, is added for T6. The curveball T6/T8 keys `x` and `z` exist only in the
  **SMP firmware** (`firmware_smp/app/main_smp.c`), both before and after this branch (section 5).
- **All 7 original host tests pass** (section 6).
- **The SMP build fails exactly as it did on curveball.** This branch adds no new error, and its 5 new C files compile
  cleanly under the SMP toolchain (section 7). `main_smp.c` would need the capture-ID and image-pick changes before it
  is used with this backend (section 8).

## 2. Files changed outside `firmware/sensing/`, `ml/` and `docs/sound/`

| file | what changed | why |
|---|---|---|
| `.gitattributes` | new: line endings and binary types, one pattern per line | Windows and macOS teammates get the same files (`.npy`, `.jpg` binary) |
| `SOUND_PLAN.md` | new: the plan, rules and Status Board | task tracking |
| `common/sound_classes.h/.py` | new: the label table (ids 0–6 groups, 16–27 animals, 0xFF unknown) as an X-macro, plus a Python twin | one source of truth for node, tools, backend and dashboard |
| `common/capture_id.h/.py` | new: `N2-000042` display ids and `N2_000042` file stems | the same id on serial, SD, bridge and dashboard |
| `common/capture_meta.h` | **comments only** (fields 20–22 describe the new meaning) | see section 4 |
| `firmware/app/main.c` | persistent capture id (`meta/next_id`, temp file + rename, restore by scanning `outbox/` and `clips/`); image chosen from the sound label (`sip_*`, label → group → `img`); image name appended to the meta tail; `[SOUND]` line; `[AUDIO]` line renamed "legacy mic.c"; capture lines print `N2-000042` | A7, A8, A6 |
| `firmware/CMakeLists.txt` | new `sound` static library (5 files, `-Wall -Wextra -Werror`); `sensing` links it and `hardware_timer`; new `audio_dump` target | A3/A6; A1 recorder |
| `firmware/audiodump/audiodump_main.c` | new: records 1–5 s of raw mic into a 160 kB buffer and sends it as base64 + CRC32; also `@FEAT` and `@CLS` | A1: training data and parity, no FreeRTOS, radios or SD |
| `firmware/sensortest/sensortest_main.c` | `q`/`m` go through `sensing_record_sound` and print `[SOUND]` | A6: H1 checks the classifier on the bench |
| `host/CMakeLists.txt` | `SOUND_ONLY` option, MSVC branch (no sanitizers), `include(sound.cmake)` | Windows teammates can build the sound tests |
| `host/sound.cmake` | new: 6 sound tests and the `sound_featx` CLI, with `-ffp-contract=off` | host = Pico arithmetic |
| `host/tests/test_sound_*.c`, `sound_model_vectors.h`, `test_capture_id.c/.py`, `test_sound_classes.py` | new tests | A3/A5/A7/A8/A2 |
| `host/tools/sound_featx.c` | new: features from raw samples, the same C as the node | training features always come from the node's code |
| `host/integration/integ_node.c` | the relay stops on SIGINT/SIGTERM and prints its stats; WiFi stats lines add `filtered N` | DEMO.md multi-hop counters; additive, both integration tests pass |
| `tools/bridge_receiver.py` | `decode_meta` names the node's label from `sound_classes.py`, adds `sound_label/group/kind` and `display_id`; the old 3-name `SOUND_NAMES` is removed | A9; the backend never classifies |
| `tools/demo_multihop.py` | new: E → C → A with the real `wifi_mesh.c` + ALFP as 3 local processes | DEMO.md segment 3 |
| `tools/gen_sound_tables.py`, `make_label_images.py`, `fetch_label_images.py` | new generators | FFT/window tables; ≤ 10 kB label photos plus credits |
| `backend/storage.py` | new columns, migration, partial unique index `(node_id, capture_id) WHERE is_demo = 0`, upsert, `species_tally` | one row per real capture (section 4) |
| `backend/app.py` | `GET /api/species`; `with_image_url()` used by REST, live push **and** the WS snapshot | A9; also fixes a curveball bug: no images after a page reload |
| `backend/tests/test_sound_labels.py` | new: 5 tests | decode, upsert, tally |
| `simulator/demo_feed.py` | new label ids, `--label-images`, a real "SIMULATED" placeholder JPEG, ids continue across runs | dashboard demo; every card is still tagged SIMULATED |
| `dashboard/src/**` | id badge, label chip, "identified on-node" panel, species pages | A9 |
| `docs/BRINGUP.md` | SD layout (`samples/<label>_NN.jpg`, `meta/next_id`) and the new log lines | bring-up instructions match the firmware |
| `sd_card/samples/*.jpg` | 35 new label images (≤ 10 kB, credited in `docs/sound/IMAGE_CREDITS.md`) | A8 |

## 3. Confirmed unchanged

`git diff --stat 2e89e98 HEAD -- <path>` is empty for:

| area | paths |
|---|---|
| ALFP | `alfp/` (src and include) |
| mesh abstraction | `mesh/` |
| radios | `third_party/lora_mesh/`, `third_party/wifi_mesh/`, `firmware/app/radios.c` |
| storage | `firmware/app/storage_fatfs.c` |
| uplink (bridge → laptop serial format) | `common/uplink.c`, `common/uplink.h` |
| curveball SMP port | `firmware_smp/` (app, buildglue, rtos_port), `third_party/mtk3smp_rp2040` (same submodule commit `900e863`) |
| team sensor drivers | `third_party/sensors/` (including `mic.c/.h`), `firmware/sensing/audio_features.*` |

## 4. The capture record (`capture_meta.h`, 27 bytes, v1)

- **Layout unchanged.** Same offsets, sizes and version byte; the diff touches comments only.
  `bridge_receiver.decode_meta` still unpacks `<BBIIBhHHBBBBBHHB`.
- **The meaning of two fields changed:**
  - `sound_class` (byte 20) was `mic.c`'s 3-value enum (0 AMBIENT_NOISE, 1 LOUD_ANIMAL_CALL,
    2 HIGH_FREQ_RUSTLE) and is now a label id from `common/sound_classes.h`.
  - `sound_rms` (bytes 22–23) is now measured around the capture's own mean instead of around 2048.
- **Consequence: a node still running a curveball image** that sends 1 would be shown as BIRD, and 2 as MAMMAL.
  **Reflash every node** (HANDOFF.md says so).
- **The optional tail after byte 27** (until now only the LoRa photo test's file name) now carries the image the node
  chose, e.g. `dog_01.jpg`. The whole meta stays ≤ 64 B, within ALFP's limit.
- **The backend upsert interacts with old firmware.** A real `(node, capture_id)` that is already stored is updated in
  place, which is how a re-sent capture stays one row. A node whose ids restart at 1 after a reboot (any curveball
  FreeRTOS image, and `main_smp.c`, section 8) would therefore **overwrite** its earlier captures on the dashboard.
  This branch's FreeRTOS firmware keeps ids on the SD card, so the issue only affects un-flashed nodes.

## 5. Console keys, and where `x` and `z` went

- **`firmware/app/main.c` (FreeRTOS, the firmware we flash).** The keys are `c e t h ? s d` at curveball
  (`git show 2e89e98:firmware/app/main.c`, lines 372–392) and now, with the same `print_help()` text. Unchanged.
- **`x` (T6: fire 5 captures fast) and `z` (T8: stop draining the console for 5 s)** exist only in
  `firmware_smp/app/main_smp.c:541-547`, both at curveball and now. README.md's "Serial commands on a curveball node"
  describes that SMP firmware. They were **never in the FreeRTOS firmware**, so this branch removed nothing.
- **Because the SMP firmware does not build (section 7), no runnable firmware has `x` or `z` today.** So A10
  **adds** one key to the FreeRTOS `main.c`: `H` toggles a hold on the capture queue for test T6. No existing key
  changed; `print_help()` lists it (`docs/sound/TESTS.md`).

## 6. Original host tests

The 7 tests that existed at curveball (`git show 2e89e98:host/CMakeLists.txt`) all pass on this branch:

| test | result |
|---|---|
| units | passed (0.5 s) |
| audio_features | passed |
| transfer | passed (0.5 s) |
| uplink | passed |
| uplink_python (runs `tools/test_bridge_receiver.py` on the C uplink stream) | passed |
| lora_mesh_integration (20% loss) | passed (23.6 s) |
| wifi_mesh_integration (20% loss) | passed (14.8 s) |

The full suite is 13/13: those 7 plus 6 sound tests. One unexplained failure was seen once earlier tonight and never
reproduced in 8 further integration runs. It is noted as a risk.

## 7. SMP build: same status as `docs/curveball_findings.md`

**How it was checked.** `build_smp.sh` modifies the submodule's working tree (it overlays the app and patches
`config.h`), so it was run on **scratch copies**: one of this branch and one of `git archive 2e89e98`, each with a
pristine copy of the submodule. Neither the repo nor the submodule was touched. Toolchain: arm-gcc 15.2 from the Pico
extension, `PICO_SDK_PATH=deps/pico-sdk` (2.1.1). Both `SMP=1` and `SMP=0` were built, with `NODE_ID=2`,
`CONSOLE=usb_cdc` and `WIFI=none`.

**Two environment problems** stop the build on macOS before any SmartRelay code is reached:
1. `build_smp.sh` uses GNU `sed -i 's/\s\+…/'`. macOS BSD sed treats the expression as a backup suffix.
   A shim was used for the check.
2. macOS ships GNU Make 3.81. It picks the kernel's generic `mtkernel_3/%.o` rule (no `mkdir -p`) over the USB rule,
   so `.d` files cannot be written. Object folders were pre-created for the check. Neither is a SmartRelay code
   problem, but they mean `build_smp.sh` currently only runs on Linux.

**Result: identical on both trees, for `SMP=1` and `SMP=0`:**

| | curveball 2e89e98 | this branch |
|---|---|---|
| SmartRelay sources compiled (`sources.mk`) | 14, 0 warnings | 19, 0 warnings (the 14 + `sound_classify/features/fft/tables/image_pick.c`) |
| `main_smp.c` | `fatal error: alfp.h: No such file or directory` | same |
| `rtos_port_mtk3.c` | `syslib.h:125: conflicting types for 'size_t'` | same |
| link | not reached | not reached |

- **Why it fails:** the kernel's own `app_program/%.o` rule compiles the overlaid SMP app without `sources.mk`'s
  `APP_INCLUDES`, and `rtos_port_mtk3.c` pulls in a header that redefines `size_t`.
- **What still lies beyond:** the link gap `curveball_findings.md` round 2 predicts (no Pico SDK runtime, no radios,
  no SD in this Make build).
- **Status matches the findings doc:** not buildable, never compiled before tonight, and this branch introduces no new
  error. The two compile errors and the two macOS issues are **new information**; the findings doc only knew it had
  never been compiled.

## 8. Do `main_smp.c` / `usermain_smp.c` need this branch's changes?

**`usermain_smp.c` (19 lines) needs no changes.** It only starts the app.

**`main_smp.c`: yes, three, before it is used with this branch's backend.** It is not changed now, because the SMP
build does not link:

1. **Persistent capture id.** It keeps `g_capture_id` in RAM (line 49, `++g_capture_id` at 147), so ids restart at 1
   after every reboot. Combined with the backend upsert (section 4), a rebooted SMP node would overwrite its earlier
   captures on the dashboard. Port `restore_capture_id()` / `store_next_id()` (`common/capture_id.h` is already
   portable C).
2. **Image from the label.** It still counts only `img_*.jpg` and rotates through them (lines 134–160), ignoring the
   label. Port the `sip_*` calls (`firmware/sensing/sound_image_pick.c`, which already compiles in the SMP build) and
   append the image name to the meta tail.
3. **Logging.** It calls the shared `sensing_capture()`, so its meta **already carries the new label and confidence**,
   but it prints no `[SOUND]` line. Add `sensing_log_sound()` through its log ring (`printf` is not allowed there).

**Effort once SMP links:** about 1 hour, most of it porting `main.c`'s two functions to `log_write_line`/`tm_sprintf`.

## 9. How it was checked

```
git diff --name-status 2e89e98 HEAD -- . ':!firmware/sensing' ':!ml' ':!docs/sound' ':!sd_card/samples'
git diff --stat 2e89e98 HEAD -- alfp mesh third_party common/uplink.c common/uplink.h firmware/app/radios.c firmware/app/storage_fatfs.c firmware_smp     (empty)
git diff 2e89e98 HEAD -- common/capture_meta.h                                  (comments only)
git show 2e89e98:firmware/app/main.c | grep -n "ch == '"                        (c e t h s d)
grep -n "'x'\|'z'" firmware_smp/app/main_smp.c firmware/app/main.c               (main_smp.c only)
ctest --test-dir build -R "^(units|audio_features|transfer|uplink|uplink_python|lora_mesh_integration|wifi_mesh_integration)$"
SMP=1|0 NODE_ID=2 firmware_smp/buildglue/build_smp.sh    (on scratch copies, see section 7)
```
