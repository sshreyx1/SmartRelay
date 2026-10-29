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

## Curveball: uT-Kernel 3 SMP port (branch `curveball`)

This branch ports the firmware from FreeRTOS (single core) to uT-Kernel 3,
an SMP kernel that runs application tasks on both RP2040 cores at once. It
is separate, additive work: the firmware described above is untouched and
still builds and runs exactly as before. The new kernel lives in
`firmware_smp/` and a vendored copy of the kernel at
`third_party/mtk3smp_rp2040`. Full details, citations, and open risks are
in `docs/curveball_findings.md`.

### What changed

- Three tasks, same as before: comms (radios, SD, ALFP), sensors (PIR,
  sound, DHT22, audio), and console. The console is merged with a new
  USB-owner task that is the only task allowed to print to the USB serial
  console; comms and sensors send it log lines instead of printing
  directly.
- Core layout: comms and the USB-owner task run on core 0. Sensors run
  alone on core 1. If pinning a task to a core is turned off at build time
  (`SMP=0`), everything runs on core 0 like the old firmware did.
- The FreeRTOS calls (tasks, queues, delays) are replaced one for one with
  uT-Kernel equivalents, wrapped in `firmware_smp/rtos_port.h` so the
  application code never calls the kernel directly.
- The capture request queue, the stats printout, and the WiFi-bridge UART
  receive buffer all got the hardening the design review asked for: no
  blocking sends, drop counters, and a high-water mark so a slow consumer
  shows up as a number instead of a silent problem.

### What is not done yet

- The build does not yet compile in the LoRa radio driver, the WiFi
  bridge driver, or the SD card storage layer. Only the kernel, the
  application tasks, the ALFP protocol, and the sensor code are wired in
  so far.
- Nothing on this branch has been compiled. The AI session that did this
  work did not have the ARM cross compiler, `make`, `cmake`, or the Pico
  SDK available, so none of it has been built, flashed, or tested.
- A real build is also expected to need Pico SDK driver code (for GPIO,
  SPI, ADC, DMA, UART) added to the kernel's build, and that needs a real
  toolchain run to get right. See `firmware_smp/buildglue/README.md` for
  exactly what to run and what to send back.
- T1 to T8 below have not been run on hardware.

### How to build

Needs `arm-none-eabi-gcc`, `make`, and (for the USB console) `PICO_SDK_PATH`
set, same as the existing firmware above.

```bash
git submodule update --init third_party/mtk3smp_rp2040

# dual core, pinned (comms + console on core 0, sensors on core 1)
SMP=1 NODE_ID=2 firmware_smp/buildglue/build_smp.sh

# single core fallback, same task layout as the old firmware
SMP=0 NODE_ID=2 firmware_smp/buildglue/build_smp.sh
```

`NODE_ID` picks which node image to build (1 is the bridge, 2 to 5 are
field nodes), one at a time. See `firmware_smp/buildglue/README.md` for
what each build flag does and what is still missing before it will link.

### Serial commands on a curveball node

Same keys as the old firmware, plus two new ones for testing: `c` capture
now, `e` sensor readings, `s` statistics, `d` toggle 30% injected loss, `t`
toggle the WiFi topology filter, `x` fire 5 capture requests fast (to check
the queue-full behaviour), `z` stop reading the console for 5 seconds (to
check that a stalled terminal does not stall the firmware), `h` help.

### Running the T1-T8 tests

These are the test numbers from the design review. None have been run yet
(no hardware or toolchain in the session that wrote this). Each needs a
real board flashed with this branch.

- **T1** (throughput and stability): send 200 packets from a field node to
  the bridge over 2 hops, with SD writing and the dashboard connected.
  Press `s` and check the packet rate and error counts in the stats line.
- **T2** (core assignment): press `s` and check that comms and the
  USB-owner task report core 0 and sensors reports core 1 in the stats
  output.
- **T3** (audio under load): run T1 and press `c` to force a capture at
  the same time. Check the lost-sample count in the stats stays at 0.
- **T4** (DHT22 under load): run T1 and watch the DHT22 reading lines for
  valid, regular readings.
- **T5** (merged sensor task): this passes if T3 and T4 both pass, since
  it shows the merged sensors task keeps up with everything at once.
- **T6** (capture queue full): press `x` to fire 5 capture requests fast.
  The stats line should show one queue-full drop and a logged line saying
  so.
- **T7** (WiFi bridge failover): unplug the WiFi bridge module and check
  the node switches to LoRa within 10 seconds, with no gap in captures
  reaching the bridge.
- **T8** (console under load): press `z` to stop reading the console for
  5 seconds while comms and sensors keep running, then check the dropped
  log line count went up instead of anything freezing.
