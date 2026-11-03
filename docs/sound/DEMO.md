# Demo script: the 5-node story with 2 physical nodes

**What is real and what is not, said out loud during the demo:**

| part | real or not |
|---|---|
| Node 2's microphone, sound classifier, image choice and capture ID | **real**, on the Pico |
| Node 2 → node 1 (bridge) → laptop → dashboard | **real** radios (WiFi, LoRa as fallback) |
| Multi-hop E → C → A | **real mesh code** (`wifi_mesh.c` + ALFP, unchanged) running as 3 laptop processes; only the ESP-01 is replaced by UDP |
| Captures from nodes 3, 4, 5 on the dashboard | **simulated** (`demo_feed.py`), every card carries a **SIMULATED** badge |

About **5:30** in full; the **3-min cut** is segments 1 + 3 + 4 (skip 2 and 5). If H12c passed, swap segment 3 for 5 in the cut (the real hop beats the laptop run).

---

## Before the demo (T-15 min)

**Hardware:** node 2 (MAX9814 mic, PIR, SD card with `sd_card/samples/` copied on, ESP-01 WST-2, LoRa) and node 1
(bridge, ESP-01 WST-1, LoRa, SD card), both flashed with tonight's `csc09_node_2.uf2` / `csc09_node_1.uf2`.
Speaker about 10–20 cm from node 2's mic (the distance from `baseline.md`). Laptop on the charger, volume up,
notifications off (macOS: Focus → Do Not Disturb; Windows: Focus assist → Alarms only).

**Clean slate** (stop the backend first):
- macOS: `rm -f backend/smartrelay.db backend/received/n* backend/received/demo_*`
- Windows: `Remove-Item backend\smartrelay.db, backend\received\n*, backend\received\demo_* -ErrorAction SilentlyContinue`

**Windows on screen** (arrange once, do not move them during the demo):

