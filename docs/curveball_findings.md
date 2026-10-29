# CURVEBALL findings (Steps 1 and 2)

Read-only survey before any code changed. Code wins on facts where this disagrees with the brief.

## Step 1 — repo vs. the brief

**Layout differs from the brief's guessed paths:**
- ALFP: top-level [`alfp/`](../alfp) (`alfp/src`, `alfp/include`), not `firmware/alfp`.
- Shared types: [`common/`](../common) (`capture_meta.h`, `uplink.h`, `loopback_radio.h`).
- Mesh abstraction: [`mesh/mesh_radio.h`](../mesh/mesh_radio.h) (a second, identical copy lives at `third_party/lora_mesh/mesh/mesh_radio.h` for that subtree's own build).
- LoRa: `third_party/lora_mesh/lora/{lora_radio.c,lora_mesh.c}`, full LR1121 driver tree under `third_party/lora_mesh/src/lr1121/`.
- WiFi bridge: `third_party/wifi_mesh/{wifi_mesh.c,wst_link.c}`. **There is no `wst_bridge.ino`** — no separate ESP8266 Arduino sketch in this repo to avoid porting.
- Sensors: `third_party/sensors/{pir,sound,mic,DHT22}.c` (unchanged team code) + `firmware/sensing/{sensing.c,audio_capture.c,audio_features.c}` (the glue).

**[`firmware/app/main.c`](../firmware/app/main.c) matches the brief closely:**
- 3 FreeRTOS tasks, priorities exactly as described: `comms` 3 / `sensors` 2 / `console` 1.
- `comms_task`: 2 ms `vTaskDelay` loop (not `vTaskDelayUntil` — comment at main.c:259 explains why: catching up after a slow iteration must not starve sensors).
- `sensors_task`: 10 ms poll loop, DHT22 priority boost already implemented exactly as brief 3.8/Gap 14 "default: keep" describes — `vTaskPrioritySet(NULL, 5)` / restore (main.c:276-284).
- `console_task` **already non-blocking** (`getchar_timeout_us(0)`, main.c:371) — not a blocking `getchar()`.
- `app_request_capture()` matches the brief exactly: 4-slot queue, `xQueueSend(..., 0)`, false on full.

**Shared volatile flags — more than the brief named** (main.c:55-56):
`g_want_stats`, `g_toggle_drop`, `g_force_capture`, `g_want_env`, `g_toggle_topo` (console → comms/sensors), plus `g_comms_ready` (comms → sensors, gates sensor startup until SD/radios are up).

**SPI1 ownership** is already documented today, not just assumed: [`board.h`](../firmware/app/board.h) and `main.c:189-190` state the single-owner rule in comments. No mutex exists; none is needed (matches brief 3.2).

**UART0 / WiFi bridge path**: the RX IRQ is installed in [`third_party/wifi_mesh/wst_link.c:178-180`](../third_party/wifi_mesh/wst_link.c), called from inside `radios_init()`, which `comms_task` calls at comms_task:201. **The IRQ is already enabled from the comms task today** — the brief's requirement 3.4 is already satisfied by the current single-core code, and stays true unchanged once comms_task is pinned to core 0.

**ADC ownership**: `adc_select_input`/`adc_read` appear only in `firmware/sensing/{sensing.c,audio_capture.c}` and `third_party/sensors/mic.c`, all driven from `sensors_task`. Confirmed sole-owner today.

**`printf`** appears in `main.c`, `freertos_hooks.c`, `radios.c`, `storage_fatfs.c`, `sensing.c`, and the sensor/self/photo-test mains — i.e. from both `comms_task` and `sensors_task` directly today, not funneled through one owner. This is exactly the condition 3.6 asks to fix.

## Step 2 — the µT-Kernel 3 SMP port (the blocking question)

**There is no official µT-Kernel 3 SMP port for RP2040.** `tron-forum/mtk3_bsp` (the real T-Engine Forum BSP) has a `pico_rp2040` branch but it is single-core, last released years ago. The only SMP variant found: **`TemariVirus/mtk3smp-rp2040`**, pinned at commit `900e863` (2026-10-26), a fork-of-a-fork (`sirfonzie` → `hollowheap` → `TemariVirus`) built from the official BSP, credited to an individual contributor "with AI Assistants." The user explicitly approved proceeding with this port despite the provenance risk (2 commits, 0 stars, longest documented soak test 10 s, mixed T-License 2.1/2.2 + Apache-2.0 + BSD-3-Clause headers — file header governs, see `LICENSE`/`LICENSES/` in the vendored copy).

Answers to brief Section 10 / Step 2, each verified against the actual source (not README prose), citations are file:line inside the vendored port at `third_party/mtk3smp_rp2040/`:

1. **Core pinning**: supported. `T_CTSK.tskatr |= TA_ASSPRC`, `T_CTSK.assprc = TP_PRC1` (processor 1 = **core 0**) or `TP_PRC2` (processor 2 = **core 1**) — `include/tk/syscall.h:43-57`. Numbering is 1-based per the SMP T-Kernel convention (header comment, same lines).
2. **Tick**: `CNF_TIMER_PERIOD` default 10 ms, `MIN_TIMER_PERIOD 1`, `MAX_TIMER_PERIOD 50` (`config/config.h:41`) — **1 ms is supported**. Core 0 is the enforced (not just documented) sole timekeeper: `kernel/sysdepend/cpu/rp2040/smp_rp2040.c:383-386` has core 1 explicitly disable its local SysTick on entry.
3. **ISR-safe calls**: **none**. No `i`-prefixed µITRON-style ISR variants (`iset_flg`, etc.) exist anywhere in this port's headers. The port's own UART driver (`device/ser/sysdepend/rp2040/ser_rp2040.c:84-123`) never calls a `tk_*` IPC primitive from interrupt context — the ISR only touches a ring buffer under `ISpinLock`/`ISpinUnlock` (from `tk/smp.h`), and a task later drains the ring and raises flags/sends messages from task context. **This is the pattern 3.4/6.2 must follow**: the UART0 RX ISR fills the ring under a spinlock only; `tk_set_flg`/`tk_snd_mbf` are called only from `comms_task`, never from the ISR.
4. **Task create/start**: `tk_cre_tsk(T_CTSK*)` returns an `ID`, and the task starts **DORMANT** — `tk_sta_tsk(ID, INT stacd)` is a mandatory second call to actually run it (`include/tk/syscall.h:193-210,734`; real usage pattern at `app_program/app_main.c:514-519`).
5. **Priority**: `MIN_TSKPRI=1`, `MAX_TSKPRI=32` (`include/sys/knldef.h:58-61`, `config/config.h:39`) — confirmed smaller number = higher priority, matching the brief's assumption.
6. **Event flags / message buffer**: `tk_cre_flg/tk_set_flg/tk_clr_flg/tk_wai_flg(id,waiptn,wfmode,&ptn,tmout)`; `tk_cre_mbf/tk_snd_mbf(id,msg,sz,tmout)/tk_rcv_mbf(id,msg,tmout)`. `TWF_ANDW=0`, `TWF_ORW=1`, `TMO_POL=0`, `TMO_FEVR=-1`. Non-blocking send is `tk_snd_mbf(..., TMO_POL)`; on a full buffer it returns **`E_TMOUT`** (`include/tk/errno.h:58`), not `E_QOVR` — the capture-queue-full check must test for `E_TMOUT`.
7. **Stack units**: bytes (`SZ stksz` field, commented `/* User stack size (byte) */`, `include/tk/syscall.h:198`). **Important deviation from the brief's plan**: the port's own demo (`app_program/app_main.c:68-72`) warns that a straight ×4 words→bytes conversion from the old FreeRTOS sizes is not necessarily enough under SMP — "the SMP kernel path is deeper... 1-2 KiB is no longer enough for a task that makes blocking kernel calls" — and uses 4096 bytes for every task regardless of its old FreeRTOS size. Applied here: `comms` 2048 words×4=8192 B (already comfortably above 4096), `sensors` 1024×4=4096 B (at the port's own stated minimum, no margin), `console/USB-owner` 512×4=2048 B (**below** the port's recommended floor). Stacks were bumped where this put us below the 4096 B heuristic — see PR.
8. **Build system**: **GNU Make, not CMake** (`build_make/pico_rp2040.mk`, `build_make/makefile`). It is a standalone, monolithic build — it compiles Pico-SDK sources (TinyUSB, cyw43 driver, lwIP) directly via raw `-I` paths, never invokes `pico_sdk_init()`, and produces one `.elf`/`.uf2` with no object-library seam for an external CMake build to link against. There is no way to "add `APP_USE_TWO_CORES` as a CMake option" toggling one unified build as the brief's Step 5 imagined — **the two kernels use two different build systems** and this PR documents that as a deliberate, bounded adaptation rather than inventing a nonexistent bridge. The port does provide one documented customization seam that makes a clean integration possible: `build_make/makefile:25` does `-include sources.mk` before anything else, and `build_make/makefile:56` does `-include mtkernel_3/app_program/subdir.mk` — this repo's build glue (`firmware_smp/buildglue/`) targets exactly those two hooks rather than touching vendored kernel files.
9. **SMP=0/1 is already the kernel's own single-core fallback flag** (`build_make/pico_rp2040.mk:1-25`): "SMP=0 ... the generated code is equivalent to the pre-SMP port." This maps directly onto the brief's 3.1 "`APP_USE_TWO_CORES` build option, not a rewrite" requirement — the same application source compiles under either `SMP=0` (single core, `TA_ASSPRC` requests silently dropped, exactly the documented demo pattern at `app_program/app_main.c:35-46,426-443`) or `SMP=1` (pinned dual-core), with no app-code fork needed.
10. **Reference pattern**: `app_program/app_main.c` is a complete, real 4-task pipeline (producer pinned `TP_PRC1`, consumer pinned `TP_PRC2`, mutex, message buffer, semaphore, event flag, `usermain()` wiring) — used as the template for this port's task/object setup.

**Section 10 answers:**
1. Pinning supported (see above); not needed as a fallback question — SMP=0 is the kernel's own single-core fallback and needs no app-code change.
2. Tick can be 1 ms; core 0 services it exclusively (enforced in kernel source, not just configuration).
3. No kernel call is ISR-safe in this port; the ISR-only-touches-a-lock-protected-ring pattern is used instead (see item 3 above).
4. `printf` is called directly from both `comms_task` and `sensors_task` today (see Step 1); this PR routes both through the new log ring / USB-owner task.
5. Yes — three more flags than the brief named (`g_force_capture`, `g_want_env`, `g_toggle_topo`), plus `g_comms_ready`.
6. Host-side parsing: see PR description for the exact stats-line fields kept unchanged and the new fields appended.
7. `selftest_main.c` is bare-metal (no FreeRTOS calls at all) and is left unported — it already runs identically on either kernel since it never calls one.
8. FreeRTOS is fetched at configure time via `firmware/FreeRTOS_Kernel_import.cmake` (env var `FREERTOS_KERNEL_PATH`), not vendored in this repo — the existing CMake/FreeRTOS build is left completely untouched by this PR and remains the single-core build.

## Toolchain note

This environment has no `arm-none-eabi-gcc`, `cmake`, or `make` installed, and no `PICO_SDK_PATH`/`FREERTOS_KERNEL_PATH`/`FATFS_PICO_PATH` configured. **Nothing in this PR has been compiled or flashed.** All kernel API usage below is written against the citations above, not guessed, but it has not been build-verified. T1-T8 (Section 8 of the brief) require physical hardware (two Picos, LoRa modules, SD cards, the sensor set, a WiFi bridge) this session does not have access to — all are reported "not run, no hardware/toolchain access" in the PR, not "pass."
