# Demo script: the 5-node story with 2 physical nodes

**What is real and what is not, said out loud during the demo:**

| part | real or not |
|---|---|
| Node 2's microphone, sound classifier, image choice and capture ID | **real**, on the Pico |
| Node 2 → node 1 (bridge) → laptop → dashboard | **real** radios (WiFi, LoRa as fallback) |
| Multi-hop E → C → A | **real mesh code** (`wifi_mesh.c` + ALFP, unchanged) running as 3 laptop processes; only the ESP-01 is replaced by UDP |
| Captures from nodes 3, 4, 5 on the dashboard | **simulated** (`demo_feed.py`), every card carries a **SIMULATED** badge |

About **5 min** in full; the **3-min cut** is segments 1 + 3 + 4 (skip 2 and 5).

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
- macOS: `cd backend && SMARTRELAY_BRIDGE_PORT=/dev/cu.usbmodemBRIDGE uvicorn app:app --port 8000`
- Windows: `cd backend; $env:SMARTRELAY_BRIDGE_PORT="COMb"; uvicorn app:app --port 8000`
- expect `bridge: connecting to ... automatically`.

**W3, node 2 monitor** (ml venv):
- macOS: `python -m serial.tools.miniterm /dev/cu.usbmodemNODE2 115200`
- Windows: `python -m serial.tools.miniterm COMn 115200`
- press `s`: expect `bridge last heard on wifi ... ms ago` (under about 3000). Press `h` if the screen is empty.

**W4, ready to paste:** the player for the best clip from tonight's live test (`ml/demo_clips/playlist.csv`
once `demo_playlist.py` exists; otherwise dog `5-203128-A-0.wav`):
- both OS: `python ml/playback.py ml/data/esc50/audio/5-203128-A-0.wav --device <N>`

**Dry run once** (segments 1 and 4), then clean the slate again so the counters start at the demo.

---

## Segment 1: the real path, node 2 → node 1 → laptop → dashboard (90 s)

**Say:** "This node listens, decides what it heard on the Pico itself, picks a picture for it, and sends both
through the mesh. The laptop only displays."

1. W4: start the clip. **Within half a second**, W3: press `c`. The node records 5 s.
2. W3 shows (about 6 s later):
   `[SOUND] group=MAMMAL label=DOG id=20 conf=82 ... lost=0 feat_us=... model_us=...`, then
   `capture N2-000001: heard DOG (82%) -> samples/dog_01.jpg`, `capture N2-000001 queued`,
   `tx samples/dog_01.jpg: done via wifi`.
3. Point at **W1**: the card appears with the **same `N2-000001` badge**, the dog picture, the chip
   `DOG · 82% · MAMMAL`, "identified on-node", and **no** SIMULATED badge. Open it: the transfer panel shows
   fragments, size and crc.
4. Species tab (simulated **unticked**): Dog 1.
5. Optional second clip: rain → `heard WEATHER (..%) -> samples/weather_00.jpg`. "When it is not sure of the
   animal, it says only the group rather than guessing."

| if | fallback |
|---|---|
| label is a group (e.g. MAMMAL) | that is the rejection working; say so, and carry on |
| label is wrong | "the classifier is right about X% on the test fold" (model card); play the playlist's 2nd clip |
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

W4 (backend venv, `requests` installed). Node 1 is the bridge and never captures, so the simulated nodes are 3–5:
- macOS: `for n in 3 4 5; do python simulator/demo_feed.py --once --node $n --label-images sd_card/samples; done`
- Windows: `foreach ($n in 3,4,5) { python simulator/demo_feed.py --once --node $n --label-images sd_card/samples }`

Point at W1: three new cards with the **SIMULATED** badge and `N3-…`, `N4-…`, `N5-…` IDs; node 2's real card
has none. Species tab: unticked = node 2's real labels only; tick "include simulated captures" = all of them.

| if | fallback |
|---|---|
| `pip install requests` printed | activate the backend venv, `pip install requests`, rerun |
| cards appear without images | reload the page (Cmd+Shift+R / Ctrl+Shift+R) |

## Segment 5 (optional): 3 physical nodes, real multi-hop (90 s)

**Correction to the plan's "node 3 → node 2 → node 1":** that chain cannot be forced. In Goutham's layout
(`third_party/wifi_mesh/wifi_mesh_config.h`) C (3) hears A (1) directly, and the only node that cannot hear A is
E (5), whose neighbours are C and D. The radio code is not ours to edit, so the real 3-node chain is
**E (5) → C (3) → A (1)**. It needs boards with **WST-5 and WST-3**, flashed `csc09_node_5.uf2` and
`csc09_node_3.uf2`, plus node 1. Node 2 then sits out (or stays on as an extra neighbour of C).

1. All nodes powered, the topology filter is ON at boot (`t` toggles it; it prints the new state).
2. Node 5's monitor: `c`. Node 3's monitor: `s` → the `wifi: ... relayed N` count goes up.
3. Dashboard: an `N5-…` card **without** SIMULATED.
4. Press `t` on every node (filter OFF): the next `c` on node 5 arrives directly; node 3's `relayed` stays put.

| if | fallback |
|---|---|
| node 5 says `bridge never heard on wifi` | node 3 is off or not WST-3; check its boot line `wifi: ESP-01 MAC ... = WST-3` |
| it went via LoRa | LoRa has no filter (on a desk everyone hears everyone); that is still a real delivery. Use segment 3 for the hop |

---

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
| 5 three nodes | 1:30 | — |
| total | about 5:30 | 3:00 |
