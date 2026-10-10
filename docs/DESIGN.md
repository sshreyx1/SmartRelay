# ALFP design notes (Member 2: Fragmentation and microSD)

The full, illustrated version is the shared design doc. This file is the offline copy that
travels with the code.

## What it does

A field node's capture (a sample image on its microSD card, standing in for a camera) is sent to
the bridge node (node 1) over one mesh radio, chosen once per transfer. The image is split into
fragments that fit the radio (241 bytes of image per LoRa packet, 236 per WiFi packet), every
fragment carries a sequence number and a CRC-16, the bridge writes each fragment straight to its SD
card and tracks which have arrived, and the sender resends only the missing ones. The whole file is
then checked with CRC-32 and handed to the laptop over USB.

## Protocol: sender-polled selective repeat

1. Sender computes the file's CRC-32 from the SD card (512 bytes per step, so the radio keeps being serviced).
2. MANIFEST: size, fragment count and size, CRC-32, radio, 27-byte capture metadata.
3. DATA 0 .. n-1, paced (LoRa 500 ms after the radio goes idle; WiFi 10 ms).
4. POLL(round). Bridge answers with a NACK listing exactly the missing fragments (list or bitmap,
   whichever is shorter), or with ACK OK if everything is in and the CRC-32 matches.
5. Sender resends only those fragments (each twice if 4 or fewer remain) and polls again.
6. The bridge also sends ACK OK the moment the last fragment arrives and verifies.

Failure handling:

| Lost | What happens |
| --- | --- |
| MANIFEST | Bridge opens the session from DATA, NACK sets "manifest needed" |
| DATA | Listed in the next NACK |
| POLL or NACK | Sender polls again after 1.5 s (LoRa) / 0.2 s (WiFi) |
| ACK | Bridge remembers its last 8 finished transfers and ACKs again; no duplicate file |
| Bits corrupted after the radio CRC | CRC-16 rejects the packet; it is re-requested |
| File corrupted on the bridge SD | CRC-32 mismatch, ACK CRC_FAIL, whole file resent once |
| Bridge unreachable | Sender gives up after ~45 s (LoRa) / ~8 s (WiFi) of silence; job stays in `outbox/`, retried with 5-60 s backoff, oldest first |
| Sender vanishes | Bridge deletes the partial file after 60 s (LoRa) / 15 s (WiFi) idle |

## Packet formats (little-endian, CRC-16/CCITT-FALSE trailer on every packet)

| Type | Layout | Size |
| --- | --- | --- |
| MANIFEST 1 | b0, sid2, size4, count2, fsize2, crc32 4, radio1, metalen1, meta, crc2 | 19 + meta |
| DATA 2 | b0, sid2, seq2, count2, payload, crc2 | payload + 9 |
| POLL 3 | b0, sid2, round1, crc2 | 6 |
| NACK 4 | b0, sid2, flags1, round1, fmt1, body, crc2 (fmt: ALL, LIST n+seq2..., BITMAP base2 nbits2 bits, NONE) | 8 to ~80 |
| ACK 5 | b0, sid2, status1, crc2 (OK, CRC_FAIL, BUSY, STORAGE, TOO_BIG) | 6 |

b0 = version (high nibble, 1) | type (low nibble).

## Decisions and the evidence for them

| Decision | Evidence |
| --- | --- |
| Selective repeat, not resend-all | 63% less LoRa airtime at 10% loss per hop (45 s vs 124 s), 71% at 20% (docs/results/e4) |
| Selective repeat, not FEC (LoRaWAN TS004 style) | FEC suits one-way broadcast; we have a return path, so only real losses cost airtime |
| Sender polls; bridge never runs NACK timers | One owner of timing; no duplicate NACKs; lost NACK fixed by polling again |
| Fast polls + silence budget (not "5 tries") | First design completed 45% of transfers at 20% loss; final 100% (e2) |
| LoRa gap 500 ms | 2 hops: 4.0 transmissions per fragment with no gap, 1.8 at 500 ms. 700 ms is worse than both 500 and 1000. 1000 ms saves ~14% airtime at 3-4 hops (e1) |
| Adaptive pacing off | Built and measured: random loss makes it slow down for nothing (88 s vs 65 s median) |
| Tail duplication (<= 4 missing) | About one round fewer at the end of a transfer |
| CRC-16 per packet + CRC-32 per file | CRC-16/CCITT keeps Hamming distance 4 up to 32,751 bits (Koopman): catches all 1-3 bit errors in a 2,000-bit fragment; tested on every 1- and many 2-bit flips |
| Stream through SD, never buffer the image | ALFP state is 2.4 kB RAM total |
| One task owns radios + SD | FatFs built non-reentrant; mesh code needs no locks |
| Radio chosen once per attempt | WiFi if bridge heartbeat heard on WiFi within 10 s, else LoRa, else wait on SD |

## Measured performance (simulator, 8 kB image, 2 hops, 10% loss per hop)

| | LoRa | WiFi |
| --- | --- | --- |
| Median transfer time | 67 s | 2.3 s |
| Transmissions per fragment | 1.84 | 1.52 |
| First-attempt success up to 30% loss/hop (1-2 hops) | 100% | 100% |

Recommendation for the LoRa mesh: channel activity detection (listen-before-talk) before each
transmission makes transfers 18% faster (e3). The bridge's 3 s heartbeat is the main source of
collisions at 1 hop without it.

## Memory (arm-none-eabi, Pico W)

| | Size |
| --- | --- |
| ALFP context | 2,452 B RAM |
| ALFP code | ~9 kB flash (+1.5 kB CRC tables) |
| Whole node firmware, all members' code | 178 kB flash, 110 kB static RAM of 264 kB (48 kB FreeRTOS heap, 16 kB audio DMA ring) |

## Integration with the team's code (8 Oct)

- One image per node (`csc09_node_1..5.uf2`) with Mridula's LoRa mesh, Goutham's WiFi mesh,
  Maryam's PIR/DHT22/weather tree and Rhaylene's sound trigger and classifier. Their sources are
  compiled as delivered; only Mridula's `gpio.h` pin defines were made overridable.
- Pins (firmware/app/board.h): the microSD card uses the Maker Pi Pico slot, sharing SPI1 with the
  LoRa module (separate chip selects GP15 and GP13). DIO9 moved GP15 to GP7 and BUSY GP14 to GP6.
  The single comms task owns both SPI devices, mounts the SD card before LoRa starts, and runs the
  bus at 1 MHz.
- Trigger: PIR and sound trigger high together (their rule), 5 s cooldown. Each capture records
  5 s of audio and sends energy-based class, RMS and zero crossings per second in the capture
  record together with temperature, humidity, light and weather label.
- Audio: Rhaylene's 160 kB record buffer does not fit, so the ADC free-runs at 16 kHz into a 16 kB
  DMA ring and the same features are computed while streaming. Identical to her
  `mic_analyze_buffer()` in 30 test cases; on the RP2040 emulator 16000 samples per second, none lost.
- DHT22: her bit-banged read needs ~5 ms without interruption; the sensors task is raised above
  the comms task for exactly that window.
- Verified on the laptop: Goutham's real `wifi_mesh.c` + ALFP in three processes, E to C to A
  through his topology filter, byte-for-byte identical up to 35% loss per hop.
