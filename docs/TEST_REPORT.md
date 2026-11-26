# CURVEBALL round 3, Task D: test report

Single entry point: `tools/run_all_tests.sh` (style check -> host unit/filtering tests -> host
integration -> backend pytest -> `ml/tests`). Every number below was produced by actually running
the command shown, in this session, on this machine (via WSL for the C/host side -- see
"Environment notes"); nothing is estimated or assumed unless marked BLOCKED/estimated explicitly.

## Requirement -> test -> result -> evidence

| Requirement (brief) | Test | Result | Evidence |
| --- | --- | --- | --- |
| Size-based radio choice: unit, every boundary | `host/tests/test_transfer.c::test_radio_selection_for_size` | PASS | this session's WSL build log (below); 0/1/thr-1/thr/thr+1/max, fresh/stale combinations, `now_ms` wraparound |
| Size-based radio choice: end to end via outbox | `host/tests/test_transfer.c::test_outbox_size_based_routing` | PASS | small job over LoRa, large over WiFi, real `alfp_outbox` |
| Old `alfp_select_radio()` unaffected | `host/tests/test_transfer.c::test_radio_selection` (pre-existing, unmodified) | PASS | unchanged since before this round |
| WiFi fails mid-transfer -> LoRa, no data loss | `host/tests/test_transfer.c::test_never_switch_mid_transfer` (pre-existing, unmodified) | PASS | SHA/CRC-equal file over the simulator's dual-radio outbox; **this is also Task D.3's "fail the WiFi fake mid-transfer" requirement** -- see "Known gaps" below for why no separate real-mesh integration harness was built for this specific case |
| Telemetry/image pairing by `capture_id` | `backend/tests/test_telemetry_split.py` (5 tests) | PASS | telemetry-then-image, image-then-telemetry, telemetry-only, duplicate-telemetry |
| Filtering: duplicate suppression | `host/tests/test_filtering.c::test_duplicate_suppression` | PASS | `dup_dropped` counter rises on a redundant-path topology |
| Filtering: not addressed to this node | `host/tests/test_filtering.c::test_not_addressed_to_node_is_not_delivered` | PASS | `delivered == 0`, `relayed > 0` at a non-destination node |
| Filtering: injected loss, transfer still completes | `host/tests/test_filtering.c::test_loss_injection_30_percent_still_completes` | PASS | 30% link loss, `data_resent > 0`, result still `ALFP_TX_DONE` |
| Filtering: bad-CRC packets rejected | `host/tests/test_transfer.c::test_corruption_caught_by_crc16` (pre-existing, unmodified) | PASS | `pkts_bad_crc > 0`, file still exact |
| Filtering: WiFi mesh topology filter (forces multi-hop) | `host/integration/run_wifi_integration.sh` (real `wifi_mesh.c`, filter ON by its own default, per `integ_node.c`'s comment) | PASS (real mesh code, not the simulator) | E(5) -> C(3, relay) -> A(1): see "Integration" below |
| Filtering: `hop_dropped` | not covered | **gap, with reason** | see "Known gaps" |
| Integration: size ladder through real mesh code | `host/integration/run_size_ladder.sh` | PASS (2 of 6 rungs run; see below) | 1024 B and 65536 B, both LoRa and WiFi, 4/4 |
| Backend: merge-upsert never erases an image | `backend/tests/test_telemetry_split.py::test_image_then_telemetry_does_not_erase_the_image` | PASS | |
| Backend: existing sound-label behaviour unchanged | `backend/tests/test_sound_labels.py` (pre-existing, unmodified) | PASS | 5/5 |
| Backend: offline receiver still saves the no-name-tail legacy case | `tools/test_bridge_receiver.py` against a real `test_uplink.c` stream | PASS | run manually this session, see below |
| Verifier logic (manifest vs received vs DB) | `backend/tests/test_verify_received.py` (3 tests, Task E) | PASS | synthetic fixture only -- no real two-node run in this session |
| BARR-C style/lint over in-scope, host-compilable files | `tools/check_style.sh` | PASS (2026-11-19: clang-format 23 run for real; 35 files reformatted, now clean) | `docs/CODING_STANDARD.md`, `docs/verify/RESULTS.md` |
| Dual-core SMP build | `firmware_smp/buildglue/build_smp.sh` (all 5 nodes) + hardware on nodes 1 and 2 | PASS (2026-11-19, was BLOCKED) | `docs/verify/RESULTS.md`: builds on macOS, T2 core assignment, 60 min soak, tick measurement |
| Two-node hardware test (8 cases) | `docs/sound/TWO_NODE_TEST.md`, `tools/soak_pair.py` | PASS for the cases runnable with 2 boards (was BLOCKED); T1 (multi-hop) needs nodes 3-5, T7 (ESP-01 removed) needs hands | `docs/verify/RESULTS.md` |
| `ml/tests` unaffected | `ml/tests` (read-only) | PASS | 10/10, unmodified |

## Counts

- **Host unit tests** (`test_units`, `test_transfer`, `test_filtering`): 31 tests, 0 failed, 9,277 checks.
- **Host integration** (real `lora_mesh.c`/`wifi_mesh.c`): 6 real runs across this session (sizes
  2000/5000/20000 B LoRa, 8000/20000/65536 B WiFi, one pair at 1024 B each), 0 failed.
- **Backend** (pytest): 13 tests, 0 failed.
- **`ml/tests`** (pytest, read-only): 10 tests, 0 failed.
- **BARR-C style check**: 1 stage, PASSED (3 of 4 checks SKIPPED for a missing tool, 1 actually run).
- **Dual-core SMP** (updated 2026-11-19): builds for all 5 nodes and runs on nodes 1 and 2; see `docs/verify/RESULTS.md`.
- **Two-node hardware** (updated 2026-11-19): run on nodes 1 and 2, including a 60 min soak, backend end-to-end and a
  reboot-persistence test; see `docs/verify/RESULTS.md`. (The counts above this line are from round 3 and are kept as
  they were.)

## Reproduction commands

```sh
tools/run_all_tests.sh                       # everything below, in order, one summary table
tools/check_style.sh                         # style/lint only
host/integration/run_size_ladder.sh --quick  # two representative sizes, both radios, real mesh code
host/integration/run_size_ladder.sh          # the full sd_card/test_payloads/MANIFEST.csv ladder (slow, see its header comment)
cd backend && python -m pytest -q            # backend only
python -m pytest ml/tests -q                 # ml only
sh firmware_smp/buildglue/check_env.sh       # Task C0
```

Host C tests (`test_units`/`test_transfer`/`test_filtering`) and the mesh integration scripts need
a real POSIX `mkdir(path, mode)` (two-argument form) -- confirmed in this session that **native
Windows MinGW/MSYS2 does not provide this** (`host/sim/storage_posix.c` fails to compile there with
"too many arguments to function 'mkdir'"), and the integration scripts additionally need
`<arpa/inet.h>`, also missing on native Windows. Both ran successfully in this session via WSL
(`wsl.exe -d Ubuntu`, plain `gcc`, no `cmake`/`make` needed) and would run directly on Linux/Mac
with no workaround. `tools/run_all_tests.sh` detects and reports this honestly (FAIL with the real
compiler error) rather than skipping silently, since a C compiler genuinely is present, it is just
missing a function this code needs -- see the script's own comment on `CC`.

## Environment notes (what actually ran where, this session)

- No ARM toolchain, `cmake`, or `make` anywhere on this machine (confirmed by
  `firmware_smp/buildglue/check_env.sh`, see `docs/curveball_results.md`) -- this is why every
  firmware-side claim in this round is "written, not compiled."
- Host-side C (ALFP library, simulator, host tests, mesh integration): compiled and run via WSL2
  (`wsl.exe -d Ubuntu`, which has `gcc` and real POSIX headers), since native Windows MinGW lacks
  the two-argument `mkdir()` and `<arpa/inet.h>` this code needs. `cmake` is not installed in
  either place, so everything here used a manual `gcc` invocation mirroring
  `host/CMakeLists.txt`'s exact source list -- `tools/run_all_tests.sh` does the same thing
  automatically when `cmake` is missing.
- Python (backend/ml tests): ran directly on Windows, where `pytest`, `fastapi`, `uvicorn`,
  `pydantic`, `pyserial`, and `Pillow` are all already installed.
- `cppcheck` and `clang-tidy` are not installed anywhere reachable from this session (checked both
  Windows PATH and WSL); `docs/CODING_STANDARD.md` gives the install commands.

## Known gaps

1. **`hop_dropped` is not tested.** `host/sim/sim_mesh.c`'s `HOP_LIMIT` is 4 and `SIM_MAX_NODES` is
   6, so the longest chain buildable in the simulator is 5 hops -- which uses exactly 4 relay
   decisions, and the final hop always lands on the real destination, which delivers regardless of
   its own hops field (confirmed by reading `sim_mesh.c:on_frame`). There is no way to make a
   non-destination node see `hops == 0` within this node cap. See
   `host/tests/test_filtering.c`'s header comment for the full reasoning.
2. **No real-mesh (non-simulator) integration test for "WiFi dies mid-transfer, falls back to
   LoRa."** `integ_node.c` (the real-mesh integration harness) links exactly one radio per process
   (`-DUSE_WIFI` or not), matching one Pico with one radio compiled in; building a dual-radio
   variant that mirrors `firmware/app/main.c`'s outbox-driven radio choice was judged out of scope
   for this round's remaining time. The equivalent behaviour **is** proven, just through the ALFP
   simulator instead of the real mesh code: `test_never_switch_mid_transfer` kills the simulated
   WiFi link mid-transfer and confirms the outbox's next attempt completes on LoRa with the file
   byte-identical.
3. **The full size ladder (all 6 rungs, both radios) was not run against the real mesh code.**
   Only 1024 B and 65536 B were (via `run_size_ladder.sh --quick`); the largest rung (120,832 B)
   is estimated at several minutes per radio based on this session's own timing data (2,000 B
   LoRa ~5 s, 20,000 B LoRa ~46 s) and was not run to avoid an open-ended session delay. The script
   and the estimate are both in `host/integration/run_size_ladder.sh`'s header comment; running the
   full ladder is a `host/integration/run_size_ladder.sh` away.
