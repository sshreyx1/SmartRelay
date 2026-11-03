# Tonight: hardware session to the 3 am demo

About **2 h 30 min** of work in total, about 45 min of which is unattended recording. All exact commands and expected
outputs are in `docs/sound/HARDWARE_TODO.md` (H1-H10); this page is the order, the timing and the fallbacks.
Run everything from the repo root in a terminal set up as in HARDWARE_TODO's "Setup" section.

**Safety net:** the firmware already contains a working **backup model** (trained on ESC-50 converted in software,
not the mic; test group macro-F1 0.61). If anything below goes wrong, the demo can still run on it.

**Updated 20:50 (interactive session).** Done so far: commit queue, H9 (simulator), H1, H2, H3, H4 (two sessions,
510 takes), H5/H5b (two-session model, groups only, D15). Remaining, in order:

| # | step | who | needs | time | clock |
|---|---|---|---|---|---|
| 1 | **H6.2** flash `audio_dump.uf2`, `python ml/bench_eval.py --device "MacBook Pro Speakers"` | R | node 2, MacBook 8 cm | 8 min | 20:50 |
| 2 | Commit blocks for everything so far (Claude prints them) | R | — | 5 min | 21:00 |
| 3 | **H6.3** flash `csc09_node_2.uf2`, one playlist clip + `c`, check the `[SOUND]` line | R | node 2 | 10 min | 21:05 |
| 4 | **H8** `sd_card/samples/` on node 2's card; node 2 → node 1 → backend → dashboard | R | nodes 1, 2, SD cards | 15 min | 21:15 |
| 5 | **H7** 3 captures, reboot, 2 captures: ids continue | R | node 2 | 5 min | 21:30 |
| 6 | **H12** a-d: node 3, nodes 2+3 together, 5 → 3 → 1, store and forward | R | nodes 1, 2, 3 (+5) | 30-40 min | 21:35 |
| 7 | Demo playlist from the new bench; DEMO.md segment 5 decided by H12c | Claude | — | 5 min | 22:15 |
| 8 | **H11** screen-record segment 1 (and 5 if H12c passed) | R | all | 10 min | 22:20 |
| 9 | Demo run-through ×2 (`DEMO.md`) | R | all | 25 min | 22:30 |
| 10 | Commit tonight's evidence | R | — | 5 min | 22:55 |
| 11 | Optional before 02:00: **T6** (2.5 min, scripted) and **T3** (15 min) from `TESTS.md`; A11 docs | R + Claude | node 2 (+1) | 45 min | 23:00 |
| | buffer | | | about 3 h | 23:45-03:00 |

**Cut first if late:** 11 (T3/T6, A11), then H12c/d, then demo segment 2. Never cut H8, H11 and one run-through.

<details><summary>Original plan (written before the session)</summary>

| # | step | time | clock (start 23:30) |
|---|---|---|---|
| 0 | Commit the autonomous session's work (`docs/sound/COMMIT_QUEUE.md`, run the blocks in order) | 5 min | 23:30 |
| 1 | Build check: `cmake --build build-fw -j4` (10 images) and `ctest --test-dir build` (13/13) | 5 min | 23:35 |
| 2 | **H1** `sensor_test.uf2` → `q` ×3, fill the baseline facts | 5 min | 23:40 |
| 3 | **H2** flash `audio_dump.uf2`, then `python ml/pico_audio.py --test 2` (clap) | 5 min | 23:45 |
| 4 | **H3** `python ml/check_parity.py --takes 5` → PASS | 5 min | 23:50 |
| 5 | **H4** `prepare_esc50.py --per-category 10`, then `record_dataset.py --device N --volumes 1.0` (240 takes) | 40 min, unattended | 23:55 |
| 6 | **H4** ambient: `--ambient 10 --ambient-only` × quiet / fan / outdoor | 8 min | 00:35 |
| 7 | **H5** extract → retrain → export → host tests | 5 min | 00:45 |
| 8 | Rebuild the firmware; flash `audio_dump.uf2`; **H6.2** `python ml/bench_eval.py --device N` | 10 min | 00:50 |
| 9 | Flash `csc09_node_2.uf2`; **live test 5-10 clips** (below) | 15 min | 01:00 |
| 10 | **H8** copy `sd_card/samples/` to node 2's card; dog → `dog_0X.jpg`, rain → `weather_00.jpg` | 10 min | 01:15 |
| 11 | **H7** 3 captures, reboot, 2 captures: ids continue | 5 min | 01:25 |
| 12 | **H9** bridge + backend + dashboard, end to end | 20 min | 01:30 |
| 13 | **H11** screen-record segment 1 of `DEMO.md`; then demo run-through ×2 (`DEMO.md`) | 25 min | 01:50 |
| 14 | Commit tonight's evidence (block at the end) | 5 min | 02:10 |
| | buffer | 45 min | 02:15-03:00 |

