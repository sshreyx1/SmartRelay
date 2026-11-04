# Two-node hardware test: backend receiving from node 2 through node 1 (CURVEBALL round 3, Task E)

Proves the professor's requirement "the backend receives correctly from node 2 through node 1"
end to end, on real hardware, covering the size-based radio choice (Task A) and both radios'
failure modes. **Nothing in this file has been run**: there is no board or radio hardware attached
to the machine that wrote it. Every case below is a procedure for Shreya (node 1 + backend) and a
friend (node 2) to run, with the exact command, expected result, and where the evidence lands.
Paste real output back into the tables; do not mark a case passed without actually running it.

This reuses `docs/sound/DEMO.md`'s existing setup (port discovery, backend venv, clean-slate
commands) rather than repeating it — read that file's "Before the demo" section first for the
mechanics. The differences here: node 2 runs on a **separate laptop** (a friend's, not Shreya's),
and every case below needs its own evidence log via `tools/serial_log.py --test twonode_<case>`.

## Setup

- **Node 1 (bridge) + backend**, Shreya's laptop: flash `csc09_node_1.uf2`, then from `backend/`
  (venv active, matching `docs/sound/DEMO.md`'s W2 exactly):
  `SMARTRELAY_BRIDGE_PORT=<node 1 port> uvicorn app:app --port 8000` (`$env:SMARTRELAY_BRIDGE_PORT="COMx"`
  on Windows). Confirm `bridge: connecting to ... automatically` in the backend's own log.
- **Node 2 (field node)**, friend's laptop: flash `csc09_node_2.uf2`, with `sd_card/test_payloads/`
  (Task A's size ladder) copied onto its SD card alongside the usual `sd_card/samples/`. Run
  `python tools/serial_log.py --port <node 2 port> --test twonode` to log everything node 2 prints,
  timestamped, to `docs/sound/evidence/twonode_<date>.log` (per-case logs use `--test twonode_caseN`
  instead so each case's evidence is a separate file).
- **No cross-laptop time sync needed.** Every case below correlates by `capture_id`
  (`N2-00000x`, printed on both the node's own serial line and the dashboard/DB) or by ALFP
  sequence number (`sid=` in the bridge's `@ALFP FILE` lines), never by wall-clock timestamp.
- **Clean slate** before case 1: stop the backend, then (from `docs/sound/DEMO.md`)
  `Remove-Item backend\smartrelay.db, backend\received\n*, backend\received\demo_* -ErrorAction SilentlyContinue`
  (Windows) or the macOS equivalent. Restart the backend after.

Keys referenced below, confirmed against the actual firmware on this branch
(`firmware/app/main.c`'s `print_help()`): `c` capture now, `s` stats, `d` toggle 30% injected loss,
`t` toggle WiFi topology filter, `H` hold the capture queue (test T6), `p` send the next
size-ladder test payload. **Note:** the brief's suggested `x`/`z` keys for burst/stall tests exist
only in `firmware_smp/app/main_smp.c` (the uT-Kernel SMP build), which has never been compiled
(`docs/curveball_results.md`) — on this FreeRTOS firmware, case 6 below uses `H` + repeated `c`
instead, exactly as `docs/sound/TESTS.md`'s own T6 procedure already does. This is a real,
pre-existing discrepancy in the brief's key list, not something newly introduced here.

---

## Case 1: baseline (telemetry + image both arrive)

**Steps:** on node 2, play a demo clip (any `ml/demo_clips/playlist.csv` entry, or just press `c`
for a manual capture) and wait ~6-10 s.

**Expect:** node 2's log shows `capture N2-00000x telemetry queued (tel/...)` then
`capture N2-00000x queued (samples/...)`; the bridge's tx lines (printed on whichever node is
driving the transfer) show the telemetry job going `via lora` and the image job `via wifi`
(`tx ... size=27 thr=1024 radio=lora reason=size` and `tx ... size=<n> thr=1024 radio=wifi
reason=size`, Task A's log line). Dashboard: one card, `N2-00000x`, with both the sound/sensor
chips and the image. `tools/verify_received.py` (below) confirms the received image's SHA-256
against the node's `sd_card/samples/` copy — note the baseline image is whichever sample
`sip_pick()` chose, not a fixed file, so compute its SHA-256 from the actual file node 2 logged
rather than assuming which one.

**Evidence:** `docs/sound/evidence/twonode_case1_<date>.log`, `backend/received/`,
`tools/verify_received.py` output.

## Case 2: size ladder

**Steps:** on node 2, press `p` once per file in `sd_card/test_payloads/MANIFEST.csv` (6 presses,
a few seconds apart so transfers don't queue up and mask which radio each one picked
individually). Each send has no capture metadata (Task A.3's `send_next_test_payload()`), so it
will not appear on the dashboard as a capture card — this case is checked entirely from the tx
log and `tools/verify_received.py`, not the dashboard.

**Expect:** every file routes by `docs/DESIGN.md`'s threshold (1,024 B): since every rung in the
default ladder (1024, 4096, 16384, 32768, 65536, 120832 B) is `>= 1024`, **all six go over WiFi**
by the size rule (`reason=size`) unless WiFi is down, in which case they fall back to LoRa
(`reason=fallback`) and will be slow (see `docs/results/e6_size.csv`: the 120,832 B file could
take several minutes over LoRa — be patient or re-run case 2 with WiFi up first, then case 3
covers the WiFi-down version of this specifically).

| file | size | radio (tx log) | reason | time | result |
|---|---|---|---|---|---|
| p_0001024.jpg | 1024 | | | | |
| p_0004096.jpg | 4096 | | | | |
| p_0016384.jpg | 16384 | | | | |
| p_0032768.jpg | 32768 | | | | |
| p_0065536.jpg | 65536 | | | | |
| p_0120832.jpg | 120832 | | | | |

**Evidence:** `docs/sound/evidence/twonode_case2_<date>.log`, this table filled in,
`tools/verify_received.py sd_card/test_payloads/MANIFEST.csv backend/received`.

## Case 3: WiFi down -> large file fails over to LoRa (or waits)

**Steps:** power off node 1's ESP-01 (never pull it with power on — `docs/sound/DEMO.md`'s
warning applies here too). On node 2, press `p` to send the largest ladder file
(`p_0120832.jpg`), or trigger a real capture with `c` (the image half is the large one).

**Expect:** `s` on node 2 shows `wifi DOWN` (or `bridge never heard on wifi`); the tx log shows
`radio=lora reason=fallback` for the large transfer, and it completes (slowly — LoRa at this size
is tens of minutes per `e6_size.csv`'s trend; consider using a mid-ladder file like
`p_0032768.jpg` instead if time is short, and note which file was actually used). No data loss:
`alfp_outbox_count` stays nonzero while queued, drops to 0 once sent. Power the ESP-01 back on
afterward and confirm any still-queued jobs drain on their own (`s` shows `outbox 0` within the
outbox's backoff window).

**Evidence:** `docs/sound/evidence/twonode_case3_<date>.log`.

## Case 4: LoRa down -> small data (telemetry) falls back to WiFi

**Steps:** with WiFi up and LoRa physically disconnected (or just check `s` reports LoRa not
heard), trigger a capture (`c`). The telemetry job (27 B, normally routed to LoRa by the size
rule) must fail over to WiFi instead.

**Expect:** tx log shows the telemetry job as `radio=wifi reason=fallback` (not `reason=size`,
since WiFi was not telemetry's size-preferred radio); it still arrives (telemetry is tiny, so even
as a fallback over WiFi it's fast). The image job, already WiFi-preferred, is unaffected.

**Evidence:** `docs/sound/evidence/twonode_case4_<date>.log`.

## Case 5: loss injection

**Steps:** on node 2 (or node 1 — `d` toggles the local node's own injected loss on both its
radios), press `d` (30% loss on), trigger 2-3 captures with `c`, then `d` again (loss off).

**Expect:** transfers still complete (selective-repeat retries, per `docs/DESIGN.md`); tx log
shows `rounds > 1` and nonzero `resent`; CRC-32 still checks out on arrival (no corrupted file ever
reaches `backend/received/` — ALFP's CRC-32 would catch it and the sender would resend, not the
backend silently accepting bad data).

**Evidence:** `docs/sound/evidence/twonode_case5_<date>.log`.

## Case 6: burst (capture queue full)

**Steps (adapted from `docs/sound/TESTS.md`'s T6, since the brief's `x` key does not exist on this
firmware — see the setup section's note above):** on node 2, press `H` (hold), then `c` six times
a few seconds apart, then `H` again (release).

**Expect:** captures 5 and 6 print `[ALERT] capture queue full, dropped` (the queue holds 4); the
4 accepted ones all arrive after release, each as telemetry + image pair, correctly linked by
`capture_id`. `[ENV]` lines (sensors task) never stop printing during the hold — the queue being
full never blocks the sensors task, only comms' capture intake.

**Evidence:** `docs/sound/evidence/twonode_case6_<date>.log`.

## Case 7: backend restart mid-run

**What the code actually does** (read, not assumed): `backend/live_bridge.py`'s `run_serial()`
auto-reconnects on a dropped *serial connection* with backoff — but a backend **process restart**
is a different event: `LiveBridge._cur` (the in-progress `@ALFP FILE`/`@D`/`@ALFP END` parse
state) is purely in-memory and is lost on restart, same as the Python process's `Store` connection
(harmless — SQLite reopens the same file, no data loss there). The real gap: the bridge node
streams its `@ALFP FILE ... @D ... @ALFP END` uplink text over USB serial **once**, when
`on_rx_done()` fires (`common/uplink.c`'s `uplink_send_file()`); if the backend process is not
running (not just disconnected, actually not started) at that exact moment, those bytes go out
on the wire with nobody reading them and are gone -- even though the underlying file is still
safely reassembled on the bridge's own SD card (`inbox/n3_00042.bin`/`.meta`), the backend has no
mechanism to ask the bridge to replay an already-streamed transfer. **This is a real, documented
gap, not a guess** — report it honestly rather than assuming the brief's "nothing lost" is
guaranteed.

**Steps:** start a capture on node 2, and while its transfer is still in flight (telemetry and/or
image), kill the backend process (Ctrl+C) and restart it within a few seconds.

**Expect (per the gap above):** if the backend restarts *before* the bridge's `on_rx_done()` fires
for that transfer, nothing is lost (the backend just wasn't listening for the ALFP-level radio
protocol itself, which the bridge's `comms_task` keeps running independent of the backend; only
the already-finished-and-streamed-once uplink text can be lost). If the restart happens to land
exactly between the bridge finishing the transfer and streaming it, that specific transfer's
telemetry or image is lost from the backend's point of view, permanently, even though it exists on
the bridge's SD card. Record which actually happened; do not mark this case "no loss" without
confirming node 2's own capture_id sequence has no gap in `backend/received`/the dashboard.

**Evidence:** `docs/sound/evidence/twonode_case7_<date>.log`, plus the backend's own stdout
around the restart.

## Case 8: dual-core (SMP firmware) repeat of cases 1-3

**Blocked.** Depends on `docs/curveball_results.md`'s C4 (incremental integration) and C5
(Tribunal tests) completing on real hardware with the uT-Kernel SMP build — neither has been
reached; see that file for the exact current status of every stage (C0 real PASS/FAIL, C1-C5
BLOCKED). Nothing to run here until C4/C5 unblock. When they do: repeat cases 1, 2 and 3 above
with `firmware_smp`'s build in place of `firmware/app/main.c`'s, and compare latency and the ring/
queue counters `docs/curveball_results.md`'s C2 (`smp_smoke`) and C5 (T1/T6/T8) already define.

---

## Verifier

`tools/verify_received.py` (see that file) checks every case above's evidence automatically:
manifest vs. `backend/received/` (present, size match, SHA-256 match, radio from the DB's `radio`
column) and the database's telemetry/image linkage (one row per `capture_id`, `image_path` not
null once the image half has actually arrived). Its own logic was exercised against a synthetic
fixture in this session (no real two-node data exists yet) — see that file's header comment for
exactly what was and was not proven.
