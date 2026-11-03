# Commit queue (autonomous session, 2026-11-03)

Claude ran no git command that changes state. Run these blocks yourself, **in order**, from the repo root
(macOS Terminal or Windows PowerShell; the commands are the same). Each block leaves the repo building.

**Merges (R's rule: tasks touching the same file share one block):**
- `firmware/CMakeLists.txt` was touched by A1, A6 and A8.
- `firmware/app/main.c` was touched by A6, A7 and A8.
- `host/sound.cmake` was touched by A3, A5, A7 and A8.
- So **A1 + A3 + A5 + A6 + A7 + A8 are one block (block 4)**.
- Cross-cutting docs and registries (the status board, `docs/sound/*.md` logs, `docs/BRINGUP.md`,
  `ml/README.md`, `ml/smoke_test.py`) go in the last block, with their final content (DECISIONS D2).

Not in any block on purpose:
- `.claude/settings.local.json` (your local settings).
- `ml/data/manifest.csv` (regenerated tonight, D7).
- Everything gitignored: `deps/`, `build*/`, `ml/.venv`, ESC-50, `.npy`, raw images, model pickles.

## 1. A0: ml workspace

```
git add .gitattributes ml/.gitignore ml/requirements.txt ml/hosttools.py ml/playback.py
git commit -m "chore(sound): add ml workspace, baseline measurements and sound docs skeleton" -m "Pinned Python 3.11/3.12 requirements (wheels checked for macOS arm64/x86_64 and Windows x64), cross-platform playback and host-tool lookup, and .gitattributes (one pattern per line). Baseline numbers are in the docs block."
```

## 2. A2: dataset tooling and the label table

```
git add common/sound_classes.h common/sound_classes.py host/tests/test_sound_classes.py ml/label_map.yaml ml/mlcommon.py ml/prepare_esc50.py ml/record_dataset.py
git commit -m "feat(ml): ESC-50 label mapping and automated Pico-mic dataset recorder" -m "common/sound_classes.h/.py is the single label table (ids 0-6 groups, 16-27 Phase A animals) with a header-vs-Python diff test. prepare_esc50 spreads clips over the 5 folds; record_dataset plays each clip at the Pico, saves raw uint16 + wav, and is resumable with a dry-run mode."
```

## 3. A4: feature extraction and parity tooling

```
git add ml/extract_features.py ml/check_parity.py ml/plots.py docs/sound/figures/feature_boxplots_direct.png
git commit -m "feat(ml): feature extraction via C extractor and Pico/host parity check" -m "Features always come from build/sound_featx, never a Python copy. check_parity compares the Pico's @FEAT with sound_featx within 1e-3 relative (dry-run passes; the live run is HARDWARE_TODO H3)."
```

## 4. A1 + A3 + A5 + A6 + A7 + A8 (merged): the on-node classifier, end to end

```
git add firmware/sensing/sound_fft.c firmware/sensing/sound_fft.h firmware/sensing/sound_tables.c firmware/sensing/sound_tables.h firmware/sensing/sound_features.c firmware/sensing/sound_features.h tools/gen_sound_tables.py host/tools/sound_featx.c host/tests/test_sound_features.c host/sound.cmake host/CMakeLists.txt firmware/audiodump/audiodump_main.c ml/pico_audio.py ml/tests/test_pico_audio.py ml/esc50_direct.py ml/qmodel.py ml/train.py ml/train_config.yaml ml/export_c.py ml/models/metrics_A.json firmware/sensing/sound_classify.c firmware/sensing/sound_classify.h firmware/sensing/sound_model.h host/tests/test_sound_model.c host/tests/sound_model_vectors.h docs/sound/model_card.md docs/sound/figures/confusion_group_test.png docs/sound/figures/confusion_label_test.png firmware/sensing/audio_capture.c firmware/sensing/audio_capture.h firmware/sensing/sensing.c firmware/sensing/sensing.h firmware/sensortest/sensortest_main.c common/capture_meta.h ml/bench_eval.py common/capture_id.h common/capture_id.py host/tests/test_capture_id.c host/tests/test_capture_id.py firmware/sensing/sound_image_pick.c firmware/sensing/sound_image_pick.h host/tests/test_sound_image_pick.c tools/make_label_images.py tools/fetch_label_images.py ml/images_src/credits.csv sd_card/samples docs/sound/IMAGE_CREDITS.md firmware/app/main.c firmware/CMakeLists.txt
git commit -m "feat(sound): on-node two-stage sound classifier, persistent capture ids and label-driven image choice" -m "Merged A1/A3/A5/A6/A7/A8 (shared CMakeLists.txt, main.c, sound.cmake). Streaming 256-pt Q15 FFT features (25, v1) with host CLI and tests; audio_dump.uf2 + pico_audio.py for raw mic recordings; BACKUP two-stage tree model (ESC-50 converted in software, test group F1 0.613) exported to C with a 200/200 parity test; sensing fills sound_class/sound_conf and prints [SOUND]; capture ids persist on SD as N2-000042; the node picks samples/<label>_NN.jpg with group/generic fallback (35 free-licence images)."
```

## 5. A9: bridge, backend, dashboard and simulator show the node's labels

```
git add tools/bridge_receiver.py backend/storage.py backend/app.py backend/tests/test_sound_labels.py simulator/demo_feed.py dashboard/src/App.tsx dashboard/src/components/CaptureFeed.tsx dashboard/src/components/CaptureModal.tsx dashboard/src/components/SoundLabel.tsx dashboard/src/lib/api.ts dashboard/src/lib/types.ts dashboard/src/pages/HomePage.tsx dashboard/src/pages/SpeciesPage.tsx dashboard/src/styles/app.css
git commit -m "feat(dashboard): display on-node sound labels and species tally" -m "decode_meta names the node's label id from common/sound_classes.py and adds display_id; storage adds sound_conf/label/group/image_name/display_id, one row per real capture, and a /api/species tally (no classification in the backend). Dashboard: N2-000042 badge, 'DOG · 82% · MAMMAL' chip, identified-on-node panel, species pages. demo_feed uses the new ids and stays SIMULATED."
```

## 6. Docs and bookkeeping (final content of the cross-cutting files)

```
git add SOUND_PLAN.md docs/sound/baseline.md docs/sound/AI_LOG.md docs/sound/HANDOFF.md docs/sound/DECISIONS.md docs/sound/HARDWARE_TODO.md docs/sound/TONIGHT.md docs/sound/COMMIT_QUEUE.md docs/BRINGUP.md ml/README.md ml/smoke_test.py
git commit -m "docs(sound): status board, baseline, handoff, decisions and tonight's hardware script" -m "Records the autonomous session: baseline + post-integration RAM/flash, teammate handoff (reflash all nodes, new SD samples, clear dev DB), decisions taken without R, AI evidence rows, and the timed hardware checklist. ml/README + smoke_test cover every new tool (34 checks)."
```

## After tonight

See the last step of `docs/sound/TONIGHT.md` for the commit of the mic recordings, the retrained model and the evidence.