4. ~~Dual-core SMP (Task C) and the two-node hardware test (Task E) could not run at all.~~ Resolved
   2026-11-19: both ran on real boards, see `docs/verify/RESULTS.md`. Still not run: T1 multi-hop
   (needs nodes 3-5) and T7 with the ESP-01 physically removed.
5. **`backend/app.py`'s `on_transfer()` telemetry/image split logic itself has no direct test** --
   its merge/discriminator behaviour is covered indirectly through `backend/storage.py`'s
   `record_capture()` (which `test_telemetry_split.py` exercises directly, mirroring what
   `on_transfer()` calls) and a manual check during development (not persisted as a test), rather
   than a `LiveBridge`/FastAPI-level test. `store` is a module-level object constructed on import
   with real side effects (opens/creates `backend/smartrelay.db`), which made an isolated
   `on_transfer()`-level test more invasive to add safely than the time budget allowed.

---

# Transfer visibility (branch `feature/transfer-visibility`, TRANSFER_VISIBILITY.md)

Goal: while node 5 sends a capture to node 1 through node 3 or 4, the bridge's laptop dashboard
shows it arriving (session start, fragments, the relay that delivered each one, hops, NACK rounds,
duplicates, radio time, time queued on node 5, upload time, three CRC checks). Everything the
dashboard shows travels through node 1's USB serial as `@EVT` lines.

