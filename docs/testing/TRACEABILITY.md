# Requirements traceability

[Back to the testing index](README.md)

Each requirement from the Week 6 design review, the µT-Kernel 3 SMP change requirement and the
Tribunal 1 test plan (T1 to T8), with the automated test and the hardware evidence that cover it.

**Status:** **Pass** = tested and met. **Partial** = met only in part, or shown indirectly. **Not run** =
no test covers it. Nothing is marked Pass without a test or a log behind it.

Where to find the evidence:

- Automated tests: `host/tests/` (C), `host/integration/` (real mesh code over a fake radio),
  `backend/tests/` (Python). Run them with `tools/run_all_tests.sh` or from the dashboard's
  **Tests** page ([README](README.md#automated-tests)).
- **25 Nov**: five boards, dual-core firmware, [report](results/2026-11-25-five-nodes/REPORT.md) and
  53 logs. Steps refer to [HARDWARE_TEST.md](HARDWARE_TEST.md).
- **19 Nov**: nodes 1 and 2, dual-core firmware, 60-minute soak, [verify/RESULTS.md](../verify/RESULTS.md).
- **Simulator**: `host/experiments/`, results in [docs/results/](../results/) and [ALFP.md](../ALFP.md).

## Functional requirements

| ID | Requirement (short) | Automated test | Hardware evidence | Status |
| --- | --- | --- | --- | --- |
| FR1 | Capture when PIR and sound are both high in the same ~10 ms poll, then a 5 s cooldown | – | 25 Nov Step 3: PIR and sound triggers fired on every node; bench PIR + sound captures every 10–15 s, all delivered | Pass (hardware only) |
| FR2 | A capture carries a photo from microSD plus node id, time, temperature, humidity, light and sound class | `test_capture_meta`, `test_metadata_delivered`, `test_sound_image_pick`; backend `test_telemetry_split` | 25 Nov Step 5: 35 images in 19 groups on every field node; dashboard shows the readings per capture | Pass |
| FR3 | Choose one radio per transfer and never switch mid-transfer | `test_radio_selection`, `test_radio_selection_for_size`, `test_outbox_size_based_routing`, `test_never_switch_mid_transfer` | 25 Nov Step 5: telemetry `reason=size` over LoRa, images over WiFi; Step 6B: `reason=fallback` to LoRa when the relay was removed | Pass |
| FR4 | Fragments with sequence number and CRC; resend only the missing ones | `test_nack_exact`, `test_crc_catches_bit_flips`, `test_lora_loss_sweep_multihop`, `test_wifi_heavy_loss`; dashboard pipeline check | 25 Nov Step 7: 30% loss, every image `done`, 10–31 fragments resent of about 40 | Pass |
| FR5 | Two independent flooding meshes with duplicate suppression (30 s) and hop limit 4 | `test_duplicate_suppression`, `test_other_node_not_delivered`; `host/integration` LoRa and WiFi runs (E → C → A) | 25 Nov Step 6: node 4 relayed 140 → 574 packets for node 5; 6A relay counters rose on nodes 2 and 3 | Pass |
| FR6 | Bridge reassembles, checks CRC-32, asks for resends, streams to the laptop over USB | `test_sd_fault_caught_by_crc32`, `test_uplink`, `test_bridge_reboot_part_names`; backend tests; pipeline check | 19 Nov: 107 of 107 files streamed, 0 CRC failures on replay; 25 Nov: 256 transfers, none lost | Pass |
| FR7 | Bridge heartbeat every 3 s on each mesh | `test_heartbeat_all_neighbours` | 25 Nov Step 4: bridge heard on both radios within 0.4–2.0 s on nodes 3, 4 and 5 (node 2's faulty LoRa module heard nothing; WiFi 0.5 s); 19 Nov: longest gap 3.2 s | Pass |
| FR8 | Record 5 s of audio after a trigger; energy, zero-crossing rate and a sound class | `test_audio`, `test_sound_features`, `test_sound_model`; `ml/tests` | 19 Nov: 54 captures, 0 samples lost; 25 Nov: classes on nodes 2–4. Node 5's microphone was disconnected (flat signal) | Pass (node 5 hardware fault) |
| FR9 | Weather label from temperature, humidity and light attached to each capture | `test_capture_meta` (weather field round trip) | 25 Nov Step 3: `Weather: <label>` on every node | Pass |
| FR10 | Captures wait on microSD when no bridge is reachable, or after giving up, and are sent later | `test_store_and_forward`, `test_outbox_survives_reboot`, `test_abandoned_session_expires` | 25 Nov Step 8: 20, 20 and 14 files queued on nodes 3, 4 and 5, all delivered; 19 Nov: 6 jobs survived a reboot | Pass |
| FR11 | Injected packet loss to demonstrate recovery | `test_30pct_loss_completes`, `test_lora_loss_sweep_multihop` (several rates), integration runs at 20% | 25 Nov Step 7: `d` gives 30% receive loss on a board | Partial: the console offers 30% only; other rates are set in code |
| FR12 | Base station shows each capture with its metadata and the status of every node | backend `test_dashboard_api`, `test_pipeline_and_runner` | 19 Nov: live backend on the bridge port, 7 captures joined and verified | Pass |

## Non-functional requirements

| ID | Requirement (short) | Automated test | Hardware evidence | Status |
| --- | --- | --- | --- | --- |
| NFR1 | Each radio serviced often enough that no received packet is lost (UART ring holds about 175 ms) | – | 19 Nov bridge: longest comms loop 242 ms, yet WiFi `rxerr` 0 over 60 min. 25 Nov node 2 (slow SD card): loop 511 ms, `rxerr` rose under heavy relay traffic, every file still delivered | Partial: the worst loop exceeds the ring's 175 ms; ALFP recovers the loss |
| NFR2 | `send()` and `recv()` never block | design: ALFP only polls `tx_busy()`; `test_progress_callbacks` | 25 Nov Step 9: the node kept working during a 5 s console stall | Pass |
| NFR3 | A corrupted packet never reaches the fragmentation layer | `test_crc_catches_bit_flips`, `test_decoder_fuzz`, `test_corruption_caught_by_crc16` | 25 Nov: node 2 `rxerr` counted, no corrupted file accepted | Pass |
| NFR4 | WiFi mesh at least 40 packets/s over 2 hops | simulator: 45 packets/s | 25 Nov: a 9–10 KB image (about 40 fragments) over 2 hops in 3.3 s median, which includes ALFP polling | Not run as specified (packets/s not measured on hardware) |
| NFR5 | At 20% loss, at least 90% of WiFi packets arrive over 2 hops before any resend | simulator: 96%; `host/integration` at 20% loss | – | Partial (simulation only) |
| NFR6 | Bridge declared lost after 10 s of silence | `test_radio_selection` (fresh window) | 25 Nov Step 8 and 6B: sending stopped, then switched radio, as expected | Pass |
| NFR7 | A ~20 KB capture within 10 s over WiFi; about 150 s over LoRa | `test_edge_sizes`; simulator size ladder (`e6_size.csv`) | 25 Nov: 9–10 KB images 2.6 s (1 hop) and 3.3 s (2 hops) over WiFi; 43.5 s over LoRa | Partial: 20 KB not run on hardware |
| NFR8 | Temperature, humidity and light every 2.1 s | – | 19 Nov: DHT22 1 730 and 1 617 good reads, 0 failures | Pass |
| NFR9 | Sound at 16 kHz in 5 s windows, no samples lost | `test_audio` | 19 Nov: 80 000 samples per capture, 0 lost on all 54 captures | Pass |
| NFR10 | LoRa only in AS923 (924.2 MHz, 14 dBm) | – | 25 Nov boot logs: `924.2 MHz SF7 BW125 14 dBm` | Pass |
| NFR11 | First fragment on air within 1 s of the capture being queued | – | 25 Nov logs: the telemetry transfer started 0–43 ms after `queued` on all 24 captures checked | Pass |

## µT-Kernel 3 SMP change requirement (Week 13)

| Requirement | Where it is documented | Evidence | Status |
| --- | --- | --- | --- |
| Every application task is a µT-Kernel task; FreeRTOS schedules nothing | [MICROKERNEL.md](../MICROKERNEL.md#tasks-and-cores) | boot log `uT-Kernel 3 SMP build, dual core` on all five nodes (25 Nov) | Pass |
| Task-to-core assignment documented and justified | [MICROKERNEL.md](../MICROKERNEL.md#tasks-and-cores); Tribunal 1 record | `comms: core 0`, `usb_owner: core 0`, `sensors: core 1` in the boot log and in `s` on every node | Pass |
| Inter-task communication, shared-resource protection, interrupt-to-task interaction | [MICROKERNEL.md](../MICROKERNEL.md#how-the-tasks-talk), [FAULTS.md](../FAULTS.md#queues-rings-and-limits) | message-queue self-test `ok` at every boot; T6 and T8 below | Pass |
| All existing functionality still works | this page | 25 Nov: every hardware step on five nodes | Pass (two hardware faults, not firmware) |
| No unacceptable loss of performance | [MICROKERNEL.md](../MICROKERNEL.md#evidence-from-the-boards), `docs/evidence/smp/` | same boards, same 150 s run (11 Nov, `docs/evidence/smp/ab_*`): image mean 5.02 s on both kernels, telemetry 2.47 s (FreeRTOS) and 2.50 s (dual core), 12 of 12 files each. After the 19 Nov bridge fixes the dual-core build sends an image in 2.4 s and telemetry in 1.0 s | Pass |

## Tribunal 1 test plan (T1 to T8)

| Test | What it checks | Evidence | Status |
| --- | --- | --- | --- |
| T1 | Worst-case comms timing: multi-hop while SD writes and the dashboard streams | 25 Nov: two-hop delivery and relaying (Steps 5 and 6); 19 Nov: longest bridge loop 242 ms over 60 min while streaming to the laptop | Partial: the 200-packet / 40 packets-per-second run was not done; transfers were timed instead |
| T2 | Core assignment and pinning | boot logs on all five nodes (25 Nov); `docs/evidence/smp/t2_*` | Pass |
| T3 | Audio capture under radio and SD load | 19 Nov: 0 lost samples over 54 captures during the soak | Pass |
| T4 | DHT22 on core 1 under radio load | 19 Nov: 0 failures in 3 347 reads after moving the timing code to RAM and masking interrupts on core 1 | Pass |
| T5 | Merged versus split sensors task | design decision to keep one task (shared ADC); T3 and T4 show the merged task meets its timing | Not run (comparison not built) |
| T6 | Capture queue full | `docs/evidence/smp/t6_*`; 25 Nov Step 9 (`x`): `capture q full 1`, nothing froze | Pass |
| T7 | WiFi failure → LoRa | 25 Nov Step 6B: relay removed, images `reason=fallback` over LoRa, all delivered | Pass |
| T8 | USB output from both cores, dashboard not reading | `docs/evidence/smp/t8_*`; 25 Nov Step 9 (`z`) | Pass |

## Coding standard

| Requirement | Test | Status |
| --- | --- | --- |
| Code conforms to BARR-C:2018 | `tools/check_style.sh` (clang-format layout, `tools/check_barrc.py` source rules, warnings-as-errors compile); see [CODING_STANDARD.md](../CODING_STANDARD.md) | Pass on 28 Nov (macOS) |
