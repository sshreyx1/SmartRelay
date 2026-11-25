# SmartRelay node communication test (paste this into your AI chat)

> **For the AI assistant reading this:** you are helping a student test SmartRelay hardware.
> SmartRelay is a five-node Raspberry Pi Pico W mesh. Your job is to walk the student through the
> steps below **one at a time**, ask them to paste the serial console output after each step,
> compare it to the "Expected" output, and say clearly **PASS** or **FAIL** with the reason.
> Do not guess. If a line you need is missing, ask for it. At the end, fill in the report template
> in section 6. Do not suggest editing firmware source code; this is a test session, not a dev
> session. If something fails, record it and move on to the next step unless the step says STOP.

Group A tests **node 2 and node 3**. Group B tests **node 4 and node 5**.

---

## 1. How the system works (read this first)

- **Node 1 is the bridge.** It is plugged into a laptop and only receives. Field nodes (2-5) send
  **only to node 1**. Field nodes never send files to each other; they only **relay** (forward)
  packets that are heading to node 1.
- **Without node 1 powered on and in range, nothing is sent.** A field node waits until it has
  heard the bridge's heartbeat (every 3 s) within the last 10 s. Until then, captures wait on the
  node's SD card in an "outbox" and are sent later. This is expected behaviour, not a fault.
  -> **Each group needs node 1 running** (it can be one shared bridge on one laptop for both
  groups, in the same room). Ask the student whether node 1 is on before starting section 5.
- **Two radios, two separate meshes:**
  - **LoRa** (Core1121 / LR1121 module on SPI). Every node hears every other node in a lab.
  - **WiFi** (ESP-01 module on UART, called a "WST"). Each ESP-01 has a fixed MAC that belongs to
    one node number (WST-1 ... WST-5). A **topology filter is ON by default** to fake a trail
    layout, so only these neighbours hear each other over WiFi:

    | Node | WiFi neighbours (filter ON) |
    |---|---|
    | 1 (bridge) | 2, 3, 4 |
    | 2 | 1, 3, 4 |
    | 3 | 1, 2, 4, 5 |
    | 4 | 1, 2, 3, 5 |
    | 5 | 3, 4 (**cannot reach node 1 directly; must be relayed by node 3 or 4**) |

- **Radio is chosen by file size:** each capture produces two files:
  - a small **telemetry** file (`tel/...`, under 1024 B) -> sent over **LoRa** (`reason=size`)
  - a **JPEG image** (1024 B or more) -> sent over **WiFi** (`reason=size`)
  - If the preferred radio cannot reach the bridge, the other one is used (`reason=fallback`).
- **ALFP** is our transfer protocol: it splits a file into fragments, checks CRCs, and resends
  only missing fragments. A transfer ends as `done` (bridge confirmed the CRC), `gave_up`,
  `refused` (bridge said BUSY / STORAGE / TOO_BIG) or `io_error` (local SD read failed).

## 2. What you need

- Your two Pico W boards (with LoRa module, ESP-01, SD card in the Maker slot, sensors).
- Node 1 (bridge) powered and on a laptop with someone watching its console (can be shared).
- USB cables and a laptop per board, or one laptop with two USB ports.
- Firmware images from the repo `uf2/` folder (pull latest `main` first):
  - Group A: `uf2/csc09_smp_node_2.uf2`, `uf2/csc09_smp_node_3.uf2`
  - Group B: `uf2/csc09_smp_node_4.uf2`, `uf2/csc09_smp_node_5.uf2`
  - Bridge: `uf2/csc09_smp_node_1.uf2`
  These are the dual-core (uT-Kernel 3 SMP) images. Do **not** mix in `csc09_node_N.uf2`
  (FreeRTOS) for this test.
- SD card on each field node must have a `samples/` folder with JPEGs (copy from
  `sd_card/samples/` in the repo). On a Mac, run `dot_clean /Volumes/<card>` after copying.

## 3. Flash each board

