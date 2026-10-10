# SmartRelay

**A dual-radio mesh prototype for trail wildlife monitoring**

SmartRelay is a five-node embedded systems project that collects sensor
readings and transfers image files across a trail-style mesh. Field nodes
use a Raspberry Pi Pico W with PIR, sound, temperature, humidity and light
sensors. A capture is triggered when motion and sound are detected
together. Each capture includes environmental readings and compact sound
features, along with an image payload sent to the bridge node.

The current prototype uses JPEG sample images stored on microSD in place
of a camera module. The firmware processes microphone input into basic
sound features and a coarse sound label; it does not identify wildlife
species. The environmental readings also produce an on-device weather
label.

## System Overview

- **Field nodes:** FreeRTOS firmware reads the sensors, evaluates the
  trigger, and selects the next sample image from microSD. Captures are
  queued locally when the bridge is unavailable.
- **Mesh transfer:** LoRa and WiFi operate as separate flooding meshes.
  Each transfer selects one radio and stays on it. The ALFP protocol
  fragments files, checks packets and complete files with CRCs, and
  retransmits missing fragments rather than restarting the transfer.
- **Bridge:** Node 1 reassembles and verifies incoming files, stores them
  on microSD, and sends the capture data to a laptop over USB serial.
- **Backend and dashboard:** A FastAPI service reads the bridge stream,
  stores captures and metadata in SQLite, and provides REST and WebSocket
  endpoints. The React dashboard displays captures, sensor readings,
  node activity, and the bridge log.
- **Demo mode:** `simulator/demo_feed.py` can be run manually to exercise
  the dashboard without hardware. Demo captures are marked as simulated
  and kept distinct from real bridge data.

## Current Scope

SmartRelay demonstrates sensor-triggered capture, dual-radio mesh
transport, selective fragment recovery, local store-and-forward, and a
laptop-side monitoring interface. It is a prototype rather than a
finished wildlife camera system: images are supplied from microSD, and
species identification, species review, and trigger-confidence scoring
are not implemented. Node activity in the dashboard is based on received
captures; the firmware does not provide a periodic laptop-facing
heartbeat.

Protocol tests and mesh simulations are included in `host/` and
`docs/results/`. The design notes report simulator measurements, including
lower LoRa airtime for selective retransmission than resending a complete
file. These results are simulation measurements, not a claim of equivalent
field performance. See [docs/DESIGN.md](docs/DESIGN.md) and
[docs/BRINGUP.md](docs/BRINGUP.md) for the design rationale, test evidence,
and hardware bring-up status.

## Repository Structure

- `alfp/` — C11 application-layer fragmentation and transfer protocol.
- `firmware/`, `mesh/`, `third_party/` — Pico W firmware, radio meshes,
  and sensor integrations.
- `common/` — capture metadata and the bridge-to-laptop serial format.
- `backend/`, `dashboard/` — capture ingestion, storage, API, and user
  interface.
- `host/` — protocol tests, simulations, and integration test programs.
- `simulator/` — manually started dashboard demo feed.
- `tools/`, `uf2/`, `docs/` — project utilities, firmware images, and
  design and test documentation.

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
