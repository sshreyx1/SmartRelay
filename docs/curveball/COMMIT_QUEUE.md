# Commit queue

Run these in order, from the repo root on branch `curveball-integration`. Nothing here has been
committed by Claude. Each block stages only its own files. Your own `3b_results.txt` and
`3c_results.txt` in the repo root are left out on purpose.

This replaces the T6 commit block given earlier in the chat (it was never run).

## 1. Test helpers

```sh
cd ~/GitHub/SmartRelay
git add docs/curveball/evidence/key_run.py docs/curveball/evidence/boot_flash_capture.py docs/curveball/evidence/ab_pair.py
git commit -m "Add console and boot-capture helpers for the SMP hardware tests

key_run.py sends keys to one node on a schedule and summarises every
stats block. boot_flash_capture.py flashes a board in BOOTSEL and logs
its boot output, recording when the new USB port appeared. ab_pair.py
now writes its raw logs as .txt so they are not git-ignored.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

## 2. Bridge A/B pair 2 (reverse order)

```sh
git add docs/curveball/evidence/ab_smp_r2_results.txt docs/curveball/evidence/ab_smp_r2_n1_log.txt docs/curveball/evidence/ab_smp_r2_n2_log.txt \
        docs/curveball/evidence/ab_freertos_r2_results.txt docs/curveball/evidence/ab_freertos_r2_n1_log.txt docs/curveball/evidence/ab_freertos_r2_n2_log.txt
git commit -m "Record a second bridge A/B pair in reverse order

SMP then FreeRTOS, same six forced captures. Over all four runs the SMP
bridge is about 0.2 s slower than FreeRTOS on a roughly 6 s receive
finish, so the delay comes from node 1's SD card, not the port.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

## 3. T6 evidence

```sh
git add docs/curveball/evidence/t6_node2_smp_results.txt docs/curveball/evidence/t6_node2_smp_log.txt \
        docs/curveball/evidence/t6_node2_smp_r2_results.txt docs/curveball/evidence/t6_node2_smp_r2_log.txt
git commit -m "Record T6 (capture queue full) passing twice on SMP node 2

Five fast capture requests: four queued, one refused, and the
queue-full counter went up by exactly one in each run.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

## 4. T8 evidence

```sh
git add docs/curveball/evidence/t8_node2_smp_results.txt docs/curveball/evidence/t8_node2_smp_log.txt
git commit -m "Record T8 (console stall) passing on SMP node 2

A 5 s console stall with eight stats requests and a transfer in flight
dropped 3 log lines, then output resumed, both transfers completed and
the longest comms loop did not grow.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

## 5. Node 1 boot timing (T2) and node 2 control

```sh
git add docs/curveball/evidence/t2_node1_smp_boot.txt docs/curveball/evidence/t2_node2_smp_boot_control.txt
git commit -m "Record why node 1's SMP boot log is lost

After a reflash, node 1's USB port appears 5.99 s after boot, past the
firmware's 3 s hold, so no boot lines reach the host. Node 2 with the
same procedure shows its port in 0.96 s and its full boot log.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

## 6. Status, hands-on steps and this queue

```sh
git add docs/curveball/STATUS.md docs/curveball/MY_STEPS.md docs/curveball/COMMIT_QUEUE.md
git commit -m "Update SMP verification status after the autonomous test run

Add the four-run A/B analysis, T6/T8 results, node 1's boot timing,
follow-ups F7 and F8, the board state, and the hands-on steps for
clearing node 1's SD card and the retest that follows.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

## Check

```sh
git status --short   # expect only 3b_results.txt and 3c_results.txt left
```
