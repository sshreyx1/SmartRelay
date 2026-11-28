# Faults: detection, response and recovery

[Back to the README](../README.md)

Every fault the firmware and the laptop side handle, how each is noticed, what the system does, and
how it gets back to normal. The last column points at the test or hardware run that exercised it.
Names in `code` are what the serial console prints; `s` prints the counters.

## On a node

| Fault | Detection | Response | Recovery | Evidence |
| --- | --- | --- | --- | --- |
| microSD missing or unreadable at boot | `f_mount` fails | prints `sd: mount failed ... retrying`; the comms task waits (nothing can be queued without the card) | retries every 2 s; starts normally once mounted | 25 Nov, node 1 fault #2 ([report](testing/results/2026-11-25-five-nodes/REPORT.md#7-hardware-faults-found)) |
| microSD error while running | FatFs result code | `s`: `<n> SD errors, last: <op> FR=<code> on <path>`; a capture whose telemetry cannot be written prints `telemetry SD write failed`; a file that cannot be read ends `io_error` | an unreadable file is dropped from the outbox (it can never succeed); everything else carries on | `test_sd_fault_caught_by_crc32` |
| LoRa module fails to start | `lora_mesh_init()` fails (chip type, BUSY timeout) | prints `lora: init FAILED ... Continuing with WiFi only` | LoRa never hears the bridge, so the size rule falls back to WiFi for every file | 25 Nov, node 1 fault #1 |
| LoRa module stops receiving after start | **not detected** (no LoRa health check) | the bridge's LoRa heartbeat is no longer heard, so after 10 s files use WiFi (`reason=fallback`) | reboot the node | 25 Nov, faults #3 and #4; recommendation R1 |
| ESP-01 missing or not answering | no reply to WHOAMI within 2 s | no `wifi: ESP-01 MAC` line; WiFi is not used | the size rule falls back to LoRa | boot log check, [HARDWARE_TEST.md](testing/HARDWARE_TEST.md) Step 1 |
| Wrong ESP-01 fitted | its MAC belongs to another node number | boot warning; `w` prints `MISMATCH` | fit the right module (the WiFi topology filter depends on it) | Step 2 |
| Corrupted frame on the Pico–ESP-01 UART | CRC-8 per frame | frame dropped, `wifi: rxerr` counted; never reaches ALFP | ALFP asks for the missing fragment again | 25 Nov: node 2 `rxerr` 0 → 590 under heavy relay traffic, every file still delivered |
| UART receive ring full (2 KB, about 175 ms of data) | the interrupt handler finds no space | byte dropped and `uart_overflow` counted; the broken frame then fails CRC-8 | ALFP resend | 19 Nov: WiFi `rxerr` 176 → 0 after the bridge loop fix ([verify/RESULTS.md](verify/RESULTS.md)) |
| Corrupted LoRa packet | radio CRC | dropped, `lora: rxerr` counted | ALFP resend | 25 Nov, node 2 antenna swap |
| Fragment lost on air | the bridge's list of received fragments has gaps when the sender POLLs | bridge answers with a NACK listing exactly the missing fragments | only those fragments are resent; the last few are sent twice | Step 7 (30% loss); `test_lora_loss_sweep_multihop`, `test_wifi_heavy_loss` |
| MANIFEST, POLL or ACK lost | sender gets no answer within the poll timeout (1.5 s LoRa, 0.2 s WiFi) | POLLs again | carries on once an answer arrives | `test_manifest_lost`, `test_ack_lost` |
| Fragment corrupted but passes the radio check | CRC-16 per fragment | fragment discarded and asked for again | resend | `test_corruption_caught_by_crc16` |
| Whole file wrong after reassembly | CRC-32 over the file, on the bridge | bridge replies `CRC_FAIL` and clears the file | sender sends every fragment again | `test_sd_fault_caught_by_crc32` |
| Bridge not heard on either radio | no heartbeat (every 3 s) for 10 s | nothing is sent; captures wait in the outbox on SD | sent automatically, oldest first, once the bridge is heard | Step 8: 20 files queued, all delivered in about 40 s |
| Bridge or relay lost mid-transfer | no answer to POLLs | gives up after 30 POLLs on LoRa (about 45 s) or 40 on WiFi, 40 rounds, or 10 rounds with no progress: `gave_up` | the file stays in the outbox; retried with back-off of 5, 10, 20 ... 60 s, on whichever radio reaches the bridge then | Step 8 (`gave_up` 60.8 s, then `done`); Step 6B (WiFi `gave_up`, then `done` over LoRa) |
| Preferred radio cannot reach the bridge | bridge last heard on it more than 10 s ago | the other radio is used for the whole file (`reason=fallback`); a file never changes radio mid-transfer | back to the size rule when the bridge is heard again | Step 6B; `test_radio_selection`, `test_never_switch_mid_transfer` |
| Several nodes send to the bridge at once | the bridge has 4 receive sessions, one per sender | a fifth sender is refused with `BUSY` | it backs off and retries | `test_two_senders_at_once`; Step 6A |
| File larger than ALFP allows (512 fragments) | checked before sending, or the bridge replies `TOO_BIG` | `refused` | the job is dropped (it can never succeed) | `test_edge_sizes` |
| Bridge cannot store a file | SD error on the bridge | bridge replies `STORAGE (bridge SD)` | sender backs off and retries | 19 Nov bridge SD fix |
| Interrupted reception on the bridge | an incoming session gets no packets for 60 s (LoRa) or 15 s (WiFi) | the session is freed | the sender's retry starts again; leftover `.part` files are deleted at boot | `test_abandoned_session_expires`, `test_bridge_reboot_part_names` |
| Capture requests faster than comms can take them | capture queue (4 entries) full | the sensors task drops the request without waiting: `[ALERT] capture queue full, dropped`, `capture q full` counted | the next trigger after the 5 s cooldown is accepted | T6; Step 9 (`x`) |
| Outbox full (64 jobs) | adding a job to a full outbox | the oldest job is dropped (never the one being sent): `outbox: FULL ... dropped oldest job`, `outbox dropped (full)` counted | none for the dropped capture; the rest are sent | code review; never reached in testing (peak 19) |
| Log ring full, or nobody reading USB | log ring (64 lines) or USB transmit ring (4 KB) full | the writing task waits at most 500 ms for space, then the line is dropped and counted (`log dropped`, `usb dropped`) | normal output resumes | T8; Step 9 (`z`) |
| Audio samples lost | the DMA ring (16 KB, about 0.5 s) overran before the task read it | lost samples counted and printed with the capture | the capture is still sent | 0 lost in 54 captures, 19 Nov |
| DHT22 read fails | checksum or timing error | `dht22 fail` counted; the last good reading is used for up to 10 s, then the reading is reported as unavailable | next read 2.1 s later | 0 failures in 1 730 and 1 617 reads, 19 Nov |
| Microphone disconnected | **not detected** | every capture classifies as ambient | fix the wiring | 25 Nov, node 5 fault #5 |

