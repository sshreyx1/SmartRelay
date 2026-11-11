# Steps that need your hands

Written 2026-11-11. Do them in order. Each step is one action and says what you should see.
Everything else in `docs/curveball/STATUS.md` was done without touching the boards.

Boards right now:
- **Node 1 (bridge):** dual-core (SMP) firmware, USB port `/dev/cu.usbmodem3`, SD card not yet cleared.
- **Node 2:** dual-core (SMP) firmware, USB port `/dev/cu.usbmodemMTK3PICO1`, PIR sensor covered.
  Leave node 2 plugged in and covered the whole time.

---

## A. Back up and clear node 1's SD card (handoff section 5, branch 1)

Why: the bridge needs ~6 s to finish every received file, on both firmwares, and gets slower as
its card fills (`STATUS.md`, "A/B pair 2"). Clearing the received files should bring that under
1 s. You need a microSD card reader for the Mac.

Do **not** open the card in Finder or drag files with Finder: Finder writes hidden `._*` files onto
the card (that is how node 2's card got its `._*` files, follow-up F1). Use Terminal only.

1. Open Terminal and go to the repo:
   `cd ~/GitHub/SmartRelay`
   You see your prompt again, no output.
2. Unplug **node 1's** USB cable from the Mac (leave node 2 plugged in).
   Check: `ls /dev/cu.usbmodem*` now shows only `/dev/cu.usbmodemMTK3PICO1`.
3. Take the microSD card out of node 1's SD card module. Note which way up it was.
4. Before you plug in the reader, list the drives:
   `ls /Volumes`
   Write down what you see (for example `Macintosh HD`).
5. Put the card in the reader and plug the reader into the Mac. Wait 5 seconds.
6. List the drives again:
   `ls /Volumes`
   One new name appeared (often `NO NAME` or `UNTITLED`). That is the card.
7. Save its name in a variable so you can copy the commands below exactly. If the new name was
   `NO NAME`, type (keep the quotes):
   `CARD="/Volumes/NO NAME"`
   Check: `ls "$CARD"` lists folders such as `inbox`, `rx`, `meta`, `outbox`.
   If you do not see `inbox`, stop: this is not node 1's card.
8. Record how full it is (this is evidence for the report):
   ```sh
   { date; echo "inbox files: $(find "$CARD/inbox" -type f | wc -l)"; echo "rx files: $(find "$CARD/rx" -type f | wc -l)"; echo "all files: $(find "$CARD" -type f | wc -l)"; du -sh "$CARD"; } | tee docs/curveball/evidence/node1_sd_before_clear.txt
   ```
   You see a date and three counts. `inbox files` is expected to be large (hundreds to thousands).
9. Make a backup folder:
   `mkdir -p ~/SmartRelay-backups/node1-sd-2026-11-11`
   No output.
10. Copy the whole card into it (this can take several minutes with thousands of files):
    `cp -R "$CARD/" ~/SmartRelay-backups/node1-sd-2026-11-11/`
    No output when it finishes. Errors about `.Spotlight-V100` or `.fseventsd` are fine.
11. Check the copy has the same number of `inbox` files:
    `find ~/SmartRelay-backups/node1-sd-2026-11-11/inbox -type f | wc -l`
    The number must equal `inbox files` from step 8. **If it does not, stop here** and tell Claude.
12. Delete the received files (only `inbox/` and `rx/`, keep the folders themselves and everything else):
    ```sh
    find "$CARD/inbox" -type f -delete
    find "$CARD/rx" -type f -delete
    ```
    No output.
13. Check they are empty:
    `find "$CARD/inbox" "$CARD/rx" -type f | wc -l`
    You see `0`.
14. Eject the card:
    `diskutil eject "$CARD"`
    You see `Disk ... ejected`.
15. Take the card out of the reader and put it back into node 1's SD module, same way up as in step 3.
16. Plug node 1's USB cable back into the Mac.
    Check after ~10 s: `ls /dev/cu.usbmodem*` shows two ports again.
17. Tell Claude: "node 1's card is cleared and back in". Claude runs part B.

If anything looks different from what a step says, stop and paste what you see.

---

## B. Retest after the card is back (Claude runs this; commands here so you can run them yourself too)

Uses the same scripts and the same traffic as the A/B runs. `PY=~/.venvs/smartrelay/bin/python`.

1. Find the ports and check both are SMP (`task comms` in the reply):
   `ls /dev/cu.usbmodem*`
2. Same A/B traffic through the cleared SMP bridge (node 1 first, node 2 second):
   ```sh
   $PY docs/curveball/evidence/ab_pair.py --n1 /dev/cu.usbmodemNODE1 --n2 /dev/cu.usbmodemMTK3PICO1 --label smp_clean
   ```
   Pass (handoff section 5): telemetry **well under 1 s** (was ~6 s), images ~3 s (was ~8.6 s),
   0 `gave_up`, node 1 `alfp service` **< 1000 ms** (was 6439 ms).
3. Node 1 boot log (T2 for node 1, follow-up F7): reboot node 1 to BOOTSEL with `B` and capture:
   ```sh
   $PY -c "import serial,time; s=serial.Serial('/dev/cu.usbmodemNODE1',115200); s.write(b'B'); time.sleep(0.3)"
   sleep 3; $PY docs/curveball/evidence/boot_flash_capture.py uf2/csc09_smp_node_1.uf2 40 docs/curveball/evidence/t2_node1_smp_boot_clean.txt
   ```
   Pass: the new port appears **< 3 s** after the copy (was 5.99 s) and the log shows `dual core`,
   `sensors: core 1`, `comms: core 0`, `usb_owner: core 0`. If the port is still late, F7 is not the
   card and stays open.
4. Optional, for the record: one FreeRTOS run on the cleared card (1200-baud open of node 1's port,
   `cp -X uf2/csc09_node_1.uf2 /Volumes/RPI-RP2/`, `ab_pair.py ... --label freertos_clean`), then
   flash SMP back.

---

## C. Later, not needed now

- **Node 1 LoRa module** (handoff 6.2, blocks T7): `lora_radio: unexpected chip type 0x00` on both
  firmwares. Unplug node 1, check the Core1121 module is fully seated, measure 3V3 at the module,
  and check the wires for CS GP13, RESET GP5, BUSY GP6. Or swap in a spare module.
- **Node 2 PIR cover:** keep it on until the A/B and retests are done; remove it before any sensor
  test (T3/T4).
