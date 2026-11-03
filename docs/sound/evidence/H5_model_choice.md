# H5: which model goes on the node (2026-11-03)

Data: 270 Pico-mic takes (H4: 240 ESC-50 clips at 10 per category through the MacBook Pro speakers at about 8 cm,
volume 1.0, plus 30 ambient: quiet, fan, outdoor). ESC-50 folds 1-3 train, 4 validation, 5 test.

All three candidates scored on the **same mic test fold** (fold 5, 54 takes), because that is what the node hears:

| model | trained on | group acc | group macro-F1 | exact label acc | animal labels emitted (correct) | animal vs not | flash |
|---|---|---|---|---|---|---|---|
| backup (committed) | ESC-50 converted in software (`esc50_direct`) | 0.315 | 0.283 | 0.167 | 1 (1) | 0.796 | 48361 B |
| **mic (chosen)** | the 270 mic takes | **0.648** | **0.617** | **0.333** | 1 (1) | **0.889** | **12448 B** |
| both | mic + esc50_direct | 0.463 | 0.454 | 0.278 | 1 (1) | 0.815 | 41549 B |

Per-group recall (mic model): AMBIENT 1.00, BIRD 0.75, MAMMAL 0.90, INSECT_FROG 0.50, HUMAN 0.12, VEHICLE 0.50,
WEATHER 0.75. The legacy mic.c thresholds score 0.444 on animal vs not for the same fold.

**Chosen: the mic-only model.** Best on every column, passes the plan's 0.6 stop rule (0.617), and is 4x smaller.
The backup collapses on the real mic (0.283): the software ADC simulation did not reproduce the MAX9814 + speaker
chain. Adding the backup data hurts (0.454) for the same reason.

**Caveats:**
- 54 test takes (2 per animal) make every number above uncertain by several points.
- `SOUND_STAGE2_MIN_CONF` came out at 54 (combined confidence, validation fold), so only 1 of 54 test takes got an
  animal-level label (coverage 0.04); the node mostly says the group (MAMMAL, BIRD). The on-device bench (H6)
  decides whether to lower it.
- Retraining is deterministic: a second `train.py` run gave a byte-identical `model_A.pkl`.
- The C export matches `qmodel.py` on all 54 test vectors (`ctest -R sound_model`).

## H6 update: the on-device bench disagrees (same evening)

`bench_eval.py --device "MacBook Pro Speakers"` (48 test-fold clips replayed, Pico classifies): group accuracy
**0.292**, group macro-F1 **0.219**, label accuracy 0.125 (offline: 0.648 / 0.617 / 0.333). Model time on the Pico
mean 273 us, max 461 us. The offline numbers above came from the same session as the training data and overstate
what the node does on a new recording; see AI_LOG. The backup model scored 0.283 offline on this fold, so neither
model is good on-device yet.

## H5b: retrained on two recording sessions (20:35)

A second session (`--takes 2`, 240 more takes, same setup) gave 510 takes. Group macro-F1 of stage-1 candidates,
6 non-ambient groups, train folds 1-4, test fold 5:

| features | best model | same session | **other session** |
|---|---|---|---|
| all 25 | RF n=40 d=6 | 0.42-0.43 | 0.25-0.35 (mean 0.30) |
| without loud_std, loud_range, log_rms, active_frac | RF n=40 d=6 | 0.41-0.46 | 0.30-0.31 (mean 0.30) |

Recording the same clips again costs about 0.12; dropping the loudness features (which follow speaker volume and the
MAX9814 AGC) makes the cross-session score steadier. Final `train.py` on both sessions, test fold 5 (unseen clips,
both sessions):

| model | group acc | group macro-F1 | animal vs not | flash |
|---|---|---|---|---|
| both sessions, all 25 features | 0.392 | 0.400 | 0.735 | 11941 B |
| **both sessions, 4 loudness features ignored (chosen, D15)** | **0.490** | **0.499** | **0.814** | **12629 B** |

The rejection rule (wrong stage-2 labels ≤ 15% on validation) gives `SOUND_STAGE2_MIN_CONF` 101: on the test fold an
animal name is wrong about half the time at every threshold (t=30: 14 of 27 wrong; t=45: 6 of 12), so the node reports
**groups only**. This is below the plan's 0.6 stop rule; R decides (DECISIONS D15).

## H6.2 bench of the two-session model (20:55)

48 test-fold clips replayed through the MacBook speakers, the Pico classifies (`bench_eval_A_2026-11-03.csv`; it
overwrote the earlier run's CSV of the same date, whose numbers are kept above):

| metric | on-device | offline |
|---|---|---|
| group accuracy | **0.500** | 0.490 |
| group macro-F1 | **0.486** | 0.499 |
| animal vs not | **0.833** | 0.814 |
| model time on the Pico | mean 175 us, max 422 us | |

Gap +0.013: the two-session model behaves on the node as it does offline (the session-1 model: 0.219 vs 0.617).
Groups that work best: WEATHER 6/8, HUMAN 3/8, VEHICLE 3/8, BIRD 5/8 (as BIRD), MAMMAL 6/10, INSECT_FROG 2/6.