## On the laptop

| Fault | Detection | Response | Recovery | Evidence |
| --- | --- | --- | --- | --- |
| Bridge USB unplugged or port busy | serial read fails | the dashboard shows `Bridge disconnected`; the bridge page lists the time | reconnects automatically every few seconds | `backend/live_bridge.py` |
| File changed between bridge and laptop | the backend recomputes the CRC-32 of every file | `crc_laptop_ok = 0`, shown in red on the capture | none; the bridge's own copy on its SD card is unaffected | `backend/tests`, the dashboard pipeline check |
| A transfer stops reporting | no `@EVT` line for 180 s | marked `failed / timeout` on the dashboard | a retry appears as a new transfer | `backend/tests/test_events.py` |
| Simulated data mixed with real | every simulated row carries `is_demo = 1` | shown with a SIMULATED badge and never counted as real | not applicable | `backend/tests`, pipeline check |

## Queues, rings and limits

| Item | Size | When it is full |
| --- | --- | --- |
| Capture queue, sensors → comms (µT-Kernel message buffer) | 4 requests | new request dropped and counted; the sender never waits |
| Log ring, any task → `usb_owner` | 64 lines of up to 256 B | writer waits up to 500 ms, then the line is dropped and counted |
| USB transmit ring | 4 096 B | line dropped and counted |
| UART receive ring (ESP-01 → Pico) | 2 048 B, about 175 ms at 115 200 baud | byte dropped, frame fails CRC-8 |
| ESP-01 link queues | 8 frames each way | sender told busy; ALFP waits for `tx_busy()` to clear |
| WiFi mesh send / relay / receive queues | 8 packets each | new packet refused; ALFP retries |
| LoRa mesh send / relay / receive queues | 8 packets each | same |
| Duplicate caches | WiFi 1 024 entries, LoRa 128, both 30 s | oldest entry reused |
| Hop limit | 4, both meshes | packet not relayed further |
| Outbox on SD | 64 jobs | oldest job dropped and logged |
| Bridge receive sessions | 4 senders at once | fifth sender refused with `BUSY` |
| ALFP file size | 512 fragments | refused with `TOO_BIG` |
| Audio DMA ring | 16 KB, about 0.5 s at 16 kHz | samples counted as lost |
| Open SD files | 8 | open fails, reported as an SD error |
| Task stacks | comms 8 192 B, usb_owner 3 072 B, sensors 8 192 B | least free seen: 5 040, 2 192 and 7 392 B (19 Nov, 60 min) |

## Known gaps

- A LoRa module that stops receiving after start-up is not detected; the node relies on the other
  radio until it is rebooted. A health check that re-initialises the radio when no packet has been
  heard for 30 s would close this.
- A disconnected microphone is not detected; captures are classified as ambient.
- Concurrent LoRa transfers share one channel with no listen-before-talk; they slow each other down but
  ALFP still completes them (25 Nov: up to three nodes sending at once).
