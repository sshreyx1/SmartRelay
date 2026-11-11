# SMP handoff verification - status log

Verification of `docs/SMP_HANDOFF.md` (last hardware session 2026-11-04) before continuing its
next steps. Started 2026-11-11 on a macOS laptop (the handoff was written on Windows).
Rules for this pass: no git state changes, no code changes, findings only.

Legend: ✅ matches the handoff · ⚠️ differs / caveat · ❌ broken · ⏳ not run yet

---

## Step 1 - repo state

| Check | Result |
|---|---|
| Branch | ✅ `curveball-integration`, up to date with `origin/curveball-integration` |
| Working tree | ✅ clean (also clean inside `third_party/mtk3smp_rp2040`) |
| HEAD | ✅ `5b3aff8 Add dual-core SMP firmware images for nodes 1-5`; the 5 commits above `35f3820` are the handoff session's |
| Submodule | ✅ `third_party/mtk3smp_rp2040` at `900e863` (the pin `docs/curveball_findings.md` names) |

Every file in handoff section 3.1 exists, last touched by the handoff session's commits
(`094bf31` build glue, `50fd488` app/port, `b66ab77` helpers, `5b3aff8` images);
`pico_sdk_glue_stubs.c` is removed as stated. Spot-checked claims, all ✅ present in code:
`log_putc()` + 500 ms line wait, `LOG_RING_DEPTH 64`, `smartrelay_runtime_init()` called from
`usermain_smp.c`, `rtos_msgq_send_wait` / `rtos_self_id` / `rtos_atomic_cas_i32`, 24 KB task
stack pool, 24 KB `SMARTRELAY_HEAP_BYTES`, `clock_set_reported_hz`, DHT22 `.text` ->
`.data.smartrelay_ramfunc_dht22` rename in `sources.mk`, `.mutex_array` in the section script,
console keys `H p x z B`, per-step comms timing (`slowest comms step`).

## Step 2 - builds (all done in a throwaway copy of the repo, see "How" below)

| Build | Result | Sizes (`arm-none-eabi-size`: text / data / bss) |
|---|---|---|
| SMP=1 nodes 1-5 (`build_smp.sh`) | ✅ compiles + links, 60 s total | node 1: 292161 / 0 / 162608 · nodes 2-5: 294209 / 0 / 162608 |
| SMP=0 node 2 (never built before) | ✅ compiles + links (full kernel rebuild, profile stamp works) | 283705 / 0 / 146240; no core-1 symbols |
| C2 smoke (`build_smoke.sh`, SMP=1 node 2) | ⚠️ builds **only from a clean submodule** (see risks) | 66946 / 64 / 66176 |
| FreeRTOS `firmware/` (all 10 images) | ✅ compiles + links, 45 s | node 1: 229932 / 0 / 113764 · nodes 2-5: 233220 / 0 / 113768 |
| Host tests (`ctest`, `host/`) | ✅ 14/14 passed, 0 compiler warnings | - |

Size notes:
- "data" shows 0 because the kernel's/SDK's link scripts fold `.data` into a load section counted
  as text. Real SMP=1 RAM: vector 192 + `.data` 3748 + noinit 16 + `.bss` 162400 = **166356 B
  (162.5 KiB)** - ✅ matches the handoff's "static use is ~163 KB of 264 KB".
- Rebuilt SMP UF2s are 3072 B (6 blocks) smaller than the committed ones (node 1: 584704 vs
  587776). Expected from the compiler difference (see below); not byte-identical, as expected.
- ⚠️ The committed FreeRTOS `uf2/csc09_node_{1..5}.uf2` are from the **Initial commit
  (2026-10-10)** and ~100 KB smaller than a fresh build (node 1: 354304 vs 460288 B). They predate
  8 firmware commits (sound classifier, persistent capture ids, telemetry/image split, size-based
  radio selection, `H` key). `alfp/src/alfp.c` and `firmware/app/storage_fatfs.c` are unchanged
  since then and `common/capture_meta.h` changed in comments only, so the section 5 A/B test (which
  uses the committed `csc09_node_1.uf2` as the bridge) is still a fair test of the receive-finish
  path - but a "FreeRTOS node" in that test is not today's FreeRTOS firmware.
- Warnings (45 in the SMP=1 log, same 9 kinds x 5 nodes): newlib `_read/_lseek/...` "not
  implemented" stubs, LR1121 driver `#warning` about sync word, `LOAD segment with RWX` (the RAM
  DHT22 code). None new, none errors.

