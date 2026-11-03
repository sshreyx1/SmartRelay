# Hardware tests T3 and T6 (A10)

The test numbers come from the design review (README.md "Running the T1-T8 tests"). Each run is logged with
`tools/serial_log.py` to `docs/sound/evidence/<test>_<date>.log`, and the result goes in the tables below.
Firmware: `build-fw/csc09_node_2.uf2` and `csc09_node_1.uf2` from this branch (FreeRTOS, on-node classifier).

**Setup for both** (ml venv active, repo root):
- Node 1 (bridge) powered, with its SD card. The backend may run (it owns node 1's port) or not.
- Node 2 with its SD card holding `sd_card/samples/`. The MacBook speakers about 8 cm from the mic.
- No other serial monitor open on node 2's port.
- Find the port: `python -m serial.tools.list_ports -v`; below, `<NODE2>` = `/dev/cu.usbmodemXXXX` (macOS) or
  `COMn` (Windows).
- Demo clips come from `ml/demo_clips/playlist.csv` (`python ml/demo_playlist.py --live <bench csv>`).

---

## T3: audio under load

**Question:** does the sound recording lose samples, or the classifier change its answer, while the node's radios
and SD card are busy?

**Deviation from the plan (2 physical nodes tonight):** the plan has node 3's transfers relayed through node 2.
With 2 boards, the load is node 2's **own** back-to-back transfers, made slow by 30% injected loss on node 1, so
WiFi, LoRa, SD and ALFP retries all run during the recording. The µT-Kernel half (does `sources.mk` pick up the new
sensing files?) was checked without hardware: `TRIBUNAL_IMPACT.md` section 7 (all 5 sound files compile, 0 warnings).

**Steps** (about 15 min):
1. Start the log on node 2:
   `python tools/serial_log.py --port <NODE2> --test T3 --count "lost=0" "[SOUND]" "done via" "warning"`
   Type a key and press Enter to send it.
2. **Idle** (nothing queued, `s` shows `outbox 0`): for each of 3 playlist clips, one per group (e.g. BIRD, MAMMAL,
   WEATHER), start the clip in a second terminal
   (`python ml/playback.py <file> --device "MacBook Pro Speakers"`) and send `c` within half a second. Wait for
   the `[SOUND]` line and for `done via ...`.
3. **Load:** on node 1's monitor (or the backend's console), press `d` (30% loss). On node 2, send `c` 3 times about
   6 s apart to queue transfers (check `outbox` ≥ 2 with `s`). While they are still sending, play the same 3 clips
   again with `c`, three times each (9 captures under load). Between clips, `s` must still show `outbox` > 0.
4. Press `d` on node 1 again (loss off), stop the log with Ctrl+C.

**Pass:**
- `lost=0` on every `[SOUND]` line (idle and load);
- `maxcall_us` far below 100000 (the ring has about 512 ms of slack), and no `warning` line;
- under load, the same **group** as the idle run of the same clip in at least 2 of 3 plays. Exact labels vary between
  plays even when idle: H6 showed re-recordings differ.

**Results:**

| # | clip (group) | idle: label, conf | load: label, conf (×3) | outbox during | lost | maxcall_us | feat_us | pass |
|---|---|---|---|---|---|---|---|---|
| 1 | | | | | | | | |
| 2 | | | | | | | | |
| 3 | | | | | | | | |

Evidence: `docs/sound/evidence/T3_<date>.log`. Verdict: _not run yet_.

---

## T6: capture queue full

**Question:** when comms cannot keep up, does a 5th capture get dropped cleanly, with a logged line, while the sensors
task keeps running?

**How it is forced:** console key `H` (this branch, `firmware/app/main.c`) toggles a hold. Comms stops taking captures
off the 4-slot `g_capture_q`, while the radios, ALFP and the outbox keep running. The curveball `x` key does the same
job but exists only in the SMP firmware, which does not build (TRIBUNAL_IMPACT section 5).

**Steps** (scripted, about 2.5 min, no typing needed):
```
python tools/serial_log.py --port <NODE2> --test T6 --duration 150 \
  --send H@3 c@5 c@20 c@35 c@50 c@65 c@80 H@100 s@130 \
  --count "capture queue full, dropped" "queued (" "[ENV]" "HELD" "released"
```
Windows PowerShell: the same line, with a backtick instead of `\` at the end of the first line.

**Expect:**
- `capture queue HELD for test T6 ... (0 waiting)` at about 3 s;
- 6 captures (`[ALERT] Manual capture`, each followed by `[SOUND] ... lost=0`); captures 5 and 6 print
  `[ALERT] capture queue full, dropped`;
- `[ENV]` lines keep coming every ~10 s throughout (the sensors task never blocks on the full queue);
- at about 100 s: `capture queue released, sending what waited (4 waiting)`, then 4 × `capture N2-…: heard …` and
  `capture N2-… queued`, and their transfers `done via …`;
- the counts line: `'capture queue full, dropped' x2`, `'queued (' x4`, `'[ENV]' x` about 14.

**Pass:** exactly 4 queued and 2 dropped, `[ENV]` never pauses for more than ~15 s (one 5 s recording plus the 10 s
print period), and all 4 held captures arrive after the release (dashboard, or the bridge's `saved ...` lines).

**Results:**

| measure | expected | measured |
|---|---|---|
| waiting when released | 4 | |
| `capture queue full, dropped` | 2 | |
| `queued (` after release | 4 | |
| longest gap between `[ENV]` lines | ≤ 15 s | |
| held captures delivered | 4 of 4 | |

Evidence: `docs/sound/evidence/T6_<date>.log`. Verdict: _not run yet_.