1. Hold the **BOOTSEL** button, plug in USB, release. A drive called **RPI-RP2** appears.
2. Drag the right `.uf2` onto it (node number on the board = number in the file name).
   The board reboots by itself.
3. If the board is already running this firmware, you can instead type `B` in its console to
   reboot it into BOOTSEL.

## 4. Open the serial console

Any terminal at 115200 baud (the speed does not really matter over USB).
- Mac: `ls /dev/tty.usbmodem*` then `screen /dev/tty.usbmodemXXXX 115200` (quit: Ctrl-A then K).
- Windows: PuTTY or the Arduino Serial Monitor on the board's COM port.
- Or: `python3 firmware_smp/buildglue/serial_tstamp.py <port>` (adds timestamps; best for logs).

The board holds its boot log for up to 10 s waiting for the port to open. If you miss it, press
the RUN/reset button or unplug and replug, then reopen the console quickly.

**Console keys:** `c` capture now | `e` sensor readings | `s` statistics | `w` which ESP-01 is
fitted | `d` toggle 30% packet loss | `t` toggle WiFi topology filter | `x` fire 5 captures fast |
`z` stall console 5 s | `k` measure 1 ms tick | `H` hold capture queue | `p` send next test
payload | `B` reboot to BOOTSEL | `h` help.

---

## 5. Test steps (do for EACH of your two nodes unless marked)

### Step 1 - Boot log

Expected (node number changes; order may vary slightly):

```
CSC09 trail mesh, node N, ALFP v1 (uT-Kernel 3 SMP build, dual core)
msgq selftest: wanted 4, sent 4, extra accepted 0, drained 4 - ok
sensors: core 1
comms: core 0
sd: mounted, <X> sample images in <Y> groups: ...
sd: <K> test payload file(s) in test_payloads/
capture id: next is N<N>-00000x (...)
wifi: ESP-01 MAC AC:0B:FB:..:..:.. = WST-<N>
node N ready (field node), outbox has <Q> queued, radio size threshold 1024 B
sensors: PIR GP0, sound GP14, mic GP26, DHT22 GP4, light GP27, button GP20
```

PASS if: the node number matches the board, `dual core`, selftest `ok`, sensors on core 1,
comms on core 0, SD mounted with **more than 0** sample images, and a `node N ready` line.

FAIL signs and what they mean:
- `sd: mount failed ... retrying` repeating -> SD card missing, not FAT32, or bad slot. **STOP**
  this node until fixed (nothing else runs without SD).
- `0 sample images` -> copy `sd_card/samples/` to the card. Captures will not have images.
- `lora: init FAILED (check the Core1121 wiring and power ...)` or `lora_radio: unexpected chip
  type 0x00` -> LoRa module/wiring problem. Node continues on WiFi only. Record it.
- `wifi: WARNING this firmware is node N but the ESP-01 is WST-M` -> wrong ESP-01 in the socket.
  See step 2.
- `wifi: WARNING this ESP-01 is not in wifi_mesh_config.h` -> unknown ESP-01; WiFi routing will
  be dropped by other nodes. Record the MAC.
- No `wifi: ESP-01 MAC` line at all -> ESP-01 did not answer (power/wiring/UART).

### Step 2 - Which ESP-01 is fitted (press `w`)

Expected: `wst: ESP-01 MAC ... = WST-N | firmware node N | MATCH`

- `MISMATCH` -> the ESP-01 belongs to another node. Either swap ESP-01 modules between boards so
  each board has its own WST, or flash the image whose number matches the WST. **Known issue:
  node 2 was last seen carrying WST-3's ESP-01**, so Group A should expect to swap modules
  between node 2 and node 3. Fix before step 5; a mismatch breaks the WiFi topology filter.
- `no ESP-01 answered at boot` -> ESP-01 power/wiring problem.

### Step 3 - Sensors (press `e`, also printed every 10 s)

