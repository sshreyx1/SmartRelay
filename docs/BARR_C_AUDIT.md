# BARR-C:2018 compliance audit (2026-11-25)

Checked against the actual BARR-C:2018 PDF (Barr Group, ISBN 978-1-72112-798-6), rule by rule.
This supersedes the "best effort, without the PDF" caveat in `docs/CODING_STANDARD.md`.

## Verdict

**The code base does not conform to BARR-C:2018.** It conforms to the subset of rules that
`docs/CODING_STANDARD.md` lists (braces, tabs, indentation, `goto`, fixed-width types), but that
table covers about 10 of the standard's ~150 lettered rules. Several mandatory ("shall") rules are
broken in almost every file: naming prefixes (7.1.k `p_`, 7.1.e 3-character minimum), the
end-of-file comment (3.3.c), the function-definition space (3.1.j), and constant-on-the-left
comparisons (8.6.a).

The standard's own *Deviation Procedure* (p. 6) says all code "shall conform to all of the rules
herein, except if its specific deviations have been documented and approved". It allows changing
*quantities* (line width, function length) at the project level. Module-level deviations need the
project manager's approval, and the approver's name and reasons must be written next to the deviation.
The current 120-column deviation is the allowed kind, but it does not name an approver.

## Scope

116 team-written files (`git ls-files '*.c' '*.h'`), 16.5k lines: `alfp/`, `common/`, `mesh/`,
`firmware/`, `firmware_smp/`, `host/`, and the team-member code in `third_party/wifi_mesh/`,
`third_party/lora_mesh/` (except `src/lr1121/`) and `third_party/sensors/`.

Excluded as vendored: `third_party/mtk3smp_rp2040/` (kernel submodule) and
`third_party/lora_mesh/src/lr1121/` (Semtech driver). `firmware/sensing/sound_model.h` is generated
by `ml/export_c.py`, so fix it in the generator, not by hand.

Method: `tools/barrc_scan.py` (regex-based, so counts are approximate; samples of each rule were
checked by hand), plus a manual read for rules a regex cannot judge. Re-run it with:

```sh
python tools/barrc_scan.py . barrc_scan.json
```

## Defects: status after the 2026-11-25 fix pass

| Rule | Where | Status |
| --- | --- | --- |
| 1.8.c.i `volatile` for ISR-shared globals | `third_party/wifi_mesh/wst_link.c` `stats` (written by the UART RX ISR) | **Fixed**: `static volatile wst_link_stats_t stats`. The ISR's CPU-ID read now uses `sio_hw->cpuid` (same register as `get_core_num()`, but a guaranteed 32-bit volatile load). |
| 6.5.b ISRs named `*_isr` | `wst_link.c` `on_uart_rx` | **Fixed**: renamed `wst_uart_rx_isr`. (`DEV_GPIO_INT` in `src/gpio/gpio.c` is only called from the vendored Semtech driver; no team ISR is left misnamed.) |
| 5.3.b no bitwise ops on signed data | `firmware/sensing/sound_fft.c`, `sound_features.c` | **Fixed**: `sound_asr32()` in `sound_fft.h` is a well-defined arithmetic shift; `(1 << 14)` became `SOUND_Q15_HALF`; the peak test shifts `uint64_t`. |
| 8.3.b every `switch` has `default` | `alfp.c` `tx_step`/`rx_step`, `alfp_profiles.c`, `wst_link.c` x2 | **Fixed**: `default: break;` with a comment. |
| 6.3.b.iv no control transfer in macros | `lora_radio.c` `CHECK()` (28 uses), `common/sound_classes.h` `SC_CASE` | **Fixed**: `CHECK` expanded into explicit `if` blocks and removed; `SC_CASE` now sets a result and `break`s. |
| 6.3.b.iv (cont.) | `common/loopback_radio.c` `END()`, `host/sim/sim_mesh.c` `DEFINE_RADIO()` | **Deviation recorded** in a comment at each macro (they generate whole functions, so the `return` belongs to the generated function). Needs an approver name. |
| 4.2.c.ii no storage allocated in headers | `sound_model.h`, `sound_model_vectors.h` (both generated), `wifi_mesh_config.h` | **Deviation recorded** at each site. Moving the data needs new `.c` files in the FreeRTOS, SMP and host build lists, and the SMP build could not be run safely for this pass. |
| 8.2.d `else if` chain ends in `else` | 18 chains in 13 files | **Fixed**: empty `else { /* reason */ }` on each. |
| 8.4.c infinite loop is `for (;;)` | `third_party/lora_mesh/main.c` | **Fixed**. |
| 8.5.b no `exit()`/`abort()` | `host/experiments/experiments.c` x2, `host/integration/fake_wst_link.c` | **Deviation recorded** (laptop-only test tools; `wst_link_init()` returns `void`). |

