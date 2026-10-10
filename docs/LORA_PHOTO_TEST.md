# Sending photos between two Picos over LoRa

Pico **A** (node 1) receives, Pico **B** (node 2) sends. Both run Mridula's LoRa mesh unchanged and
the ALFP fragmentation layer. WiFi is not used.

## 1. Wiring (both Picos identical, same as the full node: docs/BRINGUP.md)

**LoRa Core1121**:

| Core1121 | Pico |
| --- | --- |
| RESET | GP5 |
| SCK | GP10 |
| MOSI | GP11 |
| MISO | GP12 |
| CS (NSS) | GP13 |
| BUSY | GP6 |
| IRQ (DIO9) | **GP7** (not GP15: GP15 is the SD slot's chip select) |
| 3V3, GND | 3V3 (pin 36), GND |

Fit the antenna before powering on.

**microSD:** in the Maker Pi Pico's own slot. It shares GP10/11/12 with the LoRa module and has its
own chip select (GP15), so nothing else to wire.

## 2. Prepare the SD cards

- Both cards: FAT32.
- **Sender (B):** copy the photos to the card's top folder or a `photos/` folder. Each must be under
  **120 kB**. Phone photos are megabytes, so shrink them first:
  `python3 tools/make_sample_images.py my_photos/*.jpg --out sd_card/photos --width 320 --height 240`
  (320x240 is about 10-15 kB, about 40 s over LoRa; 160x120 is about 4 kB, about 15 s).
- **Receiver (A):** an empty card is fine. Received photos go into `inbox/`.

## 3. Flash

Hold BOOTSEL, plug in, drag the file onto the RPI-RP2 drive:

- Pico A: `lora_photo_rx.uf2`
- Pico B: `lora_photo_tx.uf2`

(Build them yourself with `cmake -S firmware -B build-fw && cmake --build build-fw -j4`, see docs/BRINGUP.md.)

## 4. Run

Plug both Picos into the laptop. Find their ports (Windows: Device Manager > Ports, e.g. COM5
and COM6; Mac/Linux: `ls /dev/tty.usbmodem*` or `/dev/ttyACM*`).

**Receiver (A)**, in terminal 1. This shows A's log and saves every photo it receives on the laptop:

```bash
pip install pyserial
python3 tools/bridge_receiver.py --port COM5
```

**Sender (B)**, in terminal 2 (or the VS Code Serial Monitor at 115200):

```bash
python3 -m serial.tools.miniterm COM6 115200
```

B lists the photos it found on its card. Press **1** to send the first one (**a** sends all of them).

## 5. Check it really arrived intact

1. **CRC match.** B prints `DONE ... CRC-32 1a2b3c4d`, and A prints `RECEIVED ... CRC-32 1a2b3c4d verified`.
   The two numbers must be identical. A only reports "verified" after it has read the whole file back
   from its SD card and the CRC-32 matched the one B computed before sending.
2. **Open it.** `bridge_receiver.py` saves `received/n2_00042_<original name>.jpg` on the laptop
   (CRC checked again) plus a `.json` with the transfer details. Open the image.
3. **Byte compare (optional).** Put A's SD card in the laptop and compare `inbox/n2_..._<name>` with the original:
   Windows `fc /b original.jpg received.jpg`, Mac/Linux `cmp original.jpg received.jpg` (no output = identical).

## 6. Show the repair working

Press **d** on A to drop 30% of the packets it hears, then send again from B. B prints rounds above 1
and a count of resends, A shows its fragment count climbing, and the CRC still matches. Press **s** on
either for statistics, including RSSI and SNR of the last packet.

## Keys

| Sender (B) | Receiver (A) |
| --- | --- |
| `l` list photos, `1`-`9` send one, `a` send all | `l` list received files |
| `s` stats, `d` toggle 30% loss | `s` stats, `d` toggle 30% loss |
| `g` gap 150 / 500 ms (use 500 with a relay node in between) | |

## If something is wrong

| You see | Likely cause |
| --- | --- |
| `sd: mount failed` | Card not FAT32 or not fully in the slot, or the DIO9 wire is still on GP15 |
| `LoRa init failed` | Core1121 wiring or power: BUSY must be on GP6 and DIO9 on GP7 |
| `none found` on B | Photos not in the top folder or `photos/`, or not .jpg/.jpeg/.png/.bmp/.gif |
| `TOO BIG` | Over 120 kB, shrink it (step 2) |
| B: `FAILED ... No answer from the receiver` | A not running `lora_photo_rx`, too far apart, or antennas missing |
| B: `s` shows `node 1 not heard yet` | A is not transmitting its 3 s heartbeat: check A's serial log |
| A: fragments stop increasing | Packet loss or range: B keeps polling and resends what is missing |

Tested before handing over: Mridula's real `lora_mesh.c` and this code ran as two laptop processes
over a simulated radio (`host/integration/run_integration.sh`): 5 kB and 20 kB files at 0%, 10%,
20% and 35% loss all arrived byte-for-byte identical. The real radio link itself is still to be tested.