Expected: `[ENV] Temp: 2x.x C | Hum: xx.x% | Light: <n> | Weather: <label> | PIR 0/1 Sound 0/1`

PASS if temperature and humidity are realistic. Wave a hand at the PIR sensor and clap near
the mic, press `e` again: PIR and Sound should show 1 briefly.
FAIL: `DHT22: no reading (<n> fails)` -> DHT22 wiring/power on GP4.

### Step 4 - Radio status (press `s`)

Look at these lines in the output:

```
node N | alfp in .. badcrc .. | tx ok .. fail .. | rx ok .. crcfail .. expired .. | outbox Q
  wifi: tx .. rx .. delivered .. relayed .. dup .. rxerr ..
  lora: tx .. rx .. delivered .. relayed .. dup .. rxerr ..
  dht22 good .. fail .. | lora up | wifi up
  bridge last heard on wifi <ms> ms ago
  bridge last heard on lora <ms> ms ago
  task comms: core 0 ...
  task usb_owner: core 0 ...
  task sensors: core 1 ...
  longest comms loop .. ms | capture q full .. | log dropped .. | usb dropped .. | heap used ..
  sd handles in use x/8 | no SD errors
```

PASS if:
- `lora up` and `wifi up`.
- With node 1 ON: `bridge last heard on lora` **and** on `wifi` are both under ~10 000 ms.
  - **Node 5:** WiFi bridge heartbeat only arrives if node 3 or node 4 is ON and relaying. If
    node 5 says `bridge never heard on wifi` while node 4 is on, that is a FAIL for relaying.
- Task cores: comms 0, usb_owner 0, sensors 1.
- `no SD errors`. If it says `<n> SD errors, last: <op> FR=<code> on <path>`, record the whole
  line (FR codes: 1 disk error, 3 not ready, 7 denied, 9 invalid object, 18 too many open files).
- `rxerr` should stay at or near 0.

If node 1 is OFF you will see `bridge never heard on wifi` / `on lora`. That is expected (see
section 1). Turn node 1 on before step 5.

### Step 5 - Capture and send (the main test)

On one field node press `c`. Expected on the **field node**:

```
[ALERT] Manual capture (PIR x, sound x). Recording 5s of audio...
[AUDIO] legacy mic.c: <label> energy .. zcr .. mean .., <n> samples
[ENV] Temp: ...
capture N<N>-00000x queued (...), outbox <n>
tx tel/... size=<small> thr=1024 radio=lora reason=size
tx tel/...: done via lora, <f> frags, <r> rounds, <d> resent, ~1.0 s
tx <image path> size=<~9000-10000> thr=1024 radio=wifi reason=size
tx <image path>: done via wifi, <f> frags, <r> rounds, <d> resent, ~2-3 s
```

Expected on the **bridge (node 1) console**, two lines per capture:

```
rx from node N: rcv/<xx>/nN_<sid>.bin, <bytes> bytes via lora, 0 dup
rx from node N: rcv/<xx>/nN_<sid>.bin, <bytes> bytes via wifi, 0 dup
```

PASS if: both transfers end in `done`, telemetry went `lora` and image went `wifi`, both with
`reason=size`, and the bridge shows both `rx from node N` lines. Reference timings from earlier
hardware runs on node 2: LoRa telemetry ~1.0 s (0.8-2.2 s), WiFi image ~2.4 s (2.0-2.9 s).

Repeat `c` 5 times per node, waiting for both `done` lines each time. Record how many of 5
were fully delivered and the times.

FAIL / what it means:
- `reason=fallback` -> the preferred radio could not reach the bridge; the other radio was used.
  Note which one and check step 4.
- `gave_up` -> too many lost fragments; check antennas/distance, `rxerr` in `s`.
- `tx ...: bridge refused it with status N = <name>`:
  - `BUSY` -> bridge was receiving too many files at once; retry.
  - `STORAGE (bridge SD)` -> **bridge's** SD card problem. Ask the bridge person to press `s` on
    node 1 and send the `sd handles in use ... | ... SD errors, last: ...` line and any
    `alfp: refused node N sid ..., status ..: <reason>` lines from the bridge console.
  - `TOO_BIG` -> file too large for ALFP.