## Automated results (2026-11-26, before any hardware run)

| Check | Result |
|---|---|
| `tools/run_all_tests.sh` (WSL) | no stage failed: style PASS (clang-format/cppcheck/clang-tidy not installed, so SKIPPED inside it), host tests PASS, host integration PASS (real `lora_mesh.c` and `wifi_mesh.c` E->C->A, 20% loss, byte-identical); backend and ml SKIPPED there (no pytest in WSL) |
| host `ctest` (14 tests incl. both integrations; `transfer` adds `test_progress_callbacks`, `test_outbox_prepare_meta`; `units` adds meta v1/v2) | 14/14 passed |
| `backend/tests` (pytest on Windows Python 3.14, incl. new `test_events.py`, 15 tests) | 28 passed |
| `ml/tests` | not run: `ml/requirements.txt` pins need Python 3.11/3.12 (only 3.14 here); nothing under `ml/` changed |
| backend smoke on a copy: demo ingest -> WS `transfer_start/progress/done` + `capture`, `/api/transfers*`, capture `transfers[]` | pass |
| `simulator/demo_feed.py --scenario normal / lora / loss / bridge-down` against a running backend | pass (40 frags via 3+4; LoRa v=0 shown unknown; 3 lost -> 1 NACK, rounds 2, 3 dups; queued 112-114 s) |
| firmware build `csc09_node_1..5.uf2` (+ selftest, phototest) | builds, no new warnings |
| `firmware_smp` build (nodes 1, 5) from an exported copy | builds, behaviour unchanged |

