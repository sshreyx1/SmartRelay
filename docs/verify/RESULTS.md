# Verification results, 2026-11-19 (branch `curveball-verify`)

Hardware: node 1 (bridge, WST-1) and node 2 (field node, carrying WST-3's ESP-01), both on the
dual-core uT-Kernel 3 SMP firmware, on one MacBook over USB. Every number below comes from a file in
[evidence/](evidence/) (large logs are gzipped). Report-text changes for the shared Google Docs are
in [DOC_CHANGES.md](DOC_CHANGES.md).

## 1. Summary

| Item | Before | After | Evidence |
| --- | --- | --- | --- |
| Bridge longest comms loop | 3 406 → 4 000 ms over 60 min (4 494 ms at start of today) | **242 ms** max over 60 min | `before_fix_n1_s.txt`, `soak60_summary.txt` |
| Bridge WiFi rxerr | 176 → 493 over 60 min | **0** over 60 min | same |
| WiFi image transfer time (9-10 KB) | 9.3 s mean | **2.4 s** mean (2.0-2.9) | `shake_summary`, `soak60_summary.txt` |
| LoRa telemetry transfer (27 B) | 7.3 s mean | **1.0 s** mean (0.8-2.2) | same |
| 60 min soak delivery | - | 107/107 files received, 107/107 streamed to laptop, 0 CRC failures on replay | `soak60_*` |
| Host tests | 14/14 | **14/14** (2 new tests added) | `ctest` |
| Backend tests | 13/13 | **13/13** | `pytest backend/tests` |
| clang-format | 33 files failing | **0** | `tools/check_style.sh` |
| cppcheck (script scope) | 6 `%u` warnings | **0** (exit 0) | cppcheck 2.22.0 |
| Firmware warnings (project code) | - | **0** in both builds | build logs |

## 2. The bridge freeze: what it actually was

Measured with a new SD-call timer (every storage call the comms task makes, slowest per kind, shown
by `s`). Two causes, stacked:

1. **Uplink inside the receive callback** (the cause found by reading). `rx_finish_ok()` called
   `on_rx_done`, which streamed the whole file to the laptop before returning: ~200 SD reads and
   ~200 log lines per image, both radios unpolled, and the sender's ACK held back until it finished.
2. **SD directory cost** (found by measuring, the bigger part). After fixing (1), `alfp service` was
   still 4 627 ms. The timer showed `open_w 2 843 ms` on `inbox/n2_01733.meta`, `rename 1 421 ms`,
   `open_r 546 ms`. Reasons: every received file lived in one flat `inbox/` (408 files on node 1);
   FatFs looks names up by reading the folder sector by sector; `.meta` is a long file name, which
   makes FatFs scan again to pick a unique 8.3 alias; and the shared SPI bus ran at 1 MHz
   (~6 ms per 512-byte sector: listing those 408 files took 505 ms).

Fixes, each measured on the bridge (3-4 min runs, capture every 20-30 s):

| Step | Longest bridge loop | `alfp service` max | WiFi rxerr |
| --- | --- | --- | --- |
| original firmware | 4 494 ms | 4 494 ms | 576 since boot |
| + uplink queue (1) | 5 218 ms | 4 883 ms | 12 → 385 in 4 min |
| + `rcv/<xx>/` folders, 8.3 `.met`, one open handle per uplink | 372 ms | 331 ms | 0 |
| + SPI bus 1 → 4 MHz | 133 ms | 116 ms | 0 |
| final, 60 min | 242 ms | 212 ms | 0 |

What changed:
- `common/uplink.c/.h`: `uplink_queue_*`. `on_rx_done` only copies the report into an 8-deep queue;
  the comms loop sends up to 8 lines per pass, and only as many as the log ring has room for
  (`log_free_slots()`), reading 384 B per SD read with the file kept open. Output is byte-identical
  to the old `uplink_send_file()` (host test compares them). If the queue is ever full the file is
  sent inline as before (`inline` counter in `s`; 0 in every run).
- `alfp/src/alfp.c`: finished files go to `<inbox_dir>/<sid>>8 in hex>/n<src>_<sid>.bin` + `.met`.
  Firmware uses `rcv/` as the root so the old flat `inbox/` (still on node 1's card, 408 files,
  safe to delete) is never scanned.
- SPI1 default 4 MHz in both builds (`board.h`, `firmware/CMakeLists.txt`, `sources.mk`). LR1121 and
  SD both run fine at 4 MHz on nodes 1 and 2. **Risk:** nodes 3-5 have not been tried at 4 MHz; a
  board with long SPI wires can be built at 1 MHz (`SPI1_BAUD_HZ=1000000`, see BRINGUP.md).

## 3. Other defects

| Defect | Fix | Verified by |
| --- | --- | --- |
| B.1 `s` printed a hard-coded core table | each task records `tk_get_prc()` every loop pass; priority from `tk_ref_tsk()` (`rtos_task_priority`) | `s`: comms core 0 prio 10, usb_owner core 0 prio 20, sensors core 1 prio 10 |
| FreeRTOS `.uf2` out of date | rebuilt all 5 (+ selftest, photo test) with classifier, telemetry split, size rule and every fix here | 4 min run on nodes 1+2: 24/24, bridge loop 279 ms (`freertos4m_summary.txt`) |
| F1 `._*` files counted as images | hidden names rejected in `sip_add_file` and payload scan | node 2 boot: 35 images in 19 groups (was 70 in 38); host test |
| F2 outbox drops silent | logged per drop + `outbox dropped (full)` in `s` | code + `s` line |
| F3 inbox names collide | first free `_1`..`_99`, never overwrites | host test `test_bridge_reboot_parts_and_names` |
| F4 SMP=0 overwrote SMP image | `csc09_smp0_node_N.uf2` | script |
| F6 "no samples" drops telemetry | any-group fallback; telemetry always sent; accurate message | host test (`test_sound_image_pick`) |
| F8 queue full vs error indistinguishable | non-`E_TMOUT` errors counted with last code | `s`: `msgq errors 0 (last 0)` |
| F9 orphan `.part` files | bridge deletes `rx/*.part` at start-up | host test |
| DHT22: priority boost vs agreed masking | SMP build masks interrupts on core 1 only around the ~5 ms bit loop (weak hooks in DHT22.c); FreeRTOS build keeps the boost (5 ms masking on its single core would overflow the ESP-01 UART) | 60 min: DHT22 good 1 730 / 1 617, **fail 0** on both nodes |
| `%u` with signed `NODE_ID` | `(unsigned)` casts | cppcheck clean |
| tools/*.sh not executable | `+x` on every script with a shebang (git mode 100755) | `git ls-files -s` |
| BARR-C 1.2.a deviation / no rule numbers | rule table + deviation cited by number in CODING_STANDARD.md | doc |
| build_smp.sh failed on macOS | `perl -pi` instead of GNU `sed -i`; makes the kernel's object dirs; `python3` | built all 5 here |

## 4. Tests on the two boards

- **T2 core assignment**: boot logs `final_n1_boot.txt`, `final_n2_boot.txt` (`sensors: core 1`,
  `comms: core 0`, `usb_owner: core 0`, both msgq self-tests ok).
- **1 ms tick** (new console key `k`: 1000 × `delay(1 ms)` timed by the hardware timer): mean
  2 000 µs, min 1 551-1 752 µs, max 2 242-2 451 µs on an idle node; kernel clock advanced 2 000 ms
  while the hardware timer advanced 1 999-2 000 ms. A 1 ms tick confirmed (uT-Kernel's delay(n)
  waits n+1 ticks, so `delay(2)` in the comms loop is ~3 ms). Before the freeze fix the same test on
  the bridge showed a 4.8 s maximum: usb_owner was starved on core 0 while comms sat in SD calls.
- **60 min soak** (`soak60_*`): capture every 2 min plus 24 real PIR+sound triggers = 54 captures;
  54 telemetry over LoRa, 53 images over WiFi (the 54th was queued 1 s before the end), all by the
  size rule, 0 failures, 0 fallbacks. Sound `lost=0` on all 54. Stack free min: comms 5 816 (bridge)
  / 5 040 (node 2) of 8 192 B, usb_owner 2 192 of 3 072, sensors 7 392 of 8 192. Heap fixed. Log and
  USB drops 0, capture queue full 0. Longest gap since the bridge was last heard: 3.2 s (limit 10 s).
  Replaying the bridge log through `tools/bridge_receiver.py`: 107 files, all CRC ok.
- **Backend end to end** (`backend_e2e_n2.log`, `verify_received_strict.txt`): live uvicorn on the
  bridge port, 7 captures: telemetry via LoRa and image via WiFi joined per capture id,
  `verify_received.py --strict-linkage` all linked, JPEG valid.
- **Reboot persistence** (`reboot_*`): bridge taken off air (flashed with `sensor_test.uf2`), 3
  captures queued 6 jobs in node 2's outbox, node 2 rebooted (fresh counters, `outbox 6` after boot),
  bridge restored: all 6 delivered (3 LoRa, 3 WiFi), outbox 0. (A reset, not a power cut.)
- **Final images** (`final5_summary.txt`): 5 min, 28/28 delivered, bridge loop max 136 ms.

## 5. Not done, needs hands or more boards

- **T7 WiFi failure**: remove node 2's ESP-01, press `c`, expect `reason=fallback` over LoRa.
- **Size ladder on hardware**: node 2's card has no `test_payloads/` (`sd: 0 test payload file(s)`).
  Copy `sd_card/test_payloads`, run `dot_clean` on the card, press `p`.
- **Power-cut** version of the reboot test (done here as a reset).
- **T1 multi-hop** and anything with 5 nodes; 4 MHz SPI on nodes 3-5 untested.
- **ESP-01 swap**: node 2 still carries WST-3's ESP-01 (boot warns). Put WST-2's on node 2 before
  node 3 joins.
- **ML tests** need Python 3.11 (this Mac has 3.13/3.14).
- **Old `inbox/`** on node 1's card (408 files) can be deleted; nothing reads it.

## 6. Reproduce

```sh
cmake -S host -B build && cmake --build build -j && ctest --test-dir build
python -m pytest backend/tests
tools/check_style.sh
SMP build: firmware_smp/buildglue/README.md "Building on macOS"
python3 tools/soak_pair.py --n1 <bridge port> --n2 <node 2 port> --minutes 60 --label soak --out docs/verify/evidence
```
