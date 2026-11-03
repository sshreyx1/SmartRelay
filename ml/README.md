# ml/: sound classifier workspace

Training, data recording and evaluation tools for the on-node sound classifier
(plan: [`SOUND_PLAN.md`](../SOUND_PLAN.md), design notes: [`docs/sound/`](../docs/sound/)).
The Pico does all classification. This folder only **trains** the model and exports it to C
(`firmware/sensing/sound_model.h`). Features always come from the C extractor (`build/sound_featx`),
never from a Python re-implementation, so training and the node see identical numbers.

Everything here runs on **macOS and Windows** with plain Python: no shell scripts, no ffmpeg,
no OS-specific tools.

## Setup (once)

Use **Python 3.11 or 3.12**. The pinned packages have no wheels for 3.13+ yet, so
`pip install` would try to compile numpy/scipy and fail. On a Mac with Homebrew, plain
`python3` may be 3.13 or 3.14, so name the version explicitly.

**macOS**
```sh
cd ml
python3.11 -m venv .venv        # or python3.12; brew install python@3.11 if neither exists
.venv/bin/pip install -r requirements.txt
.venv/bin/python smoke_test.py
```

**Windows (PowerShell)**
```powershell
cd ml
py -3.11 -m venv .venv          # or py -3.12; install from python.org if missing
.venv\Scripts\pip install -r requirements.txt
.venv\Scripts\python smoke_test.py
```

`smoke_test.py` must end with `0 failed`. No ffmpeg is needed: `soundfile` decodes MP3 itself.

## Tools

Pipeline order: record -> extract -> train -> export -> flash -> bench. `docs/sound/TONIGHT.md` is the timed walk-through.

| Script | What it does |
|---|---|
| `smoke_test.py` | Imports, serial ports, every CLI's `--help`, no-hardware dry runs. Run after setup, and on Windows at the 🪟 checkpoints. |
| `pico_audio.py` | Talks to `audio_dump.uf2`: `record(s)` raw 16 kHz uint16 (CRC-checked), `features()`, `classify()`. CLI `--test 2`. |
| `playback.py` | `play(path, volume, device)`, blocking, via sounddevice. `--list-devices`, `--dry-run`. |
| `prepare_esc50.py` | ESC-50 + `label_map.yaml` -> `data/manifest.csv` (`--per-category N`, spread over the 5 folds). |
| `record_dataset.py` | Plays every manifest clip at the Pico and saves what it heard: `data/raw/<LABEL>/*.npy` + `data/recordings.csv`. Resumable; `--ambient K`, `--dry-run`. |
| `extract_features.py` | Runs the C extractor `build/sound_featx` over the recordings -> `data/features_v1.csv`. |
| `check_parity.py` | The Pico's own `@FEAT` vs `sound_featx` on the same samples (1e-3 relative). |
| `esc50_direct.py` | **Backup** dataset: ESC-50 converted to ADC-like samples in software (no mic) -> `data/features_esc50_direct.csv`. |
| `train.py` | Two-stage tree classifier (group -> animal) + rejection threshold -> `models/`, `docs/sound/model_card.md`, confusion PNGs. |
| `qmodel.py` | Python twin of `sound_classify.c` (quantised trees); every metric is computed with it. |
| `export_c.py` | `models/model_A.pkl` -> `firmware/sensing/sound_model.h`; `--vectors` -> `host/tests/sound_model_vectors.h` (C must match 100%). |
| `bench_eval.py` | Plays test-fold clips at the Pico and scores the Pico's own `@CLS` answers vs offline. |
| `plots.py` | Per-group box plots of the most informative features. |
| `hosttools.py` | `find_host_tool("sound_featx")` finds the built host tool (also `.exe`, `Debug`/`Release`). |
| `../tools/make_label_images.py` | Label photos -> `sd_card/samples/<prefix>_NN.jpg` (<= 10 kB) + `docs/sound/IMAGE_CREDITS.md`. |
| `../tools/demo_multihop.py` | Demo: E -> C -> A with the real `wifi_mesh.c` + ALFP as 3 local processes, prints hop path and relay counters (macOS/Linux; Windows in WSL). |

The host tool comes from `cmake -S host -B build && cmake --build build -j` (Windows/MSVC:
`cmake -S host -B build -DSOUND_ONLY=ON` then `cmake --build build --config Release`).

## Serial ports

Tools auto-detect the Pico by USB VID `0x2E8A`, or take `--port`:

| | macOS | Windows |
|---|---|---|
| list ports | `.venv/bin/python -m serial.tools.list_ports -v` | `.venv\Scripts\python -m serial.tools.list_ports -v` |
| port name | `/dev/cu.usbmodem1101` (the `/dev/tty.` name works too) | `COM5` |

Close any serial monitor before running a tool; only one program can hold the port.

## Data

Everything big stays local and gitignored (`data/raw/`, `data/esc50/`, `data/xc/`, audio, `.npy`).
Manifests and CSV summaries in `data/` are small and are committed.
