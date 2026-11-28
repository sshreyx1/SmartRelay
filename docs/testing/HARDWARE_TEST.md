# Hardware test procedure

[Back to the testing index](README.md)

This procedure tests the five boards on the dual-core µT-Kernel 3 SMP firmware: boot, sensors, both
radios, capture and delivery, multi-hop relaying, loss recovery, store-and-forward and overload. It
was run on 25 November 2026; the results are in
[results/2026-11-25-five-nodes/REPORT.md](results/2026-11-25-five-nodes/REPORT.md).

Each step lists what to press, the expected console output and what counts as a pass. The tribunal
test that each step covers (T1 to T8, from the Tribunal 1 test plan) is given in brackets.

## Before you start

**How the system behaves**

- **Node 1 is the bridge.** It is plugged into a laptop and only receives. Field nodes (2 to 5) send
  only to node 1 and relay packets heading to it; they never send files to each other.
- **Nothing is sent until a field node has heard the bridge.** Node 1 sends a heartbeat every 3 s on
  both radios. A field node sends only if it heard one within the last 10 s; otherwise captures wait in
  the outbox on its SD card. Keep node 1 on for every step except Step 8.
- **Two radios, two meshes.** On a bench every LoRa node hears every other. The WiFi mesh applies a
  topology filter (on by default, `t` toggles it) to recreate the trail:

  | Node | WiFi neighbours with the filter on |
  | --- | --- |
  | 1 (bridge) | 2, 3, 4 |
  | 2 | 1, 3, 4 |
  | 3 | 1, 2, 4, 5 |
  | 4 | 1, 2, 3, 5 |
  | 5 | 3, 4 (cannot reach node 1 directly: node 3 or 4 must relay) |

- **The radio is chosen by file size.** Each capture makes two files: a 31-byte telemetry record
  (under 1024 B, so LoRa, `reason=size`) and a JPEG of about 9 to 10 KB (WiFi, `reason=size`). If
  the preferred radio cannot reach the bridge, the other is used (`reason=fallback`).
- A transfer ends as `done` (the bridge confirmed the CRC-32), `gave_up`, `refused` (bridge answered
  BUSY, STORAGE or TOO_BIG) or `io_error` (the sender's SD read failed). A file that gave up stays in
  the outbox and is retried.

**What you need**

- The five boards, each with its LoRa module, ESP-01, sensors and an SD card in the Maker Pi Pico slot.
- `uf2/csc09_smp_node_N.uf2` for each board (the dual-core images; do not mix in the FreeRTOS
  `csc09_node_N.uf2` images).
- On each field node's SD card, a `samples/` folder copied from `sd_card/samples/` (on a Mac, run
  `dot_clean /Volumes/<card>` afterwards).

**Flash and open a console**

1. Hold BOOTSEL, plug in USB, release. Copy the node's `.uf2` onto the `RPI-RP2` drive. A board
   already running this firmware reboots into BOOTSEL when it receives `B`.
2. Open the serial port in any terminal (`screen /dev/tty.usbmodemXXXX 115200` on a Mac, PuTTY on
   Windows). The board waits up to 10 s for the port at boot; press RUN to see the boot log again.
   [results/2026-11-25-five-nodes/con.py](results/2026-11-25-five-nodes/con.py) is a dependency-free
   console that timestamps lines and can send keys on a schedule.
3. Identify a board by its ESP-01 (`w` prints `WST-N`), not by the port name.

**Console keys:** `c` capture · `e` sensor readings · `s` statistics · `w` which ESP-01 · `d` toggle 30%
receive loss · `t` toggle the WiFi topology filter · `x` five captures fast · `z` stall the console 5 s
· `k` measure the 1 ms tick · `H` hold the capture queue · `p` send the next test payload · `B` reboot to
BOOTSEL · `h` help.

## Step 1 – Boot log (T2)

Expected, for node N:

```
CSC09 trail mesh, node N, ALFP v1 (uT-Kernel 3 SMP build, dual core)
msgq selftest: wanted 4, sent 4, extra accepted 0, drained 4 - ok
sensors: core 1
comms: core 0
sd: mounted, <X> sample images in <Y> groups: ...
wifi: ESP-01 MAC AC:0B:FB:..:..:.. = WST-N
node N ready (field node), outbox has <Q> queued, radio size threshold 1024 B
```

**Pass:** node number matches the board, `dual core`, message-queue self-test `ok`, sensors on core 1,
comms on core 0, SD mounted with more than 0 images, and a `node N ready` line.

| If you see | It means |
| --- | --- |
| `sd: mount failed ... retrying`, repeating | no card, not FAT32 or a bad slot. Stop until fixed. |
| `lora: init FAILED ...` or `unexpected chip type 0x00` | LoRa wiring or power. The node continues on WiFi only. |
| `wifi: WARNING this firmware is node N but the ESP-01 is WST-M` | wrong ESP-01 fitted (Step 2) |
| no `wifi: ESP-01 MAC` line | the ESP-01 did not answer (power, wiring, UART) |

