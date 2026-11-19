# Steps that need your hands

Updated 2026-11-11 ~18:55. Do them in order. Each step is one action and says what you should see.

Boards right now:
- **Node 1 (bridge):** dual-core (SMP) firmware, plugged in. LoRa still DOWN after its module swap.
- **Node 2:** dual-core (SMP) firmware, **not seen on USB** in the last run (unplugged?). Its LoRa
  module works.
- The USB port name depends on which board is plugged in (both use the serial `MTK3PICO`): with only
  node 1 plugged in, node 1 is `/dev/cu.usbmodemMTK3PICO1`. Claude identifies boards by the `node N`
  line in `s`, not by the port name.

## Done

- **A. Back up and clear node 1's SD card** - done by you. Backup in
  `~/SmartRelay-backups/node1-sd-2026-11-11` (692 inbox files). Keep it until the backend run has
  checked nothing in it is needed.
- **B. Retest** - done by Claude, see `STATUS.md` "Retest after node 1's SD card was cleared".
- **Old C. Check/swap node 1's LoRa module** - done by you. LoRa is still down
  (`unexpected chip type 0x00`), but node 1's boot log now comes through, and T2 passed on node 1.

---

## C. Decisive test: put node 2's WORKING LoRa module on node 1

Why: node 1 has now failed with two different modules. Node 2's module is known to work
(`lora_radio: LR1121 firmware 0x0101, hardware 0x22`). If node 2's module also fails on node 1,
the fault is node 1's board or wiring, not the module. If it works, both modules tried on node 1
were bad.

You need: both boards, both LoRa modules, about 15 minutes. Take your time; nothing here is urgent.

Safety first:
- Always unplug a board's USB cable before touching its LoRa module. Never move a module while the
  board has power.
- Each module has an antenna. Move the antenna together with its module, and never power a board
  whose LoRa module has no antenna fitted (it can damage the radio if it transmits).
- Touch a metal tap or radiator before handling the modules, to discharge static.

Steps:
1. Unplug **both** boards' USB cables from the Mac.
2. Take a photo of each board from above, so you can see how each module and its wires sit.
   You will use these photos to put everything back.
3. Put a small piece of tape on node 1's current module and write "N1" on it. Do the same for
   node 2's module with "N2". This keeps them from getting mixed up.
4. Gently lift node 1's module (N1) off node 1. Lift straight up and keep the pins straight. Put it
   aside with its antenna.
5. Gently lift node 2's module (N2) off node 2, the same way.
6. Fit module **N2** onto **node 1**, in the same position and orientation that N1 had (check the
   photo). Make sure its antenna is attached.
   Check: every pin is fully pushed in, and no pin sits outside the socket.
7. Plug in **only node 1** (leave node 2 unplugged).
   Check: `ls /dev/cu.usbmodem*` shows one port.
8. Tell Claude: "N2 module is on node 1". Claude runs D1 and tells you the result:
   - `lora_radio: LR1121 firmware ...` = the module works on node 1, so both modules tried there were bad.
   - `unexpected chip type 0x00` again = **node 1's board or wiring is the fault**. Next would be
     checking node 1's wires with a multimeter (Claude will write those steps).

Put everything back (do this whatever the result):
9. Unplug node 1's USB cable.
10. Lift module N2 off node 1 and fit it back onto **node 2**, as in node 2's photo, with its antenna.
11. Fit module N1 back onto **node 1**, as in node 1's photo, with its antenna.
12. Plug in **both** boards. Make sure node 2's PIR cover is still on.
    Check: `ls /dev/cu.usbmodem*` shows two ports.
13. Tell Claude: "modules are back". Claude runs D2 to confirm node 2's LoRa still works.

## D. After C (Claude runs this; commands here so you can run them yourself)

`PY=~/.venvs/smartrelay/bin/python` (your venv needs `pyserial`)

1. With only node 1 plugged in (step 8): boot log and LoRa status of node 1.
   ```sh
   $PY -c "import serial,time; s=serial.Serial('/dev/cu.usbmodemMTK3PICO1',115200); s.write(b'B'); time.sleep(0.3)"
   sleep 4; $PY docs/curveball/evidence/boot_flash_capture.py uf2/csc09_smp_node_1.uf2 40 docs/curveball/evidence/node1_lora_with_n2_module.txt
   grep lora docs/curveball/evidence/node1_lora_with_n2_module.txt
   ```
2. With both plugged in (step 13): send `s` to each port and check that node 2's stats show `lora up`
   (and node 1's `lora DOWN`, as before). Then T7 (WiFi -> LoRa failover) only once node 1's LoRa
   works; Claude will write those steps then.

## E. Later

- **Node 2 PIR cover:** keep it on for any further bridge timing runs; remove it before sensor tests
  (T3/T4).