| # | window | started with |
|---|---|---|
| W1 | browser, http://localhost:8000 (Home), Species page in a second tab | — |
| W2 | terminal: **backend** (owns node 1's USB port) | see below |
| W3 | terminal: **node 2 serial monitor** (the `[SOUND]` and `capture N2-…` lines) | see below |
| W4 | terminal: **player + simulator + multi-hop** (ml venv active) | see below |

Find the two ports: `python -m serial.tools.list_ports -v` (both are VID 2E8A; plug them in one at a time to tell
them apart). Below, `BRIDGE` = node 1's port and `NODE2` = node 2's port.

**W2, backend** (backend venv):
- macOS: `cd backend && source .venv/bin/activate && SMARTRELAY_BRIDGE_PORT=/dev/cu.usbmodemBRIDGE uvicorn app:app --port 8000`
  (e.g. `/dev/cu.usbmodem101` on R's Mac on 2026-11-04; `python tools/serial_log.py --identify` tells which port is node 1).
  The venv matters: `which uvicorn` must point into `backend/.venv`. The system Python's uvicorn has no pyserial, and
  the backend then says the bridge port is set but never connects.
- Windows: `cd backend; $env:SMARTRELAY_BRIDGE_PORT="COMb"; uvicorn app:app --port 8000`
- expect `bridge: connecting to ... automatically`.

**W3, node 2 monitor** (ml venv):
- macOS: `python tools/serial_log.py --port /dev/cu.usbmodemNODE2 --test demo` (or `python -m serial.tools.miniterm /dev/cu.usbmodemNODE2 115200`)
- Windows: `python -m serial.tools.miniterm COMn 115200`
- press `s`: expect `bridge last heard on wifi ... ms ago` (under about 3000). Press `h` if the screen is empty.

**W4, ready to paste:** the player for the best clip from tonight's live test (`ml/demo_clips/playlist.csv`
once `demo_playlist.py` exists; otherwise dog `5-203128-A-0.wav`):
- both OS: `python ml/playback.py ml/data/esc50/audio/5-203128-A-0.wav --device "MacBook Pro Speakers"` (by name: the numbers change when an iPhone connects)

**Dry run once** (segments 1 and 4), then clean the slate again so the counters start at the demo.

---

## Segment 1: the real path, node 2 → node 1 → laptop → dashboard (90 s)

**Say:** "This node listens, decides what it heard on the Pico itself, picks a picture for it, and sends both
through the mesh. The laptop only displays."

1. W4: start the playlist's first clip (`ml/demo_clips/playlist.csv`, from the latest bench). **Within half a
   second**, W3: press `c`. The node records 5 s.
2. W3 shows (about 6 s later), e.g. for a cat:
   `[SOUND] group=MAMMAL label=MAMMAL id=2 conf=58 ... lost=0 feat_us=... model_us=...`, then
   `capture N2-000001: heard MAMMAL (58%) -> samples/mammal_00.jpg`, `capture N2-000001 queued`,
   `tx samples/mammal_00.jpg: done via wifi`.
3. Point at **W1**: the card appears with the **same `N2-000001` badge**, the mammal picture, the chip
   `MAMMAL (unidentified) · 58%` (group only), "identified on-node", and **no** SIMULATED badge. Open it: the transfer panel shows
   fragments, size and crc.
4. Species tab (simulated **unticked**): Mammal 1.
5. Optional second clip from another group (weather or bird). **Say:** "It names the group, not the animal: with
   20 recordings per animal the animal names were wrong half the time, so the node refuses to guess (model card).
   The old threshold detector got animal vs not-animal right 47% of the time; this one 81%."

| if | fallback |
|---|---|
| wrong group | "about half the groups are right on new recordings" (model card, bench); play the playlist's next clip |
| no `done via wifi` within 30 s | press `s` on W3. If `bridge never heard on wifi`, it falls back to LoRa by itself (about 30 s for a small image): keep talking through segment 3 meanwhile |
| nothing reaches the dashboard | W2 shows the bridge's lines? If not, the backend lost the port: Ctrl+C, start it again. Last resort: play **H11's video** |

## Segment 2 (optional): dual-radio failover (60 s; only if BOTH nodes have working LoRa)

Check first, on W3: `s` shows `lora up` and `bridge last heard on lora ... ms ago`. If either is missing,
**skip this segment.**

The node prefers WiFi and uses LoRa when it has not heard the bridge on WiFi for 10 s (`BRIDGE_FRESH_MS`).
**Never pull an ESP-01 with the power on.** Two safe ways to show it:

- **A, prepared (recommended):** before the demo, power node 2 off, take out its ESP-01, power it on
  (`wifi: the ESP-01 did not answer`). In the demo: `c` + clip → `tx ...: done via lora` after about 30 s;
  the dashboard card shows the `lora` chip. **Say:** "Same capture, same ID, other radio. Nothing to do on the
  laptop." Put the ESP-01 back with the power off afterwards.
- **B, store and forward (no unplugging):** power node 1 off (unplug its USB; the backend logs a disconnect),
  press `c` twice on node 2 (`outbox 2`), plug node 1 back in and restart W2. Both captures arrive by themselves
  within about a minute, IDs in order.

| if | fallback |
|---|---|
| `done via lora` takes over 60 s | carry on with segment 3; the card appears when it arrives |
| LoRa not up | skip; mention T7 in `README.md` instead |

## Segment 3: multi-hop E → C → A (45 s)

**Say:** "On the trail, node E is too far from the base station. Its photo hops through C. Here are three
copies of our real mesh and transfer code running on the laptop, with the radio replaced by a network socket,
and the trail layout switched on."

W4:
- macOS / Linux: `python tools/demo_multihop.py`
- Windows: inside WSL (`wsl`, then `python3 tools/demo_multihop.py`); the fake radio uses POSIX sockets.
- Harder version: `python tools/demo_multihop.py --loss 0.1` (10% of packets lost: watch `resent` and `rounds`).

It runs about 10 s and ends with:
```
 HOP PATH   E (node 5)  --wifi-->  C (node 3, relay)  --wifi-->  A (node 1, bridge)
 relay C    relayed 49 packets (E's data towards A, A's ACKs back to E), ...
 bridge A   delivered 45 packets to ALFP; ignored 45 packets heard straight from E (not a neighbour)
 result     PASS: the file at A is byte-for-byte the photo E sent
```
**Point at:** "ignored 45 packets heard straight from E": A heard E but the layout says they are not
neighbours, so every packet had to come through C.

| if | fallback |
|---|---|
| `bind: Address already in use` | a previous run is still alive: macOS `pkill integ_node_wifi`, then run again |
| no compiler / build fails | `bash host/integration/run_wifi_integration.sh` (same test, shorter output), or the screenshot in `docs/sound/evidence/` |
| FAIL | run again without `--loss` |

## Segment 4: the dashboard, real next to simulated (45 s)

**Say:** "We have two physical nodes tonight. Nodes 3 to 5 are simulated so you can see the whole trail on the
dashboard; every simulated card says so."

W4 (backend venv, `requests` installed). Simulate **only nodes that are not physically there** (never next to a
real node's own IDs); node 1 is the bridge and never captures. With only node 2 real, that is 3–5; if nodes 3 and 5
are real (H12), simulate node 4 only (`--node 4`):
- macOS: `for n in 3 4 5; do python simulator/demo_feed.py --once --node $n --label-images sd_card/samples; done`
- Windows: `foreach ($n in 3,4,5) { python simulator/demo_feed.py --once --node $n --label-images sd_card/samples }`

Point at W1: three new cards with the **SIMULATED** badge and `N3-…`, `N4-…`, `N5-…` IDs; node 2's real card
has none. Species tab: unticked = node 2's real labels only; tick "include simulated captures" = all of them.

| if | fallback |
|---|---|
| `pip install requests` printed | activate the backend venv, `pip install requests`, rerun |
| cards appear without images | reload the page (Cmd+Shift+R / Ctrl+Shift+R) |

## Segment 5: the real two-hop, node 5 → node 3 → node 1 (90 s; runs only if H12c passed)

**Use this segment only if `HARDWARE_TODO.md` H12c passed tonight** (node 5's `done via wifi` and node 3's
`relayed` going up). Otherwise use the fallback at the bottom of this segment.

Why 5 → 3 → 1 and not 3 → 2 → 1: in Goutham's layout (`wifi_mesh_config.h`) node 3 hears node 1 directly, and only
node 5 (E) cannot (its neighbours are 3 and 4). See DECISIONS D10.

**Before:** nodes 1, 3, 5 powered, flashed `csc09_node_1/3/5.uf2`, ESP-01s WST-1/3/5; node 4 off; no `t` pressed
since boot (the filter is ON at boot). The backend runs on the bridge's port (H12 setup), and a logger is open on
node 3 and on node 5 (`python tools/serial_log.py --port <NODE3> --test demo_node3`, same for node 5).

**Say:** "On the trail, node 5 is out of the base station's range. Watch node 3 pass its photo along."

1. Node 3's logger: `s`. Read out `wifi: ... relayed R`.
2. Node 5's logger: `c` (play a playlist clip first if you like). After about 10 s: `done via wifi`.
3. Node 3's logger: `s`. `relayed` went up by about 40-50, which is node 5's photo passing through.
4. Dashboard: an `N5-…` card, **without** SIMULATED.
5. Optional (30 s): press `t` on nodes 1, 3 and 5 (filter OFF, everyone hears everyone), `c` on node 5 again: node 3's
   `relayed` barely moves. Press `t` on all three again afterwards.

| if | fallback |
|---|---|
| node 5 says `bridge never heard on wifi` | node 3 is off or not WST-3 (boot line `wifi: ESP-01 MAC ... = WST-3`); then use the fallback below |
| `done via lora` | LoRa has no filter, so it went straight to the bridge: a real delivery, but not the hop. Say so, then use the fallback below |
| H12c did not pass tonight | **fallback:** segment 3's laptop run (`python tools/demo_multihop.py`) shows the same 5 → 3 → 1 path with the real mesh code, and `demo_feed.py --once --node 5` puts an N5 card (SIMULATED) on the dashboard |

---|---|
| node 5 says `bridge never heard on wifi` | node 3 is off or not WST-3; check its boot line `wifi: ESP-01 MAC ... = WST-3` |
| it went via LoRa | LoRa has no filter (on a desk everyone hears everyone); that is still a real delivery. Use segment 3 for the hop |

---

## If the classifier misbehaves

Build and flash the legacy firmware (curveball's mic.c thresholds, round-robin `img_NN.jpg`; the SD card needs its old
`img_NN.jpg`): `cmake -S firmware -B build-fw-legacy -G Ninja -DSOUND_LEGACY_MODE=ON && cmake --build build-fw-legacy`,
then flash `build-fw-legacy/csc09_node_2.uf2`. The boot line says `sound: LEGACY mode`. Captures arrive as AMBIENT or
Unidentified, no animal labels. Say so: "this is the old threshold detector we replaced".

## If everything fails

- Radios misbehave: show node 2's `[SOUND]` line on W3, plus `python ml/pico_audio.py --test 5 --features` with
  `audio_dump.uf2` (the classification still runs on the Pico, no radio needed).
- Nothing works: play the **H11** screen recording of tonight's real run, then
  `python simulator/demo_feed.py --continuous --every 10 --label-images sd_card/samples`. Every card says
  SIMULATED; say so.

## Timings

| segment | full | 3-min cut |
|---|---|---|
| 1 real path | 1:30 | 1:30 |
| 2 failover | 1:00 | — |
| 3 multi-hop | 0:45 | 0:45 |
| 4 dashboard | 0:45 | 0:45 |
| 5 real two-hop (if H12c passed) | 1:30 | (replaces 3) |
| total | about 5:30 | 3:00 |