## Step 2 – Which ESP-01 is fitted (`w`)

Expected: `wst: ESP-01 MAC ... = WST-N | firmware node N | MATCH`. A `MISMATCH` breaks the topology
filter: swap the ESP-01s so each board has its own, then continue.

## Step 3 – Sensors (`e`)

Expected: `[ENV] Temp: 2x.x C | Hum: xx.x% | Light: <n> | Weather: <label> | PIR 0/1 Sound 0/1`.
**Pass:** realistic temperature and humidity; PIR and Sound show 1 briefly after waving at the PIR and
clapping near the microphone. `DHT22: no reading (<n> fails)` points at the DHT22 wiring on GP4.

## Step 4 – Radio status (`s`)

Look for:

```
  dht22 good .. fail .. | lora up | wifi up
  bridge last heard on wifi <ms> ms ago
  bridge last heard on lora <ms> ms ago
  task comms: core 0 ...    task usb_owner: core 0 ...    task sensors: core 1 ...
  longest comms loop .. ms | capture q full .. | log dropped .. | usb dropped ..
  sd handles in use x/8 | no SD errors
```

**Pass:** `lora up` and `wifi up`; with node 1 on, the bridge was heard on both radios within 10 000 ms
(node 5 hears the WiFi heartbeat only through node 3 or 4); task cores as above; `no SD errors`;
`rxerr` at or near 0.

## Step 5 – Capture and send (T1, T3)

Press `c` on a field node. Expected on the field node:

```
[ALERT] Manual capture (PIR x, sound x). Recording 5s of audio...
capture NN-00000x queued (...), outbox 2
tx tel/... size=31 thr=1024 radio=lora reason=size
tx tel/...: done via lora, 1 frags, 1 rounds, 0 resent, ~1.0 s
tx samples/<image>.jpg size=<~9000-10000> thr=1024 radio=wifi reason=size
tx samples/<image>.jpg: done via wifi, <f> frags, 1 rounds, 0 resent, ~2-3 s
```

and on node 1, one line per file: `rx from node N: ..., <bytes> bytes via lora|wifi, 0 dup`.

**Pass:** both transfers `done`, telemetry over LoRa and the image over WiFi, both `reason=size`, and
node 1 shows both files. Repeat five times per node and record the times. Reference: LoRa telemetry
about 1.0 s, WiFi image about 2.4 s over one hop.

## Step 6 – Multi-hop and failover (T1, T7)

**6B (nodes 4 and 5, required).** Node 3 off, nodes 1, 4 and 5 on.

1. Press `s` on node 4 and note `wifi: ... relayed <n>`. Press `c` on node 5 a few times.
2. **Pass:** node 5's images end `done via wifi` and node 4's `relayed` counter rises. With the
   filter on, node 5 can only reach node 1 over WiFi through node 4.
3. Unplug node 4 and press `c` on node 5. **Pass:** the image goes `radio=lora reason=fallback` and is
   still `done` (slower). A file that was in flight over WiFi ends `gave_up`, stays in the outbox and is
   delivered over LoRa. Plug node 4 back in.

**6A (nodes 2 and 3, optional).** Press `s` on both before and after a few captures on each; their
`relayed` counters show them forwarding each other's packets even though both reach node 1 directly.

## Step 7 – Packet loss recovery

Press `d` on the field node **and on node 1** (`injected loss 30%` on both), then `c`.

**Pass:** both transfers still end `done`, with `rounds` above 1 and `resent` above 0: ALFP resent only
the missing fragments. Press `d` again on both boards to turn loss off.

`d` drops packets on the **receive** side of the board you press it on. Pressed only on the field node,
it drops just the bridge's acknowledgements and you will see `resent 0`.

## Step 8 – Store-and-forward with the bridge offline

1. Unplug node 1. Wait 15 s, then press `c` twice on a field node.
2. Expected: `capture ... queued ..., outbox <n>` growing by 2 per capture and no `tx` lines; `s` shows
   the bridge not heard recently.
3. Plug node 1 back in. **Pass:** within about 10 to 40 s every queued file is sent (`tx ...: done`) and
   the outbox returns to 0.

## Step 9 – Overload (T6, T8)

- `x` fires five captures quickly. **Pass:** one `[ALERT] capture queue full, dropped`, `s` shows
  `capture q full 1`, every queued capture is still delivered and nothing freezes. (T6)
- `z` stalls the console for 5 s. **Pass:** the node keeps transferring during the stall and `s`
  afterwards shows normal counters, with `log dropped` possibly increased. (T8)

## Recording results

For each node record: boot ok, SD image count, LoRa and WiFi up, the `w` result, DHT22 reading, how long
ago the bridge was heard on each radio, the five capture times, fallbacks and failures, the relay counters
from Step 6, `rounds`/`resent` from Step 7 and the outbox counts from Step 8. Copy any error line exactly.
The 25 November report shows the format.
