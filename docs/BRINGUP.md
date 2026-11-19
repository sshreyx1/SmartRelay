# CSC09 node firmware: wiring, build, flash and test

One firmware image now carries everyone's code:

| Member | Code | Where it lives | Changed? |
| --- | --- | --- | --- |
| Mridula (LoRa mesh) | `lora_mesh.c`, `lora_radio.c`, LR1121 driver | `third_party/lora_mesh/` | Only `gpio.h`: the pin defines are wrapped in `#ifndef` so BUSY and DIO9 can move. Defaults unchanged. |
| Goutham (WiFi mesh) | `wifi_mesh.c`, `wst_link.c`, `wifi_mesh_config.h` | `third_party/wifi_mesh/` | No |
| Rhaylene (sound) | `sound.c`, `mic.c` | `third_party/sensors/` | No |
| Maryam (environment) | `pir.c`, `DHT22.c`, `weather_tree.h` | `third_party/sensors/` | No |
| Shreya (ALFP, SD, integration) | `alfp/`, `firmware/` | | |

What a field node does: every 2 s it reads the DHT22 and the light sensor and runs Maryam's weather
tree. When the PIR and the sound trigger are high at the same moment (the rule from their
`main.c`), it records 5 s of audio, runs Rhaylene's sound classifier, packs everything into the
27-byte capture record and queues the next sample image from the SD card. ALFP sends the image
plus the record to node 1 over WiFi if WiFi can hear the bridge, otherwise over LoRa, and repairs
any lost fragments. Node 1 checks the CRC-32, saves the file and streams it to the laptop.

---

## 1. Wiring (every node is the same)

### The one change you must make

**Move the LoRa DIO9 wire from GP15 to GP7.** On the Maker Pi Pico, GP15 is hard-wired to the
microSD slot's chip select. With DIO9 on GP15 the LoRa chip and the Pico both drive that pin, the
SD card never answers, and the two outputs fight each other. The firmware is built for DIO9 on
GP7 and refuses to build for GP15.

(BUSY on GP6 is already right: you moved it off GP14, which is now the sound trigger.)

### Full pin map

| Pin | Connects to | Owner |
| --- | --- | --- |
| GP0 | PIR OUT (Grove port 1). PIR VCC from VBUS | Maryam |
| GP4 | DHT22 DATA. VCC 3.3 V (pin 36), GND (pin 38) | Maryam |
| GP5 | LoRa RESET | Mridula |
| GP6 | LoRa BUSY | Mridula |
| **GP7** | **LoRa DIO9 (IRQ), moved from GP15** | Mridula |
| GP10 | LoRa SCK, shared with the SD slot | shared SPI1 |
| GP11 | LoRa MOSI, shared with the SD slot | shared SPI1 |
| GP12 | LoRa MISO, shared with the SD slot | shared SPI1 |
| GP13 | LoRa CS (NSS) | Mridula |
| GP14 | Sound trigger OUT | Rhaylene |
| GP15 | microSD CS (the Maker slot, nothing to wire) | Shreya |
| GP16 / GP17 | ESP-01 (WST) in the Maker ESP-01 socket, UART0 | Goutham |
| GP20 | Maker button 1 = "capture now" for demos (on the board) | |
| GP26 | MAX9814 mic analog OUT, through the RC filter | Rhaylene |
| GP27 | Light module (SN-LIGHT-MOD) AO | Maryam |
| 3V3 / GND rails | LoRa, DHT22, mic, light, sound trigger | |

GP26 for the mic comes from Rhaylene's `mic.h` (`MIC_ADC_PIN 26`). Check that is where her mic
output actually goes.

### Why sharing SPI1 is safe

The SD card and the LoRa module use the same three SPI wires and have separate chip selects
(GP15 and GP13). Only one task in the firmware (comms) ever talks to either chip, so they never
talk at the same time. At power-up the firmware drives both chip selects high, mounts the SD card
first (so the card is in SPI mode before LoRa traffic appears), then starts LoRa, and runs the bus
at 1 MHz, the speed Mridula tested the LR1121 at. GP13 and GP14 are also the SD slot's DAT1 and
DAT2 lines, which a card ignores in SPI mode.

### Each node also needs