### How (environment differences from handoff section 4.1)

- Toolchain: **Arm GNU 15.2.1** from `~/.pico-sdk/toolchain/15_2_Rel1` (handoff used 14.2);
  CMake 4.3.4 + Ninja from `~/.pico-sdk`; Homebrew `gnu-sed` and `make` (GNU make 4.4.1) installed
  and put first on PATH - `build_smp.sh` uses GNU `sed -i` / `\s\+` / `\b`, which macOS BSD sed
  does not accept. `PYTHON=python3` (the script defaults to `python`).
- Deps: pinned versions re-fetched with `tools/fetch_deps.sh` (pico-sdk 2.1.1 `bddd20f9`,
  FreeRTOS `8be86d4a2`, FatFS `d5e4534`). The existing copies under
  `~/Documents/GitHub/SmartRelay/deps` are the right versions but **iCloud has evicted them**
  (`compressed,dataless`); reads timed out mid-compile and `brctl download` / `cp` also timed out.
  The ~/.pico-sdk SDKs are 2.3.0/2.3.1, not the required 2.1.1.
- Built in a copy of the repo (scratchpad), because `build_smp.sh` always writes
  `uf2/csc09_smp_node_N.uf2` and would overwrite the committed images. The real repo was checked
  clean afterwards.
- `build_smp.sh` / `build_smoke.sh` / `run_all_tests.sh` are mode 100644 in git: run them with
  `bash <script>`, not `./<script>`.

## Step 3 - hardware re-check

⏳ In progress - see below as results come in.

| # | Test | Result | Evidence |
|---|---|---|---|
| 3a | Boot + core pinning (T2) on SMP node 2 | ✅ PASS (retry 2): `dual core`, `sensors: core 1`, `comms: core 0`, `usb_owner: core 0` | see 3a notes |
| 3b | Re-run measured results (handoff section 7), SMP bridge | ⚠️ mostly matches; the 11 s problem did **not** reproduce: bridge finish is now ~4.5-5 s, all transfers `done` | see 3b notes |
| 3c | Section 5 A/B: FreeRTOS bridge receive-finish time | ⚠️ partly: 1 image in the window; FreeRTOS bridge was **not faster** (8.2 s vs 7.1-7.6 s on SMP) | see 3c notes |

### 3a notes (2026-11-11, node 2 flashed with committed `uf2/csc09_smp_node_2.uf2`, macOS, node 1 unplugged)

Port `/dev/cu.usbmodemMTK3PICO1`. 40 s capture, `s` at 30 s.

- ✅ The new image is running: `s` has the `slowest comms step` line the old build lacked.
- ⚠️ Boot lines lost: the capture starts at `, button GP20` (tail of the sensors pin line). The
  usb_owner task holds start-up lines only 3 s for a host to open the port (`main_smp.c:800`);
  cp -> re-enumerate -> `flash_node.py`'s 1 s sleep took longer on macOS. Lost lines are **not**
  counted in `usb dropped` (it read 0). Banner, `msgq selftest`, `clocks (measured)`,
  `comms: core`, `sensors: core`, `sd:`, `wifi:` lines therefore not yet seen.
- ✅ `usb_owner: core 0` (measured by `rtos_core_id()`).
- ✅ Matches handoff section 7: stack free comms 5152/8192, usb_owner 2248/3072, sensors 7400/8192;
  `dht22 good 10 fail 0`; audio `lost=0`, `feat_us` 917-947 ms, `model_us` 980-1164; `lora up`,
  `wifi up`; outbox 25 before the first new capture (handoff: ~26); capture ids persist
  (N2-000594..596, handoff saw N2-000338).
- ⚠️ `heap used 1224 B` on node 2 (handoff: 21.7 KB on node 2). No transfers happened (bridge
  unplugged), so not comparable yet; re-check in 3b.
- ⚠️ A capture fires every ~12 s: `PIR 1` in every `[ENV]` line plus legacy `mic.c` reports
  `LOUD_ANIMAL_CALL` with the same energy (~3.78e6, zcr 0.000) every time while the classifier
  sees rms 2-3 (near silence). Outbox grows by 2 per capture (25 -> 29 in 40 s).
- ⚠️ `slowest comms step: captures 886 ms` (longest comms loop 1258 ms): queuing one capture costs
  ~0.9 s of comms time on node 2's SD card - node 2's card is slow too, not only node 1's.