Every **Deviation recorded** comment has an `Approved by: ____ (project manager), date: ____` line.
The standard's Deviation Procedure requires that line to be filled in before the code counts as
conforming.

### How the fixes were verified

- **Firmware (FreeRTOS, arm-none-eabi-gcc 14.2, Pico SDK 2.1.1)**: all 49 project object files
  were built before and after the changes and their disassembly compared. 46 are byte-identical
  machine code: every `else`/`default` addition, the `CHECK` expansion, `sound_classes.h`, and all
  of `alfp/`, `firmware/app/`, `lora_mesh`. The 3 that differ are the intended ones:
  `sound_fft.c` and `sound_features.c` (the shift helper) and `wst_link.c`, where the ISR keeps the
  same 73 instructions and `wst_link_poll` only changes register allocation. With the later
  `snprintf` fix (below), `alfp.c` differs too, so it is 45 of 49 identical.
- **FFT / features are bit-exact**: `sound_asr32()` matched the compiler's `>>` on 68,480,608
  inputs (all shifts 0-31, edges, a dense range and random values, UBSan on). The old and new
  `sound_fft256()` + `sf_add()`/`sf_finish()` produced the identical output hash over 200,000
  random FFTs and 3,000 full feature runs.
- **FFT cost**: the well-defined shift costs a sign test per shift. `sound_fft256` grows from 376 to
  468 bytes, about 18-21k extra cycles per FFT, or about 50 ms extra over a 5 s capture (hardware
  logs show `feat_us` ≈ 910 ms, so roughly 960 ms after). The audio DMA ring holds 512 ms and the
  slowest measured `sf_add` call was 3.2 ms, so there is no risk of losing samples.
- **Host**: all 26 build and test steps of `host/CMakeLists.txt` + `host/sound.cmake` pass (WSL gcc 15,
  `-Werror -Wconversion`, ASan+UBSan), including both real-time mesh integration tests and the
  Python cross-checks; `tools/check_style.sh`'s compile stage passes. `tools/demo_multihop.py`'s
  parser still reads the same neighbour table from `wifi_mesh_config.h`.
- **Not run**: hardware tests.

### Build break fixed on this branch

Commit `d57bd31` (2026-11-25, on `main`) added `snprintf(why, ..., "cannot rename to %s",
final_path)` in `alfp/src/alfp.c` with `why` one byte too small for the longest `final_path`.
`-Wformat-truncation` fires there, and the firmware's `-Werror` (and the host's) turned it into a
build failure, which is likely why `main`'s FreeRTOS UF2s were not rebuilt after Nov 19. Fixed by
sizing `why` as `sizeof("cannot rename to ") + sizeof(final_path)`. In the compiled code only that
function's stack frame changes (240 to 248 bytes). The FreeRTOS firmware and the host tests now
build with the project's unmodified `-Werror` flags.

### UF2 images on this branch (rebuilt 2026-11-25)

All 14 images in `uf2/` were rebuilt from this branch: the 9 FreeRTOS images with
`cmake -S firmware` and the 5 `csc09_smp_node_*` with `firmware_smp/buildglue/build_smp.sh`.