- `capture ...: SD write failed` -> field node SD problem.
- `outbox` count keeps growing and no `tx` lines -> bridge not heard; go back to step 4.

### Step 6 - Multi-hop check (Group B mainly; Group A optional)

The WiFi topology filter (ON by default) means **node 5 cannot talk to node 1 directly**.
1. Both node 4 and node 5 on, node 1 on. On node 5 press `c`.
2. Expected: node 5's image goes `via wifi` and is `done`. Then press `s` on **node 4**: the
   `wifi: ... relayed <n>` counter should have gone up. That proves node 4 forwarded node 5's
   packets to the bridge.
3. Turn node 4 off (unplug). Press `c` on node 5 again. Expected (if node 3 is off too): WiFi can
   no longer reach the bridge, so the image goes `radio=lora reason=fallback` and is still
   `done` (just slower). Plug node 4 back in afterwards.

Group A version: on node 2 and node 3 press `s` before and after a few captures and see whether
either `relayed` counter goes up (with the filter ON, 2 and 3 both reach node 1 directly, so
relaying is not required, but it can still happen).

Optional: press `t` on all powered nodes (filter OFF = everyone hears everyone) and repeat. Press
`t` again to turn it back ON.

### Step 7 - Packet loss recovery

On one field node press `d` (`injected loss 30%`). Press `c`.
Expected: transfers still end `done`, but with more `rounds` and `resent` > 0. That is ALFP
resending only the missing fragments. Press `d` again to turn loss off (`injected loss off`).

### Step 8 - Store-and-forward (bridge offline)

1. Ask the bridge person to unplug node 1.
2. Wait 15 s, press `c` twice on one field node. Expected: `capture ... queued ..., outbox <n>`
   lines, the outbox count growing by 2 per capture (telemetry + image), and **no** `tx` lines. `s` shows the bridge not heard
   recently.
3. Plug node 1 back in. Within ~10-20 s the field node should send everything: `tx ...: done`
   lines appear and `outbox` goes back to 0 in `s`.

PASS if every queued file is delivered after the bridge returns.

### Step 9 (optional) - Stress

- `x`: fires 5 captures fast. Expected one `[ALERT] capture queue full, dropped` and `s` showing
  `capture q full 1`. Nothing freezes.
- `z`: console stops for 5 s. Afterwards `s` shows `log dropped` increased. Board keeps working.

---

## 6. Report template (AI: fill this in and give it back to the student)

```
Group: A (nodes 2+3) / B (nodes 4+5)        Date:
Bridge (node 1) on during test: yes/no      Firmware: csc09_smp_node_N.uf2 (SMP)

Per node:
| Node | Boot ok | SD images | LoRa up | WiFi up | ESP-01 (w) | DHT22 | Bridge heard lora/wifi (ms) |
|------|---------|-----------|---------|---------|------------|-------|-----------------------------|
|      |         |           |         |         | WST-? MATCH/MISMATCH |  |                         |

Captures (step 5):
| Node | Captures | Telemetry done via LoRa | Image done via WiFi | Avg LoRa s | Avg WiFi s | Fallbacks | Failures |
|------|----------|-------------------------|---------------------|------------|------------|-----------|----------|

Multi-hop (step 6): relayed counter on node __ went from __ to __ . Fallback to LoRa when relay off: yes/no
Packet loss (step 7): done with 30% loss: yes/no, rounds __, resent __
Store-and-forward (step 8): queued __ files, all delivered after bridge back: yes/no
Errors seen (copy exact lines):
Hardware problems found (module swaps, wiring, SD):
Overall: PASS / PARTIAL / FAIL - one sentence why
```

Send the filled report and the raw console logs (from `serial_tstamp.py` if possible) back to
the group.