Not measured yet (needs the boards): `longest comms loop` on the bridge before/after the `RXF` lines
(section 3.3 says to add `APP_RXF_EVERY=N` if it grows noticeably).

## Hardware acceptance (section 7) -- TO DO, fill in after the run

Flash all five nodes from this branch (`build-fw/csc09_node_N.uf2`), WiFi topology filter ON (`t`).
Bridge laptop: backend + `npm run dev`. Press `c` on node 5.

| # | Step | Expected | Result | Evidence |
|---|---|---|---|---|
| 1 | `s` on node 1 before and after the run | `longest comms loop` not noticeably higher | | |
| 2 | `c` on node 5 | within ~1 s of node 5's `tx start`, dashboard shows "Arriving now · Node 5", fragments counting | | |
| 3 | same transfer | route counts for C and D, hops = 2 | | |
| 4 | on verify | capture detail: radio time, queued_ms, upload time, three CRCs | | |
| 5 | `d` (30% loss) on node 1, `c` on node 5 | NACK events, amber resent fragments, rounds >= 2, file verifies | | |
| 6 | small file over LoRa | hops shown, relay "unknown" | | |
| 7 | unplug node 1's USB mid-transfer, plug back | `bridge_status` shown; transfer completes or is marked timeout; nothing invented | | |
| 8 | relays | node 3 / node 4 print `relay wifi: N pkts in last 1 s (from node 5: N)` | | |

Serial logs of node 1, node 3 and node 5 go to `docs/verify/evidence/`.

## Notes and deviations from the spec text

- `CAPTURE_META_LEN` now means the packed (v2, 31-byte) length and `CAPTURE_META_LEN_V1` (27) was
  added next to `CAPTURE_META_LEN_V2`. Keeping it at 27 would have left every existing
  `uint8_t mb[CAPTURE_META_LEN]` buffer 4 bytes short of what `capture_meta_pack()` writes.
- `queued_ms` is stamped only for captures made in the current boot (capture ids persist and only
  grow): a job queued before a reboot has an `uptime_ms` from the old boot, so its wait is left
  missing instead of being invented.
- Real fragment sizes are WiFi 236 B and LoRa 241 B (mesh payload minus the 9-byte ALFP DATA
  overhead); the spec's `fsz=238` was only an example. The simulator uses the real values.
- `on_rx_fail` also reports refusals before a session exists (`busy`, `storage`, `too_big`) and the
  file CRC-32 mismatch (`crc`). `crc` is not final (the bridge asks for the whole file again in the
  same session): the backend logs it and keeps the transfer in progress.
- `@ALFP FILE` is now written as one line in one call (was three pieces), so an `@EVT` line can
  never land inside it. The new `first= done= nacks= dups=` fields come before `meta=`.
- The SDK's `pioasm` does not compile with WSL's GCC 15 (missing `<cstdint>`); it was built once
  outside the repo with `-include cstdint` and passed as `pioasm_DIR`. The SDK was not changed.
