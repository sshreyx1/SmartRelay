# Commit queue

Run these in order, from the repo root on branch `curveball-integration`. Nothing here has been
committed by Claude. Each block stages only its own files. The previous queue (2 commits) was
committed on 2026-11-11.

## 1. Node 1 boot log after the LoRa module swap (T2 node 1)

```sh
cd ~/GitHub/SmartRelay
git add docs/curveball/evidence/boot_flash_capture.py docs/curveball/evidence/t2_node1_smp_boot_lora.txt
git commit -m "Capture node 1's SMP boot log and show T2 on node 1

pyserial's open() raises DTR and then flushes the input buffer, which
deleted the start-up lines the firmware sends as soon as DTR rises.
boot_flash_capture.py now opens the port with os.open and TCSANOW.
With the swapped LoRa module, node 1's port comes up 1.04 s after boot
and the log shows sensors on core 1, comms and usb_owner on core 0.
LoRa is still down: unexpected chip type 0x00."
```

## 2. flash_node.py on macOS

```sh
git add firmware_smp/buildglue/flash_node.py
git commit -m "Find the RPI-RP2 drive on macOS in flash_node.py

find_drive() only knew Windows drive letters. On macOS it now uses
/Volumes/RPI-RP2 and waits up to 3 s for the volume to finish mounting,
and the copy is data only with one retry on a permission error. The
Windows path is unchanged."
```

## 3. Docs: LoRa still down after the swap, T2 result, hands-on steps

```sh
git add docs/SMP_HANDOFF.md docs/curveball_results.md docs/curveball/STATUS.md docs/curveball/MY_STEPS.md docs/curveball/COMMIT_QUEUE.md
git commit -m "Record node 1 LoRa still down after module swap, T2 passed

Node 1's LoRa fails the same way with a second module, so the fault is
most likely node 1's board or wiring. Update handoff 6.2 and the C5
results, resolve F7, add F10 (pyserial flushes the boot burst), and add
the steps to test node 2's working module on node 1."
```

## Check

```sh
git status --short   # expect nothing left
```
