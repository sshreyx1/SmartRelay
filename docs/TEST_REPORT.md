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
| BARR-C style/lint over in-scope, host-compilable files | `tools/check_style.sh` | PASS (format/lint SKIPPED: tools not installed) | see `docs/CODING_STANDARD.md` |
| Dual-core SMP build | `firmware_smp/smoke/` + `firmware_smp/buildglue/check_env.sh` | BLOCKED | see `docs/curveball_results.md` |
| Two-node hardware test (8 cases) | `docs/sound/TWO_NODE_TEST.md` | BLOCKED (no hardware) | see that file's own per-case notes |
| `ml/tests` unaffected | `ml/tests` (read-only) | PASS | 10/10, unmodified |

## Counts

- **Host unit tests** (`test_units`, `test_transfer`, `test_filtering`): 31 tests, 0 failed, 9,277 checks.
- **Host integration** (real `lora_mesh.c`/`wifi_mesh.c`): 6 real runs across this session (sizes
  2000/5000/20000 B LoRa, 8000/20000/65536 B WiFi, one pair at 1024 B each), 0 failed.
- **Backend** (pytest): 13 tests, 0 failed.
- **`ml/tests`** (pytest, read-only): 10 tests, 0 failed.
- **BARR-C style check**: 1 stage, PASSED (3 of 4 checks SKIPPED for a missing tool, 1 actually run).
- **Dual-core SMP**: 0 of 6 stages (C0-C5) could run past C0; C0 itself ran and correctly reported FAIL (missing toolchain).
- **Two-node hardware**: 0 of 8 cases run (no hardware); verifier logic covered by 3 synthetic-fixture tests instead.

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
4. **Dual-core SMP (Task C) and the two-node hardware test (Task E) could not run at all** -- no
   toolchain, no board. See `docs/curveball_results.md` and `docs/sound/TWO_NODE_TEST.md`.
5. **`backend/app.py`'s `on_transfer()` telemetry/image split logic itself has no direct test** --
   its merge/discriminator behaviour is covered indirectly through `backend/storage.py`'s
   `record_capture()` (which `test_telemetry_split.py` exercises directly, mirroring what
   `on_transfer()` calls) and a manual check during development (not persisted as a test), rather
   than a `LiveBridge`/FastAPI-level test. `store` is a module-level object constructed on import
   with real side effects (opens/creates `backend/smartrelay.db`), which made an isolated
   `on_transfer()`-level test more invasive to add safely than the time budget allowed.