- the **antenna on the Core1121 before powering on**
- the **right ESP-01** in its socket: WST-1 on node 1 (A), WST-2 on node 2 (B), and so on.
  Goutham's topology filter identifies senders by the ESP-01's MAC, so a swapped ESP-01 breaks
  WiFi routing. The firmware prints a warning if they don't match.
- a **FAT32 microSD card** in the Maker slot (32 GB or smaller)

---

## 2. Prepare the SD cards

- **Node 1 (bridge):** an empty FAT32 card. Received files go into `rcv/<xx>/` (xx = the session id's
  high byte, so no folder grows large; firmware before 2026-11-19 used one flat `inbox/`, which can be deleted).
- **Nodes 2 to 5:** a `samples/` folder of small images. They stand in for camera frames, and the
  node picks one **from what it heard** (`firmware/sensing/sound_image_pick.c`): it looks for
  `samples/<label>_NN.jpg` (e.g. `dog_00.jpg`), then the label's group (`bird_00.jpg`,
  `mammal_00.jpg`, ...), then the generic `img_NN.jpg`, round-robin within each. The ready-made
  label images (free-licence, credits in `docs/sound/IMAGE_CREDITS.md`) are in `sd_card/samples/`
  in the repo; copy that folder to the top of each card and keep the old `img_NN.jpg` files too.
  To make generic images from your own photos:

  ```bash
  pip install pillow
  python3 tools/make_sample_images.py my_photos/*.jpg --out sd_card/samples
  ```

  (label images: `python tools/make_label_images.py`, see `docs/sound/HANDOFF.md`). 160x120 JPEGs
  are 3 to 6 kB, the label images up to 10 kB: about 1 s over WiFi, 30 to 70 s over LoRa.
  Anything up to 120 kB works. The node also keeps `meta/next_id` on the card so capture ids
  (`N2-000042`) never restart after a reboot; do not delete it between demo runs.

---

## 3. Get the .uf2 files

### Option A: use the ones I built (no setup)

The `uf2/` folder has everything ready to flash:

| File | Flash it on | What it is |
| --- | --- | --- |
| `csc09_node_1.uf2` | node A (bridge, plugged into the laptop) | full firmware, receives |
| `csc09_node_2.uf2` ... `csc09_node_5.uf2` | nodes B to E | full firmware, field nodes |
| `sensor_test.uf2` | any node | sensors only, no radios, no SD |
| `alfp_selftest.uf2` | any node | SD card + the protocol, no radios |
| `lora_photo_rx.uf2` / `lora_photo_tx.uf2` | two nodes | LoRa-only photo test |

The node number is baked into the file, so `csc09_node_3.uf2` always runs as node 3 (C).

### Option B: build them yourself (after changing code)

You need a Linux, Mac or WSL (Ubuntu on Windows) terminal. On Windows, install WSL once with
`wsl --install` in PowerShell, reboot, then open "Ubuntu" from the Start menu.

```bash
# one time: compiler and tools
sudo apt update && sudo apt install -y cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential git python3
#   (Mac: brew install cmake && brew install --cask gcc-arm-embedded)

# one time: unzip the project and fetch the pinned SDK, FreeRTOS and SD-card library
unzip csc09_alfp.zip && cd alfp
./tools/fetch_deps.sh                    # about 2 minutes, prints three export lines

# every new terminal: paste the three export lines it printed, e.g.
export PICO_SDK_PATH=$PWD/deps/pico-sdk
export FREERTOS_KERNEL_PATH=$PWD/deps/FreeRTOS-Kernel
export FATFS_PICO_PATH=$PWD/deps/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico

# build everything (first time about 3 minutes)
cmake -S firmware -B build-fw
cmake --build build-fw -j4
ls build-fw/*.uf2
```

After editing code, run only `cmake --build build-fw -j4` again. In WSL the files are at
`\\wsl$\Ubuntu\home\<you>\alfp\build-fw` in Windows Explorer, so you can drag them from there.

Build options (add to the first `cmake` line, e.g. `cmake -S firmware -B build-fw -DAUDIO_SECONDS=3`):