</details>

## While step 5 records (40 min free)

Leave the room quiet and the speaker fixed, and keep the laptop on the charger. Meanwhile (other terminal, no Pico
needed):
- Run the Windows smoke check with a teammate (**H10**).
- Plug the bridge's USB in, and start the backend and, from the backend's venv with `requests` installed, `simulator/demo_feed.py --burst 5 --label-images sd_card/samples`
  to see that the dashboard renders (**H9** without hardware).

## Step 7 in detail: retrain on the mic

```
python ml/extract_features.py
python ml/train.py --features data/features_v1.csv
python ml/export_c.py --vectors
cmake --build build -j && ctest --test-dir build --output-on-failure
```
The train.py `test:` line is the offline result. The model card and confusion matrices are rewritten
automatically, and this time without the "BACKUP" banner (it only appears when `--note` says BACKUP).

**If the mic model is worse** (test group macro-F1 < 0.6 or clearly below the backup's 0.613), pick one:
1. **Train on both** (usually best: more data, and the backup data teaches the shape of each animal):
   `python ml/train.py --features data/features_v1.csv data/features_esc50_direct.csv`
2. **Go back to the backup:** `git checkout -- firmware/sensing/sound_model.h host/tests/sound_model_vectors.h`
   (it was committed in step 0).
Decide by step 8's **on-device** bench number, not the offline one. Write the choice in `docs/sound/DECISIONS.md`.

## Step 9: live test 5-10 clips on the node

Node 2 running `csc09_node_2.uf2`, monitor open in terminal A; terminal B plays the clips. For each clip, start the
playback in B and press `c` in A within half a second. The node records 5 s.

| clip | expect `[SOUND] ... label=` |
|---|---|
| dog `5-203128-A-0.wav` | DOG (or MAMMAL) |
| rooster `5-194930-A-1.wav` | ROOSTER (or BIRD) |
| crow `5-213836-A-9.wav` | CROW (or BIRD) |
| frog `5-156026-A-4.wav` | FROG (or INSECT_FROG) |
| cat `5-169983-A-5.wav` | CAT (or MAMMAL) |
| rain `5-181766-A-10.wav` | WEATHER |
| laughing `5-171118-A-26.wav` | HUMAN |
| nothing (quiet room) | AMBIENT |

Every line must show `lost=0`. A group instead of the animal is the rejection working (low confidence), not a bug.
Note which clips work best: those are the demo clips.

## Step 13: the demo (3 min, from SOUND_PLAN.md)

**Superseded by `docs/sound/DEMO.md`** (windows to open, all segments, fallbacks). Kept below as the short version.

1. The serial monitor on node 2 and the dashboard are side by side; the bridge and backend are running.
2. "The Pico does all the analysis." Play the best dog (or rooster) clip and wave at the PIR.
3. Point at the serial line: `[SOUND] group=MAMMAL label=DOG conf=82 lost=0`, then
   `capture N2-000042: heard DOG (82%) -> samples/dog_01.jpg`.
4. The dashboard card appears with the **same `N2-000042` badge**, the dog picture the node chose, and
   `DOG · 82% · MAMMAL · identified on-node`. Open the Species page: the tally went up.
5. Play rain: it reports WEATHER and the weather picture. "It says the group when it is not sure, rather than guessing."
6. Before / after: the model card's legacy mic.c thresholds vs the new classifier (animal vs not-animal).

**Fallbacks:**
- Radios misbehave → show node 2's serial line + `python ml/pico_audio.py --test 5 --features` (classification
  on the Pico, no radio).
- Nothing works → (backend venv, `requests` installed) `python simulator/demo_feed.py --continuous --every 10 --label-images sd_card/samples`. Every
  card says SIMULATED; say so out loud.

## Step 14: commit tonight's results

```
git add ml/data/manifest.csv ml/data/recordings.csv ml/data/features_v1.csv ml/models/metrics_A.json firmware/sensing/sound_model.h host/tests/sound_model_vectors.h docs/sound/model_card.md docs/sound/figures docs/sound/evidence docs/sound/baseline.md docs/sound/DECISIONS.md SOUND_PLAN.md
git commit -m "feat(ml): retrain two-stage classifier on Pico-mic recordings, bench evaluation" -m "240 ESC-50 takes at volume 1.0 + 30 ambient recorded through the node's MAX9814; on-device bench vs offline metrics in docs/sound/evidence."
```
(Raw `.npy`/`.wav` recordings stay local; `ml/.gitignore` keeps them out.)