- Toolchain: Arm GNU Toolchain 14.2.Rel1 (**Windows** build), Pico SDK 2.1.1, FreeRTOS and FatFS at
  the `tools/fetch_deps.sh` commits, picotool 2.3.0 from the Pico VS Code extension. pioasm 2.3.0
  from the extension was used through a version shim; its output for `rp2040_sdio.pio` has the same
  instruction words as pioasm 2.1.1's, and the only differences are the banner comment and
  `const` spelled as `__pio_const`, which is `const` in C.
- `main`'s images were built with the **macOS** build of Arm GCC 14 (its newlib path strings
  `/Volumes/data/jenkins/...` are in the binaries). The two toolchain builds lay out the C library
  differently, so a byte-for-byte comparison with `main`'s images is not possible: rebuilding
  `main`'s own commits on Windows also gives different bytes. What *is* proven is the object-level
  comparison above. With the same toolchain, 45 of the 49 project objects are byte-identical to
  `main`, and the other 4 are the intended changes.
- The FreeRTOS images on `main` date from commit `163d5a3` (Nov 19). These new ones also contain
  `main`'s later commits `d57bd31` and `463f028`, which had only reached the SMP images until now.
- **Flash one node and run `docs/TEAM_NODE_TEST.md` before using these images for a demo.**

## Rule-by-rule results

Counts are hits across the 116 files. "Preferred" rules are recommendations in the standard, not
"shall" rules.

### Section 1: General rules

| Rule | Status | Count / notes |
| --- | --- | --- |
| 1.1.a C99 | Pass (build flags use `-std=c11`, a superset; note it as a deviation) | |
| 1.1.c minimal `#pragma`/asm/extensions, kept in drivers | Mostly | 17 uses, 11 in `firmware_smp/buildglue` (driver glue, acceptable) |
| 1.1.d no `#define` renaming keywords | Pass | |
| 1.2.a lines ≤ 80 chars | **Fail** (deviation recorded at 120, no approver named) | 2,616 lines > 80 (1,975 excluding the generated model) |
| 1.3.a braces on every body | **Fail** | 82: 36 `wifi_mesh`, 31 `host/`, 13 `lora_mesh`, 2 `firmware_smp/buildglue/usb_sdk_compat_overlay.c:240,248` |
| 1.3.b braces on their own line | **Fail** | 107: K&R style in `wifi_mesh` (66), `sensors` (37), `lora_mesh` (3) |
| 1.4.b parentheses around `&&`/`||` operands | **Fail** | 168 in 42 files, e.g. `alfp/src/alfp.c:171` |
| 1.5.b project abbreviation table | **Fail** | none in the repo |
| 1.6.a every cast has a comment | **Fail** | ~539 casts without a comment |
| 1.7.a/b no `auto`/`register` | Pass | |
| 1.7.c `goto` only jumps forward | Pass | 8 forward `goto done` in `host/tools/sound_featx.c` (allowed; avoiding them is preferred) |
| 1.7.d avoid `continue` (preferred) | Advisory | 29 |
| 1.8.a `static` for module-local items | Mostly | see 6.1.i/7.1.j below |
| 1.8.c `volatile` where shared with an ISR | Pass (fixed 2026-11-25) | `wst_link.c` `stats`, see defects above |

### Section 2: Comments

| Rule | Status | Count / notes |
| --- | --- | --- |
| 2.1.b/c no nested tokens, no commented-out code | Pass (by scan) | |
| 2.2.h Doxygen-style comments on every module and function | **Fail** | only 6 of 116 files use `@brief`/`@param` |
| 2.2.i `WARNING:`/`NOTE:`/`TODO:` markers | Pass | no `FIXME`/`XXX` |

### Section 3: White space

| Rule | Status | Count / notes |
| --- | --- | --- |
| 3.1.j one space between function name and `(` in a definition | **Fail** | 710 of ~710 definitions (`int foo(void)` should be `int foo (void)`) |
| 3.2.a–c align declarations, struct members, assignments | **Fail** | `.clang-format` has `AlignConsecutiveAssignments: false` |
| 3.2.d `#` in column 0 | Pass | |
| 3.3.a one statement per line | **Fail** | 29 (`wifi_mesh` 19, `host` 8) |
| 3.3.c end-of-file comment followed by a blank line | **Fail** | 114 of 116 files |
| 3.4 indent multiple of 4, case labels aligned | Pass | |
| 3.5.a no tab characters | **Fail** (1) | `third_party/lora_mesh/src/gpio/gpio.c:2` |
| 3.6.a LF line endings | Pass | all 202 files are LF in git (CRLF only appears in the Windows checkout) |

