# Sound work: handoff to teammates

Every edit the sound work makes to a teammate's file is logged here (SOUND_PLAN.md rule 6),
with the date, the file, what changed and what the owner needs to do.

## All nodes

- **2026-11-03, reflash every node together.** `sound_class` on the wire is now a **label id** from
  `common/sound_classes.h` (0-6 groups, 16-27 Phase A animals, 0xFF unknown). The old values 1 LOUD_ANIMAL_CALL /
  2 HIGH_FREQ_RUSTLE are retired, and 1 and 2 now mean BIRD and MAMMAL. A mix of old and new firmware would
  mislabel captures. The 27-byte layout is unchanged (still version 1).
- **SD cards for nodes 2-5:** copy the repo's `sd_card/samples/` folder onto each card (label images such as
  `dog_00.jpg`; credits in `docs/sound/IMAGE_CREDITS.md`) and **keep the existing `img_NN.jpg`**, which are the
  generic fallback. The node also creates `meta/next_id` (persistent capture ids); do not delete it.
- **Dev DB:** delete `backend/smartrelay.db` once after updating. The backend adds the new columns itself, but
  old rows have no labels, and duplicate capture ids from before the persistent ids block the new uniqueness
  index (the backend warns and keeps running).
- **New image:** `audio_dump.uf2` (Rhaylene's training recorder; no radios, no SD). Only flash it on the bench node.

## Shreya (`common/capture_meta.h`, `firmware/app/main.c`, `firmware/sensortest/`, `firmware/CMakeLists.txt`, `tools/bridge_receiver.py`, `simulator/demo_feed.py`, `backend/`, `dashboard/src/`)

All 2026-11-03, R-approved per SOUND_PLAN.md rule 6:

- `common/capture_meta.h`: comments only. `sound_class` = label id, `sound_conf` = the classifier's confidence,
  `sound_rms` is DC-corrected. No layout change.
- `firmware/app/main.c`:
  - `handle_capture()` picks the image from the sound label (`sound_image_pick.c`: label -> group -> `img_NN`)
    instead of round-robin `img_NN`, sets `image_index` to the picked NN, and appends the file name to the meta
    (the existing optional-name tail; 27 + name <= 64 bytes).
  - The capture id survives reboots: `meta/next_id` is written via a temp file + rename on every capture; at
    boot `restore_capture_id()` takes the larger of that file and the highest id still in `outbox/` jobs or
    `clips/` names.
  - Logs use `N2-000042` (`common/capture_id.h`), e.g. `capture N2-000042: heard DOG (82%) -> samples/dog_01.jpg`.
  - The sensors task prints the `[SOUND]` line (`sensing_log_sound()`); the `[AUDIO]` line is now labelled
    "legacy mic.c".
  - The boot line lists the sample images per prefix.
  - New static RAM: about 1.8 kB.
- `firmware/sensortest/sensortest_main.c`: `m`/`q` run the full classifier (`sensing_record_sound()`) and print
  the `[SOUND]` line; the dual-trigger path prints it too.
- `firmware/CMakeLists.txt`: new `sound` library (pure-C sound files, linked into `sensing`), new `audio_dump`
  target; `sensing` also links `hardware_timer`.
- `tools/bridge_receiver.py`: names come from `common/sound_classes.py`. `decode_meta()` adds `sound_label`,
  `sound_group`, `sound_kind` and `display_id`; `sound_name` is now the label name. The printed line shows
  `N2-000042: DOG 82%`.
- `backend/storage.py`: new columns `sound_conf`, `sound_label`, `sound_group`, `image_name`, `display_id`
  (added by `ALTER TABLE` when missing); a partial unique index on (node_id, capture_id) for real rows (a
  re-delivered capture updates its row); `species_tally()`.
- `backend/app.py`: `GET /api/species` (a tally only, no classification). New `backend/tests/` (pytest).
- `simulator/demo_feed.py`: random label ids from the table and `--label-images sd_card/samples`; still tagged
  SIMULATED.
- `dashboard/src/`: `N2-000042` badge on cards and the modal, a label chip (`DOG · 82% · MAMMAL` /
  `BIRD (unidentified)`) with "identified on-node", the modal's "Identified on-node" panel, `SpeciesPage` and
  HomePage "Species seen" from `/api/species`. New `components/SoundLabel.tsx`; small CSS block at the end of
  `styles/app.css`.

## Maryam (`firmware/sensing/sensing.c/.h`, trigger rule, ADC ownership, `trigger_trust`)

- 2026-11-03, `sensing.c/.h`: `sensing_capture()` now records through `sensing_record_sound()`. Same DMA
  capture, plus streaming spectral features and the two-stage classifier, which fill `sound_class`,
  `sound_conf`, `sound_rms` and `sound_zcr`.
  - New: `sensing_record_sound()`, `sensing_last_sound()` and `sensing_log_sound()`.
  - The `sf_t` state (1.9 kB) is static.
  - **Not touched:** the trigger rule, the cooldown, ADC ownership (light reads unchanged), `trigger_trust`.
- `audio_capture.c/.h`: the new `audio_capture_sf()` also feeds `sf_add()` and times it; `audio_capture()` keeps
  its exact old behaviour.

## Goutham (WiFi mesh)

- No changes. Each capture's meta is now 27 + up to ~18 bytes (the image name), still within ALFP's 64.

## Mridula (LoRa mesh)

- No changes. Label images are up to 10 kB (the old samples were 3-6 kB), so a LoRa capture can take up to
  about 70 s over 2 hops. `tools/make_label_images.py --max-kb 6` makes smaller ones if LoRa demos run long.

## Interactive session (2026-11-03 evening)

- `backend/app.py` (Shreya's file): **bug fix, pre-existing in curveball 2e89e98.** The WebSocket `snapshot`
  sent capture rows without `image_url`, so after any page reload or reconnect every card showed "no image" until a
  new capture arrived. All four places that send capture rows (REST list, REST single, live push, snapshot) now go
  through one `with_image_url()` helper. Found during H9.
- `simulator/demo_feed.py` (Shreya's file), demo-only, found during H9:
  - the placeholder for captures with no label image was random bytes behind a JPEG header (curveball 2e89e98),
    so browsers showed a broken image; it is now a real 375-byte grey "SIMULATED" JPEG with the padding after its
    end marker (same transfer sizes);
  - capture ids restarted at 1 every run, so two runs both showed `N2-000001`; they now continue from a
    time-based start (minutes, mod 100000).