3a retry 1 (replug + `serial_tstamp.py` in a wait loop): ❌ no boot lines at all - first line is a
periodic `[ENV]` at +3.0 s, i.e. the board had been running for a while before the port was
opened (most likely the board was already plugged when the wait loop started). `PIR 0` in all
three lines, no captures in 30 s. Next: a capture helper that refuses to start while a port
exists and reports appear->open latency.

3a retry 2 (scratchpad `boot_capture.py`, port opened 0.029 s after it appeared): ✅ **T2 PASS.**
Boot lines arrived at +1.896 s, so the 3 s hold works when the port is opened in time.
- `CSC09 trail mesh, node 2, ALFP v1 (uT-Kernel 3 SMP build, dual core)`
- `msgq selftest: wanted 64 ... drained 64 - ok` (log ring) and `wanted 4 ... drained 4 - ok`
  (capture queue)
- `clocks (measured): sys 125001000 peri 125000000 usb 48000000 adc 48000000 Hz` ✅ (handoff: peri
  125.001 MHz; 1 kHz difference is counter resolution)
- `sensors: core 1`, `comms: core 0`, `usb_owner: core 0` - all measured with `rtos_core_id()`
- LR1121 fw 0x0101 hw 0x22 up, 924.2 MHz SF7; `wifi: ESP-01 MAC AC:0B:FB:C8:19:96 = WST-2`;
  `node 2 ready (field node), outbox has 64 queued`; next capture id N2-000620.
- ⚠️ Outbox is at its cap: `ALFP_OUTBOX_MAX 64` (`alfp/include/alfp_outbox.h:26`) drops the
  **oldest** job when full (`alfp_outbox.c:137`). Each new capture still logs "queued ...,
  outbox 64", and `dropped_full` is never printed by either firmware, so the drops are silent.
  The ~26 jobs the handoff mentions have likely been evicted by newer captures (ids 596 -> 619
  between runs while the PIR was triggering).
- ⚠️ Node 2's SD `samples/` holds macOS AppleDouble files: `70 sample images in 38 groups`, half
  are `._ambient`, `._bird`, ... (resource-fork stubs from copying with Finder). They count as
  image groups; if any pick path falls back to an arbitrary group, a `._*.jpg` (not a JPEG) could
  be sent. Not changed.
- ⚠️ PIR still reads 1 and dual triggers still fire (captures 620, 621) - covered or not at this
  point is to be confirmed.

### 3b notes (node 1 flashed with committed `csc09_smp_node_1.uf2`; node 2 PIR covered)

macOS ports: node 1 `/dev/cu.usbmodemMTK3PICO1`, node 2 `/dev/cu.usbmodem4` (the second board
with the same `MTK3PICO` serial gets a location-based name). 150 s simultaneous capture.

- ⚠️ Node 1 boot lines missing although the port was opened 0.028 s after it appeared: the
  first bytes (+0.083 s) are the tail of the help text, which is printed right after the 3 s hold.
  So on node 1 the port only appeared after the hold expired. Hypothesis (not verified): USB
  enumeration on core 0 is delayed > 3 s by the higher-priority comms task's SD mount/scan of
  node 1's very full card. Node 1 is confirmed as the SMP build only from `s` (per-task table +
  `slowest comms step`), not from measured core lines; its banner/MAC lines are not seen.
- ✅ No `[ALERT]` lines on node 2 in 150 s: PIR cover works, no captures during the measurement.
- **Half-done problem, result now (SMP bridge):** node 2 sent 23 files (12 telemetry 27 B,
  11 images 9.2-10 KB), **all `done via wifi`, 0 `gave_up`**. Node 1 received each exactly once
  (`inbox/n2_00156..00178.bin`, no duplicate files; `dup 1` only where node 2 resent 2-4
  fragments). 46 `@ALFP` lines = 23 files x (FILE + END).
- But the bridge is still slow: telemetry (1 fragment, 27 B) takes **4.7-4.9 s** end to end every
  time; images (40-43 fragments) 7.1-7.6 s. Node 1 `s`: `alfp service 4985 ms`, longest comms
  loop 4985 ms, wifi `rxerr 707`, `dup 59`. Handoff: 11 508 ms and `gave_up` at 11.0 s. So the
  receive-finish stall is ~4.5-5 s now (down from ~11 s), below node 2's give-up time.
  Not explained yet: same images, same card (as far as we know).
