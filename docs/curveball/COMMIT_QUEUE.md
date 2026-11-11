# Commit queue

Run these in order, from the repo root on branch `curveball-integration`. Nothing here has been
committed by Claude. Each block stages only its own files. The previous queue (6 commits) was
committed on 2026-11-11.

## 1. Retest evidence after clearing node 1's SD card

```sh
cd ~/GitHub/SmartRelay
git add docs/curveball/evidence/node1_sd_before_clear.txt \
        docs/curveball/evidence/ab_smp_clean_results.txt docs/curveball/evidence/ab_smp_clean_n1_log.txt docs/curveball/evidence/ab_smp_clean_n2_log.txt \
        docs/curveball/evidence/ab_freertos_clean_results.txt docs/curveball/evidence/ab_freertos_clean_n1_log.txt docs/curveball/evidence/ab_freertos_clean_n2_log.txt \
        docs/curveball/evidence/t2_node1_smp_boot_clean.txt
git commit -m "Record bridge retest after clearing node 1's SD card

The card held 692 inbox files. After clearing it, telemetry finishes in
2.5 s instead of 6.1 s and images in 5.0 s instead of 8.7 s, the same on
both firmwares. Node 1's USB port still appears about 6 s after boot."
```

## 2. Status, follow-ups and hands-on steps

```sh
git add docs/curveball/STATUS.md docs/curveball/MY_STEPS.md docs/curveball/COMMIT_QUEUE.md
git commit -m "Update SMP status after the SD card retest

Clearing the card removed about 3.6 s per received file but left about
2.5 s that both firmwares share. Rule out the card as the cause of node
1's late USB port (F7), add follow-up F9 (orphan .part files are never
deleted), and replace the card steps with the LoRa module check."
```

## Check

```sh
git status --short   # expect nothing left
```