### Section 4: Modules

| Rule | Status | Count / notes |
| --- | --- | --- |
| 4.1.a lowercase file names | **Fail** | `DHT22.c/.h`; `FreeRTOSConfig.h` (name fixed by FreeRTOS, so document it as a deviation) |
| 4.1.b names unique in first 8 chars | **Fail** | 10 groups, e.g. `capture_api`/`capture_id`/`capture_meta`, `sound_classes`/`sound_classify` |
| 4.1.d a file with `main()` has "main" in its name | **Fail** | 13 (host tests/tools, `third_party/lora_mesh/diag.c`) |
| 4.2.a one `.h` per `.c` | **Fail** | 24 `.c` files without a header (mostly host tests) |
| 4.2.b include guards | Pass | all headers |
| 4.2.c.i no `extern` variables in headers (preferred) | Advisory | 9 |
| 4.2.c.ii no storage in headers | Deviation (needs approval) | 3 headers, see defects above |
| 4.3.c `.c` includes its own `.h` | Pass | |
| 4.3.f no `#include` of a `.c` | Pass | |
| 4.4.a project file templates | **Fail** | none in the repo |

### Section 5: Data types

| Rule | Status | Count / notes |
| --- | --- | --- |
| 5.1.a type names end in `_t` | Mostly | |
| 5.2.a fixed-width types where width matters | Review | 430 plain `int`/`unsigned`; many are return codes/handles, so check each |
| 5.2.b no `short`/`long` | **Fail** | 146, mostly `(unsigned long)` casts for `%lu`; use `PRIu32` from `<inttypes.h>` |
| 5.2.c `char` only for strings | **Fail** | 12 (mostly `firmware_smp`) |
| 5.3.b no bitwise ops on signed data | Pass for the cited files (fixed 2026-11-25) | `sound_fft.c`, `sound_features.c`; other files not re-audited |
| 5.3.c no signed/unsigned mixing | Pass by compiler (`-Wsign-compare -Wconversion`) on the host-compiled files | |
| 5.4.b.i `float32_t`/`float64_t` names | **Fail** | 215 `float`/`double` (159 excluding the generated model) |
| 5.4.b.ii `f` suffix on float constants | **Fail** | 38 |
| 5.5.a no padding in wire/bus structs | Pass | ALFP serializes field by field (`alfp_wire.c`), with no struct `memcpy` |
| 5.6.a `bool` for booleans | Pass | |

### Section 6: Procedures

| Rule | Status | Count / notes |
| --- | --- | --- |
| 6.1.c no leading `_` | **Fail** | 7 (`firmware_smp`, 1 `firmware/app`) |
| 6.1.d names ≤ 31 chars | **Fail** | 4 (host tests) |
| 6.1.e no uppercase in function names | **Fail** | 14 (`DHT_init`, `DEV_GPIO_INT`, ...) |
| 6.1.f no lowercase in macro names | **Fail** | 2 (`firmware/app`) |
| 6.1.i public functions prefixed with their module name | **Fail** | ~118 (71 in `firmware_smp`) |
| 6.2.a functions ≤ 100 lines (quantity rule, can be changed) | **Fail** | 7: `alfp.c:361 tx_step` 153, `main_smp.c:745 comms_task` 148, `phototest_main.c:346 main` 133, `alfp_wire.c:200 alfp_decode` 128, `experiments.c:261 suite` 128, `main.c:450 comms_task` 120, `test_uplink.c:41` 105 |
| 6.2.c single exit point (preferred) | Advisory | 152 functions with more than one `return` |
| 6.3.a no function-like macros when a function would do | Review | 19 |
| 6.4.a task functions end in `_task` | Pass | |
| 6.5.b ISRs end in `_isr` | Pass (fixed 2026-11-25) | `wst_uart_rx_isr`; scanner's other hits are false positives |