- Node 2 `s` also shows `captures 1121 ms` as its slowest step: ~1 s per SD create on node 2's
  card too. `rx_finish_ok` does ~5 directory-scanning operations (close, unlink, rename, create
  .meta, close), so ~1 s each would give the ~4.7 s seen. Both cards being ~1 s per create is
  consistent with either "both cards are full" or "SD is slow under SMP" - 3c decides.
- Matches handoff section 7: ✅ node 1 `lora DOWN` (known hardware fault), `wifi up`;
  ✅ DHT22 node 1 152/0, node 2 153/0; ✅ stacks node 1 comms 5536 / usb_owner 2264 / sensors 7408,
  node 2 5096 / 2264 / 7400; ✅ `log dropped 0`, `usb dropped 0` on both.
- ⚠️ Heap: node 2 **1224 B** after 54 tx ok (handoff 21.7 KB - not reproduced); node 1 **5320 B**
  (handoff 1.2 KB bridge).
- Node 2 outbox went 64 -> 22 (backlog is draining).

### 3c notes (node 1 flashed with committed FreeRTOS `uf2/csc09_node_1.uf2`, same SD card; node 2 unchanged, PIR covered)

macOS ports: node 1 (FreeRTOS) `/dev/cu.usbmodem101`, node 2 `/dev/cu.usbmodemMTK3PICO1`.
Node 1 (FreeRTOS) boot lines were all captured (it sleeps 1.5 s before its first line):
`CSC09 trail mesh, node 1, ALFP v1` (old banner, no SMP tag), `sd: mounted, 0 sample images`
at +1.04 s, `lora_radio: unexpected chip type 0x00` + `lora: init FAILED` (✅ same hardware fault on
FreeRTOS), `wifi: ... AC:0B:FB:C7:7F:E9 = WST-1`, `node 1 ready (bridge)`.

- ⚠️ Not enough traffic: node 2's backlog was used up (6 files, sids 10807-10812) mostly before the
  150 s A/B capture started (`outbox 0`, `tx ok 6`). In the window: **one image**,
  `insect_frog_00.jpg` 10032 B, 43 frags, `done via wifi, 1 rounds, 0 resent, 8.2 s`. No
  telemetry `tx` line was captured, so the cleanest metric (1-fragment telemetry time, 4.7-4.9 s on
  the SMP bridge) has no FreeRTOS sample.
- What the one sample says: the FreeRTOS bridge is **not faster** than the SMP bridge for the same
  43-fragment file on the same card (8.2 s vs 7.3-7.6 s for the 43-fragment files in 3b).
- Supporting: node 1 wifi `rxerr` per received file is the same on both firmwares (FreeRTOS 73 / 6
  files = 12.2; SMP 707 / 53 = 13.3). The handoff explains `rxerr` as UART ring overflow while
  comms is stuck, so the same per-file stall exists on FreeRTOS.
- Lean: **card / FatFs, not the SMP port** (handoff section 5 branch 1) - but n = 1, not a
  conclusive A/B.