| Option | Default | When to change it |
| --- | --- | --- |
| `-DLORA_IRQ_PIN=7` | 7 | DIO9 wired to a different free pin |
| `-DLORA_BUSY_PIN=6` | 6 | BUSY wired to a different pin |
| `-DSPI1_BAUD_HZ=4000000` | 4 MHz (since 2026-11-19) | back to 1000000 for a board whose LoRa or SD fails at 4 MHz (long SPI wires); SMP build: `SPI1_BAUD_HZ=1000000 NODE_IDS=N firmware_smp/buildglue/build_smp.sh` |
| `-DAUDIO_SECONDS=5` | 5 | shorter recording per capture |
| `-DALFP_LORA_GAP_MS=500` | 500 | use 1000 if the bridge is 3+ LoRa hops away |
| `-DUSE_WIFI_MESH=OFF` / `-DUSE_LORA_MESH=OFF` | ON | build without one radio |
| `-DNODE_IDS="2"` | 1;2;3;4;5 | build only some node images (faster) |

Settings that live in the members' own files: LoRa frequency and power in
`third_party/lora_mesh/lora/lora_radio.h` (`LORA_TX_POWER_DBM`, lower it to force multi-hop on a
desk), WiFi MAC table and demo layout in `third_party/wifi_mesh/wifi_mesh_config.h`, sound
thresholds in `firmware/sensing/audio_features.h` (copied from `mic.c`), weather tree in
`third_party/sensors/weather_tree.h`.

---

## 4. Flash a Pico

1. Unplug the Pico.
2. Hold the **BOOTSEL** button on the Pico W (the white button on the Pico itself, not the Maker
   board buttons).
3. Plug the USB cable into the laptop while still holding it, then let go.
4. A drive called **RPI-RP2** appears.
5. Drag the `.uf2` onto it. The drive disappears by itself and the Pico restarts with the new
   firmware. That is normal, not an error.

To see what it prints, open a serial monitor at 115200 baud:

- VS Code: Serial Monitor extension, pick the port, 115200.
- Terminal: `pip install pyserial`, then `python3 -m serial.tools.miniterm COM5 115200`
  (Mac `/dev/tty.usbmodem...`, Linux `/dev/ttyACM0`). Ctrl+] quits.
- Windows: Device Manager > Ports shows which COM number the Pico got.

---

## 5. Bring it up in stages

Do these in order. Each stage only adds one thing, so when something fails you know which part it is.

### Stage 1: sensors (`sensor_test.uf2`, one node at a time)

Flash it, open the serial monitor. Every second:

```
PIR 0 | sound 0 | light 2310 | 29.4 C 78.2 %RH | weather Hot_Dry | dht ok 12 fail 0
```

Check:

- [ ] wave at the PIR: `PIR 1` (it stays high for a few seconds)
- [ ] clap near the mic: `sound 1`
- [ ] cover the light sensor: the number changes a lot
- [ ] DHT22 temperature and humidity look sensible and `fail` stays low
- [ ] PIR and clap together: `[ALERT] PIR + sound together`, then 5 s later an `[AUDIO]` line
- [ ] press `q` in the monitor: a 1 s mic reading. **Look at `mean ADC`.** Rhaylene's code
  subtracts 2048 (1.65 V) from every sample. If the mean is far from 2048 (for example 1550,
  which is what a MAX9814's usual 1.25 V bias gives), the energy includes that offset and a quiet
  room will be classed LOUD. The test prints a note when this happens: show it to Rhaylene.

### Stage 2: SD card + protocol (`alfp_selftest.uf2`)

With the LoRa module still wired (it shares the bus, so this also proves it stays quiet). Ends
with `SELFTEST PASSED (3/3)` and the SD read/write speed. `sd: mount failed` means: card not
FAT32, card not fully pushed in, or the DIO9 wire is still on GP15.

### Stage 3: LoRa between two Picos (`lora_photo_rx.uf2` + `lora_photo_tx.uf2`)

Proves Mridula's radio on the new pins with the shared SD card. Follow `docs/LORA_PHOTO_TEST.md`.
Pass = the CRC-32 printed by both Picos is identical and the photo opens on the laptop.

### Stage 4: full firmware, two nodes (node 1 + node 2)

1. Flash `csc09_node_1.uf2` on the node with WST-1 and `csc09_node_2.uf2` on the node with WST-2.
2. On the laptop, start the receiver on node 1's port and leave it running:

   ```bash
   pip install pyserial
   python3 tools/bridge_receiver.py --port COM5
   ```

3. Open node 2's port in a serial monitor.

Node 2 should print (numbers will differ):

```
CSC09 trail mesh, node 2, ALFP v1
sd: mounted, 3 sample images
lora_radio: LR1121 firmware 0x0103, hardware 0x22
lora_radio: 924200000 Hz, SF7 BW125 CR4/5, 14 dBm, CRC on
wifi: ESP-01 MAC AC:0B:FB:C8:19:96 = WST-2
node 2 ready (field node), outbox has 0 queued
sensors: PIR GP0, sound GP14, mic GP26, DHT22 GP4, light GP27, button GP20
[ENV] Temp: 29.4 C | Hum: 78.2% | Light: 2310 | Weather: Hot_Dry | PIR 0 Sound 0
```

Then:

- [ ] press `s` on node 2: `bridge last heard on wifi ... ms ago` and `on lora ... ms ago`, both
  under about 3000. Both radios can reach the bridge.
- [ ] press `c` on node 2 (or the GP20 button, or trigger PIR + sound): after the 5 s recording,
  `[SOUND] group=... label=... conf=... lost=0`, `capture N2-000001: heard DOG (82%) -> samples/dog_00.jpg`,
  `capture N2-000001 queued`, then `tx samples/dog_00.jpg: done via wifi`.
- [ ] the laptop prints `saved received/n2_<session>_dog_00.jpg (... bytes via wifi, crc ok) | N2-000001: DOG 82%, Hot_Dry, 29.4 C`
  and the `.json` next to it holds every sensor value (capture number, triggers, temperature,
  humidity, light, weather, the node's sound label + group + confidence, RMS, crossings per second).
- [ ] **LoRa fallback:** unplug node 2, take its ESP-01 out, plug node 2 back in (it prints
  `wifi: the ESP-01 did not answer`), press `c`. Now it says `done via lora` and takes about 30 s
  for a small image. Put the ESP-01 back afterwards (with the power off).
- [ ] **Repair:** press `d` on node 1 (drops 30% of what it hears), press `c` on node 2. The
  transfer shows `rounds` above 1 and some `resent`, and the laptop still says `crc ok`.
- [ ] **Store and forward:** power node 1 off, press `c` twice on node 2 (`outbox 2`), power node 1
  back on. Both captures arrive by themselves within a minute.

### Stage 5: all five nodes

Flash `csc09_node_3/4/5.uf2` on the nodes with WST-3/4/5. Goutham's topology filter is ON, so
over WiFi node E (5) cannot hear A directly and every packet is relayed through C or D: press `c`
on node 5 and `s` on node 3 or 4 to see `relayed` go up. Press `t` on every node to turn the filter
off (everyone hears everyone). LoRa has no filter: on a desk all nodes hear each other directly.
Lower `LORA_TX_POWER_DBM` in Mridula's `lora_radio.h` if you need real LoRa hops.

---

## 6. Serial keys on a node

| Key | Does |
| --- | --- |
| `c` | capture now (records audio, reads sensors, sends the next sample image) |
| `e` | print the sensor readings now |
| `s` | statistics: ALFP, both radios, bridge last heard, DHT22, heap |
| `d` | toggle 30% injected packet loss on this node (demo of the repair) |
| `t` | toggle Goutham's WiFi topology filter |
| `h` | help |

---

## 7. If something is wrong

| You see | Most likely |
| --- | --- |
| `sd: mount failed` repeating | card not FAT32 or not seated; DIO9 still on GP15 |
| `lora: init FAILED` (about 10 s after start) | BUSY not on GP6 or DIO9 not on GP7, RESET/CS loose, no 3.3 V. Get stage 3 working first. The node carries on with WiFi only |
| `wifi: the ESP-01 did not answer` | ESP-01 not fully in the socket; it then falls back to LoRa |
| `wifi: WARNING this firmware is node 2 but the ESP-01 is WST-4` | flash `csc09_node_4.uf2` on this board, or swap the ESP-01 |
| `bridge never heard on wifi` | node 1 off, or (topology filter ON) this node is not a neighbour of A and no relay node is running |
| `capture N2-...: no samples/*_NN.jpg on the SD card` | the `samples` folder is missing on that card |
| DHT22 `fail` keeps climbing | loose data wire on GP4, or no pull-up on a bare sensor |
| `[AUDIO] warning: samples lost` | should not happen; tell Shreya |
| Nothing at all on the serial monitor | wrong COM port. USB serial drops anything printed before the monitor opened, so a late monitor misses the start-up lines: press `h` or `s` |

---

## 8. What changed from each member's original code, and why

- **Rhaylene, mic recording.** `mic_record_buffer()` needs a 160 kB buffer (5 s x 16 kHz x 2 bytes),
  60% of the Pico's RAM, which does not fit next to two radio stacks, FreeRTOS and the SD driver.
  The firmware records the same 5 s at the same 16 kHz, but the ADC is paced by its own clock and
  DMA streams the samples through a 16 kB ring while `audio_features.c` computes her exact energy,
  zero-crossing rate and decision tree. `host/tests/test_audio.c` runs her unchanged
  `mic_analyze_buffer()` and the streaming version on the same data in 30 cases: the numbers are
  identical. On the RP2040 emulator the capture took exactly 16000 samples per second with none lost.
  The raw audio is not stored or sent (160 kB would take about 20 minutes over LoRa); its features go
  with every capture.
- **Rhaylene, classifier observations for her to look at:** quiet white noise around 2048 comes out
  as `HIGH_FREQ_RUSTLE` (ADC noise crosses the mid-line often, so zcr > 0.15), and any DC offset
  from 2048 comes out as `LOUD_ANIMAL_CALL`. Both come straight from the thresholds, not the integration.
- **Maryam, DHT22.** `DHT_read()` times the sensor's bits by counting loop iterations, so it must
  not be interrupted. The firmware raises the sensors task above the radio task for those few
  milliseconds, and builds `DHT22.c` with its 40 us `sleep_us` turned into a busy wait (under
  FreeRTOS a sleep can overrun by tens of microseconds, right when the sensor starts answering).
  Her source file is unchanged; both are build settings.
- **Both, the 16-byte `LoRaMeshPacket` and `lora.c`.** Replaced by the capture record that rides
  inside every ALFP transfer (`common/capture_meta.h`), so sensor data and image always arrive
  together and verified. `lora.c` was a placeholder; Mridula's real driver is used. (The old packet
  also put the energy into a `uint16_t`, which overflows above 65535; the record carries the RMS,
  which fits.)
- **Fields still "unknown" in every record:** `weather_conf`, `trigger_trust` and `sound_conf`.
  Neither tree outputs a confidence. If Maryam or Rhaylene add one, it is one line each in
  `firmware/sensing/sensing.c`.
- **Mridula, pins.** BUSY GP14 to GP6 and DIO9 GP15 to GP7, passed in from CMake. Her driver runs
  SPI1 at 1 MHz; the SD card now runs at the same speed on the same bus.
- **Goutham.** Used as is: `wifi_mesh.c` and `wst_link.c`. His `bridge.c` is not used, because node
  1's firmware already forwards finished files to the laptop (`tools/bridge_receiver.py`).

## 9. Tested before handing over

| Test | Result |
| --- | --- |
| All firmware images build with the team's code (`cmake --build`) | 9 .uf2 files, no warnings in our code |
| Laptop test suite (`ctest`, 7 suites, sanitizers on) | all pass |
| Mridula's real `lora_mesh.c` + ALFP, two processes, 20% loss | file byte-for-byte identical |
| Goutham's real `wifi_mesh.c` + ALFP, three processes, E to C to A through his topology filter, 0/10/20/35% loss per hop | file byte-for-byte identical every time |
| Rhaylene's `mic_analyze_buffer()` vs the streaming version, 30 cases | identical |
| DMA audio capture on the RP2040 emulator (rp2040js), 1 kHz test tone | 16000 samples in 1.001 s, 0 lost, energy and crossings match the maths, `adc_read()` for the light sensor still works after. With a forced 0.7 s stall the lost-sample counter is exact |
| Code review of the integration by a second reviewer | 4 fixes applied (comms loop yield, lost-sample count, DHT22 start pulse, WiFi status line) |

Still to be tested on the real boards: everything in stage 1 to 5 above.