### Section 7: Variables

| Rule | Status | Count / notes |
| --- | --- | --- |
| 7.1.e names ≥ 3 chars (including loop counters) | **Fail** | ~666 (`i`, `n`, `r`, `h`, `d`, ...) |
| 7.1.j globals start with `g_` | **Fail** | 25 non-static globals, e.g. `sound_twiddle_cos`, `tm_usb_irq_count` (file-`static` `s_` names are fine) |
| 7.1.k pointers start with `p_` (`pp_` for pointer to pointer) | **Fail** | ~695 (e.g. `ctx`, `buf`, `path` parameters) |
| 7.1.m booleans start with `b_` | **Fail** | 68 |
| 7.2.d uninitialized pointers set to `NULL` | Not checked by tool | |

### Section 8: Statements

| Rule | Status | Count / notes |
| --- | --- | --- |
| 8.1.a no comma in declarations | **Fail** | ~109 (excluding initializer lists), e.g. `main.c:81` |
| 8.2.c no assignment in `if` tests | Pass | scanner hits were braceless bodies, already counted under 1.3.a |
| 8.2.d `else if` ends in `else` | Pass (fixed 2026-11-25) | was 18 |
| 8.3.b `switch` has `default` | Pass (fixed 2026-11-25) | was 5 |
| 8.4.b no assignment in loop conditions | **Fail** | 4 |
| 8.4.c `for (;;)` for infinite loops | Pass (fixed 2026-11-25) | was 1 |
| 8.5.b no `exit`/`abort`/`setjmp`/`longjmp` | Deviation (needs approval) | 3, host test tools only |
| 8.6.a constant on the left of `==`/`!=` | **Fail** | 374 in 48 files (e.g. `storage_fatfs.c:31 if (r == FR_OK)`) |

## Errors in `docs/CODING_STANDARD.md`

- Cites **5.3.b** for "no signed/unsigned mixing"; that rule is **5.3.c**. 5.3.b (no bitwise ops on
  signed data) is actually violated in `sound_fft.c`.
- Claims 1.3.a "yes" for its scope, but `firmware_smp/buildglue/usb_sdk_compat_overlay.c:240,248`
  (in scope) has braceless `for (;;)` bodies.
- Describes 1.7.c as "no `goto`"; the rule allows forward `goto` and only prefers avoiding it.
- Scope leaves out `host/`, `third_party/lora_mesh/`, `third_party/sensors/` and most of
  `wifi_mesh`, which are team code. If "all projects must conform", those count.
- The deviation section has no approver name, which the Deviation Procedure requires.

## Suggested fix order

1. **Real defects** (table above): done 2026-11-25, except the recorded deviations, which need
   an approver name.
2. **Mechanical white space** in separate commits, as the standard asks (p. 5). Most of it is a
   `.clang-format` change: `InsertBraces: true`, `BreakBeforeBraces: Allman`,
   `SpaceBeforeParensOptions: {AfterFunctionDefinitionName: true}`,
   `AlignConsecutiveAssignments/Declarations: true`, plus a script that appends `/*** end of file ***/`.
   This covers 1.3.a/b, 3.1.j, 3.2.a–c, 3.3.c and the tab.
3. **Line width**: either reflow to 80, or keep 120 as an approved project-level quantity deviation
   with the approver's name added to `CODING_STANDARD.md`.
4. **Expression style**: 8.6.a (374), 1.4.b (168), 8.1.a (~109), 5.2.b `%lu` to `PRIu32`, 5.4
   float typedefs and `f` suffixes. Manual, but each change is local.
5. **Naming** (biggest job, highest risk): 7.1.k/e/m/j, 6.1.e/i, 4.1 file names. Use clang-tidy
   `readability-identifier-naming` per module, and re-run host tests and both firmware builds after each module.
6. **Documents**: abbreviation table (1.5.b), header/source templates (4.4.a), Doxygen headers (2.2.h),
   cast comments (1.6.a), and split the 7 long functions (6.2.a) or record a quantity deviation.