- FreeRTOS `s` has no per-step timing (`comms stack free: 1535 words, heap free 30592 bytes`).
- Node 2 SMP: `heap used 1224 B`, `captures 881 ms` slowest step again (no captures happened in
  this boot's window either - this step costs ~0.9 s even when idle? unresolved).
- To finish: a fair pair with the same driven traffic on both bridges (see Step 4).

Decision (user, 2026-11-11): for 3b/3c node 2's PIR is physically covered to stop captures; `H`
is NOT used (it changes comms behaviour during the measurement).

## Risks / unclear points found so far

1. `s` prints each task's core from a **hard-coded table** (`main_smp.c:497-499`,
   `RTOS_CORE_0/1` constants), not a measurement. Only the boot lines `comms: core N`,
   `sensors: core N`, `usb_owner: core N` call `rtos_core_id()`. T2 evidence must come from the
   boot lines (README's T2 says "press `s`", which would pass even if pinning were broken). On
   an SMP=0 build `s` would still say "sensors core 1".
2. `SMP=0` writes the same output name `uf2/csc09_smp_node_N.uf2` as SMP=1 - running the
   handoff's single-core command in the real repo silently replaces the committed dual-core image.
3. `build_smoke.sh` cannot run after `build_smp.sh`: it does not restore `config.h` (fails on
   the `CNF_TIMER_PERIOD` guard) nor remove `main_smp.c` & co. from `app_program/` (then fails on
   `alfp.h: No such file`). Builds fine from a clean submodule
   (`git -C third_party/mtk3smp_rp2040 checkout -- . && git -C third_party/mtk3smp_rp2040 clean -fd app_program build_make/sources.mk`).
4. `flash_node.py` flashing is Windows-only (`ctypes.windll` drive lookup); `--no-flash` console
   mode is portable. On macOS flash with `cp file.uf2 /Volumes/RPI-RP2/`.
5. Both SMP boards share USB serial `MTK3PICO`; on macOS the `/dev/cu.usbmodem*` names must be
   identified by plugging boards one at a time.
6. Committed FreeRTOS UF2s are stale (above).
7. Boot lines are silently lost if the port is not open within 3 s of boot, and `usb dropped`
   does not count them - a T2 "pass" needs a capture that actually shows the boot lines.
8. Continuous PIR/sound triggers on node 2 keep adding SD writes and transfers; for the A/B
   (3b/3c) node 2 must be in the same condition for both bridge runs.
9. Outbox cap (64) drops the oldest job silently; `dropped_full` is not shown in `s`.
10. `._*` AppleDouble files on node 2's SD `samples/` are counted as sample-image groups.
11. Inbox names are `n<src>_<sid>` with a 16-bit session id that starts at a random value each
    boot (`alfp.c:104`), and `s_rename` unlinks the target first (`storage_fatfs.c:122`): on a
    long-used bridge card a new file can silently overwrite an older received file with the same
    sid. The handoff's "session ids in the 24 000s" therefore says nothing about card age.
12. Node 1's boot output is lost even with an instant port open (3b) - T2 on node 1 is not
    evidenced by measured core lines.

---

## Step 4 - report (2026-11-11)

### T1-T8 status (definitions: README.md "Running the T1-T8 tests")

| Test | Status | Evidence |
|---|---|---|
| T1 throughput/stability, 200 packets over 2 hops | not run | needs nodes 3-5 (2 hops) and the backend; 1-hop WiFi only so far: 23 files, 0 failures (3b) |
| T2 core assignment | ✅ passed on node 2 | measured boot lines `sensors: core 1`, `comms: core 0`, `usb_owner: core 0` (3a retry 2); node 1 boot lines not captured |
| T3 audio under load | partly | `lost=0` on every capture (3a), but not during a T1 load |
| T4 DHT22 under load | partly | 152/0 (bridge) and 153/0 (sender) during 23 WiFi transfers (3b); not under T1 |
| T5 merged sensor task | not run | = T3 + T4 under T1 |
| T6 capture queue full (`x`) | not run | `capture q full 0` only |
| T7 WiFi -> LoRa failover | not run, blocked | node 1 LoRa hardware fault on both firmwares |
| T8 console stall (`z`) | not run | `log dropped 0` / `usb dropped 0` in normal running only |

---

## Follow-ups (recorded 2026-11-11, not fixed yet)

| # | Item | Where | Why it matters |
|---|---|---|---|
| F1 | macOS `._*` AppleDouble files in node 2's SD `samples/` are counted as sample-image groups (`70 sample images in 38 groups`, half `._*`) | SD card content; sample scan in `main_smp.c` / `main.c` | a fallback pick could send a `._*.jpg` that is not a JPEG; group counts are wrong. Fix: delete them from the card and/or skip names starting with `._` in the scan |
| F2 | Outbox drops the oldest job silently when full (`ALFP_OUTBOX_MAX 64`); `dropped_full` is never printed | `alfp/src/alfp_outbox.c:137-151`, `s` output of both firmwares | lost captures are invisible; captures still log "queued ..., outbox 64". Fix: print `dropped_full` in `s` and log each drop |
| F3 | Inbox names `n<src>_<16-bit sid>`, sid starts random per boot, and `s_rename` unlinks the target first | `alfp/src/alfp.c:104,883`, `firmware/app/storage_fatfs.c:122` | on a long-used bridge card a new file can silently overwrite an older received file. Fix: wider/unique names or refuse to overwrite |
| F4 | `SMP=0` writes the same output name `uf2/csc09_smp_node_N.uf2` as the dual-core build | `firmware_smp/buildglue/build_smp.sh` (`OUT_DIR`, uf2 name) | the documented single-core command silently replaces a committed dual-core image. Fix: include `smp0` in the name |
| F5 | Node 2's `captures` comms step costs ~0.9-1.1 s (881-1121 ms), including in a run with no visible captures | node 2 `s` (`slowest comms step`), `main_smp.c` captures step | stalls comms on the sender for ~1 s; cause unknown (SD create latency on node 2's card?). Needs a timed breakdown |
