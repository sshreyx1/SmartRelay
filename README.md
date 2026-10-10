# SmartRelay (CSC09 trail mesh)

A field-node mesh (PIR/sound-triggered camera captures over LoRa or WiFi,
reassembled by a bridge node) with a live dashboard on top.

This repo = the real ALFP firmware and protocol code (unchanged) **plus**
a new backend and dashboard that show that real data, and a clearly
separate, manual-only simulator for demoing the UI without hardware.

**Real vs. simulated, and why it matters:** the dashboard must never
present invented data as if it came from the mesh. So:

- The backend only ever gets real captures by actually reading the
  bridge's USB serial port — automatically, the moment it's plugged in.
  Nothing about that path is faked.
- The `simulator/` only runs when a person runs it, by hand, on purpose.
  Every row it sends is tagged `is_demo=1` end to end — stored that way,
  returned that way by the API, and shown with a **SIMULATED** badge in
  the dashboard. Demo rows are also excluded from node "last seen"
  status, so a demo run can never make a node look alive.
- Node status is never a fabricated heartbeat. The real firmware doesn't
  send a laptop-facing heartbeat, so "last seen" is derived only from
  real captures actually received — see `backend/storage.py:node_last_seen()`.
- Any sensor field the firmware didn't fill in (e.g. no DHT22 reading
  that cycle) is sent as an explicit missing/unknown sentinel
  (`common/capture_meta.h`) and shown in the dashboard as `-`, never a
  guessed number.

## Layout

- `alfp/` — the ALFP protocol library (C11, no malloc). Unchanged.
- `mesh/`, `third_party/` — radio + sensor drivers from the rest of the
  team (LoRa mesh, WiFi mesh, PIR/DHT22/weather, sound trigger). Unchanged.
- `common/` — capture metadata wire format (`capture_meta.h`) and the
  bridge→laptop USB uplink text protocol (`uplink.h`/`uplink.c`). Unchanged.
- `firmware/` — Pico W firmware for all 5 nodes, sensor test, SD
  self-test, LoRa photo test. Unchanged.
- `host/` — laptop-side mesh simulator and test suite for the protocol
  itself (not the dashboard). Unchanged.
- `tools/` — `bridge_receiver.py` (the original CLI bridge reader) and
  other original helper scripts. Unchanged; `backend/` reuses its
  decode functions directly rather than re-implementing the wire format.
- `uf2/`, `docs/` — ready-to-flash images and the original design docs.
- **`backend/`** — *new*. A FastAPI service that reads the bridge's
  serial port automatically, parses the real ALFP uplink protocol,
  stores captures in SQLite, and serves them over REST + WebSocket.
- **`dashboard/`** — *new*. A React + TypeScript dashboard that shows
  only real backend fields: node status, the live capture feed (photo +
  sensor metadata), and the raw serial log.
- **`simulator/`** — *new*. `demo_feed.py`, a manual-only script that
  posts synthetic-but-correctly-formatted captures to the backend for
  demoing the dashboard without hardware. Never started automatically.

## Running it

### 1. Backend (talks to the real bridge automatically)

```bash
cd backend
pip install -r requirements.txt
# Plug in the bridge node (node 1) over USB first.
# SMARTRELAY_BRIDGE_PORT is optional -- if unset, the backend tries to
# autodetect a Raspberry Pi Foundation serial device.
SMARTRELAY_BRIDGE_PORT=/dev/ttyACM0 uvicorn app:app --host 0.0.0.0 --port 8000
```

Check `curl localhost:8000/api/health` — it reports whether a bridge
port was found and whether it's actually connected right now.

### 2. Dashboard

```bash
cd dashboard
npm install
npm run dev          # dev server on :5173, proxies /api and /ws to :8000
# or: npm run build   # production build in dashboard/dist/
```

### 3. Demo/simulator mode (manual, for presenting without hardware)

```bash
cd backend && uvicorn app:app --port 8000     # in one terminal, no bridge needed
python3 simulator/demo_feed.py --burst 5      # in another terminal, run it yourself
# or: python3 simulator/demo_feed.py --continuous --every 20
```

Every capture this produces is stored and shown with a **SIMULATED**
badge, and is never substituted for real sensor/node data.

## Firmware: build and flash

Needs gcc, cmake, python3.

```bash
cmake -S host -B build
cmake --build build -j
ctest --test-dir build --output-on-failure      # protocol test suite, sanitizers on
```

```bash
./tools/fetch_deps.sh                  # pico-sdk 2.1.1, FreeRTOS-Kernel, carlk3 FatFs, pinned
export PICO_SDK_PATH=$PWD/deps/pico-sdk
export FREERTOS_KERNEL_PATH=$PWD/deps/FreeRTOS-Kernel
export FATFS_PICO_PATH=$PWD/deps/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
cmake -S firmware -B build-fw && cmake --build build-fw -j4
```

Outputs `csc09_node_1.uf2` (bridge) through `csc09_node_5.uf2`, plus the
test images. Hold BOOTSEL, plug in, drag the `.uf2` onto the RPI-RP2
drive. Full wiring/bring-up steps: `docs/BRINGUP.md`.

## Serial commands on a node

`c` capture now, `e` sensor readings, `s` statistics, `d` toggle 30%
injected loss, `t` toggle the WiFi topology filter, `h` help.
