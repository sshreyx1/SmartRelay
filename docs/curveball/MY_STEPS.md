# Steps that need your hands

Updated 2026-11-11 ~15:05. Do them in order. Each step is one action and says what you should see.

Boards right now:
- **Node 1 (bridge):** dual-core (SMP) firmware, USB port `/dev/cu.usbmodem3`, SD card cleared.
- **Node 2:** dual-core (SMP) firmware, USB port `/dev/cu.usbmodemMTK3PICO1`, PIR sensor covered.

## Done

- **A. Back up and clear node 1's SD card** - done by you. Backup in
  `~/SmartRelay-backups/node1-sd-2026-11-11` (692 inbox files). Keep it until the backend run has
  checked nothing in it is needed.
- **B. Retest** - done by Claude, see `STATUS.md` "Retest after node 1's SD card was cleared".

---

## C. Check or replace node 1's LoRa module (blocks T7, and is the lead for F7)

Why: node 1 reports `lora_radio: unexpected chip type 0x00` on both firmwares (hardware, not
firmware). Node 1's dual-core firmware also takes ~6 s to bring up USB, which loses its boot log
(F7). The dead LoRa module is the main suspect for that too, so fixing it may fix both.

You need: a multimeter, and a spare Core1121 module if you have one.

1. Unplug **node 1's** USB cable from the Mac (leave node 2 plugged in).
   Check: `ls /dev/cu.usbmodem*` shows only `/dev/cu.usbmodemMTK3PICO1`.
2. Look at the LoRa module on node 1. Press it gently down into its header/socket so every pin is
   fully seated.
3. Follow the wires from the module to the Pico and check each is firmly attached:
   - CS to GP13
   - RESET to GP5
   - BUSY to GP6
   - DIO9 (IRQ) to GP7
   - 3V3 and GND
4. Plug node 1's USB back in. Wait 10 seconds.
5. Measure the voltage between the module's 3V3 pin and GND with the multimeter.
   You should see about 3.3 V. If it reads 0 V or far below 3 V, the module has no power: fix that
   wire first.
6. If the wiring and 3.3 V are fine and you have a spare module: unplug node 1, swap the module,
   plug node 1 back in.
7. Tell Claude: "node 1's LoRa module is checked/swapped". Claude then runs part D.

## D. After C (Claude runs this; commands here so you can run them yourself)

`PY=~/.venvs/smartrelay/bin/python`

1. Boot log and LoRa status of node 1 (also the F7 retest):
   ```sh
   $PY -c "import serial,time; s=serial.Serial('/dev/cu.usbmodem3',115200); s.write(b'B'); time.sleep(0.3)"
   sleep 4; $PY docs/curveball/evidence/boot_flash_capture.py uf2/csc09_smp_node_1.uf2 40 docs/curveball/evidence/t2_node1_smp_boot_lora.txt
   ```
   Pass: `lora_radio: LR1121 firmware ...` instead of `unexpected chip type 0x00`. If the port now
   appears < 3 s and the log shows `sensors: core 1`, `comms: core 0`, `usb_owner: core 0`, T2 is
   done for node 1 and F7 was the LoRa module.
2. Then T7 (WiFi -> LoRa failover): that one needs you to unplug node 2's ESP-01 at a given moment;
   Claude will write the exact steps when we get there.

## E. Later

- **Node 2 PIR cover:** keep it on for any further bridge timing runs; remove it before sensor tests
  (T3/T4).
