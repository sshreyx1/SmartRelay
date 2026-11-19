# Coding standard (CURVEBALL round 3, Task B)

## Standard used

**BARR-C:2018**, the Barr Group Embedded C Coding Standard, as requested by the professor.

### Rules applied, by BARR-C:2018 rule number

Rule numbers below are cited from BARR-C:2018's published numbering (free PDF from Barr Group).
They were written from the standard's numbering as known on 2026-11-19, not copied from a copy of
the PDF bundled here (there is none in the repo); someone should open the PDF once and tick each
row before the table is shown to the professor as a rule-by-rule audit.

| BARR-C:2018 rule | Requirement (short) | How this repo meets it | Checked by |
| --- | --- | --- | --- |
| 1.2.a | lines no longer than 80 characters | **deviation**: 120 (see Deviations) | `.clang-format` `ColumnLimit: 120` |
| 1.3.a | braces around every `if`/`else`/`switch`/`while`/`do`/`for` body, even one statement | yes | regex search for `if/for/while/else` followed by a statement on the same line: 0 hits |
| 1.3.b | each `{` and `}` on its own line, aligned (Allman) | yes | `.clang-format` `BreakBeforeBraces: Allman`, `tools/check_style.sh` |
| 1.7.c | no `goto` | yes (0 in scope) | `grep -rn goto` over scope |
| 1.8.a / 1.8.b / 1.8.c | use `static`, `const`, `volatile` where they apply | file-local functions and data are `static`; read-only tables `const`; data shared between tasks/cores or with an ISR `volatile` (e.g. `log.c` `s_dropped`/`s_queued`, `main_smp.c` `g_task_core`) | review; cppcheck `constVariable`/`constParameter` style checks |
| 3.4.a | indentation in multiples of 4 spaces | yes | `.clang-format` `IndentWidth: 4` |
| 3.5.a | no tab characters | yes | `.clang-format` `UseTab: Never` |
| 5.2.a | fixed-width types (`<stdint.h>`) wherever width matters | yes; plain `int` only for small counters/handles whose width does not matter | review + `-Wconversion` |
| 5.3.b | no mixing signed and unsigned in comparisons/expressions | compiled with `-Wconversion -Wsign-compare`; the `%u`-with-signed-`NODE_ID` printf cases cppcheck found were fixed with explicit `(unsigned)` casts | gcc + cppcheck |
| 6.2.a | functions kept short (about one page / 100 lines) | mostly; known long ones: `comms_task`, `usb_owner_task`, `handle_capture` (main loop / key dispatch) | review (not enforced by a tool) |

## Scope

## Scope

In scope for this task: `alfp/**`, `common/**`, `firmware/app/**`, `firmware/sensing/**`,
`firmware_smp/**` (excluding the vendored kernel, see below), the lines of
`third_party/wifi_mesh/wst_link.c`/`.h` added by this project's own commits (not Goutham's
original vendored code), and new test code. Out of scope / excluded: `third_party/mtk3smp_rp2040/**`
(vendored kernel fork), `third_party/lora_mesh/**`, `third_party/sensors/**`, and everything else
not listed. (Round 3 left `main.c`, `main_smp.c` and `alfp/**` unformatted to avoid colliding with
Task A; the 2026-11-19 verification round formatted them too, so the whole scope is now clean.)

## How to run format / lint / build locally

```sh
# Format check (dry run, no changes):
clang-format --dry-run --Werror $(git ls-files '*.c' '*.h' | grep -E '^(alfp|common|firmware)/')

# Format, in place:
clang-format -i <file>

# Lint (cppcheck):
cppcheck --enable=warning,style,performance,portability --std=c11 --language=c \
  --suppressions-list=tools/cppcheck_suppressions.txt \
  -I alfp/include -I common -I mesh -I firmware/app -I firmware/sensing \
  alfp/src common/*.c firmware/app/*.c firmware/sensing/*.c

# Lint (clang-tidy), needs a compile_commands.json or explicit flags per file, e.g.:
clang-tidy common/uplink.c -- -std=c11 -I alfp/include -I mesh -I common

# Warnings-as-errors compile check (host-compilable files only; see "What was and was not
# actually run" below for which files that is):
gcc -std=c11 -Wall -Wextra -Wconversion -Wshadow -Wundef -Wstrict-prototypes -Werror -c -o /dev/null \
  -I <includes for the file> <file.c>
```

**`tools/check_style.sh`** wraps all of the above over the in-scope files, skipping (not failing)
any tool that is not installed, and exits non-zero only on an actual violation it could check. It
is stage 1 of `tools/run_all_tests.sh`.

## What was actually run (2026-11-19, macOS, `docs/verify/RESULTS.md`)

- **clang-format 23.1.4** (Homebrew): `tools/check_style.sh`'s format stage over every in-scope
  file. The first ever run found 35 files not matching `.clang-format` (33 from before plus two this
  round touched); all 35 were reformatted with `clang-format -i`, no code change, and the host
  tests (14/14) and both firmware builds were re-run afterwards. Format check now clean.
- **cppcheck**: see `docs/verify/RESULTS.md` (installed from Homebrew for this run).
- **Compiler warnings**: both firmware builds compile with zero warnings from project code
  (FreeRTOS build: `-Werror` on; SMP build: the kernel's own Make flags). The only remaining
  warnings are in vendored code (`lr11xx_radio.c` `#warning` about a deprecated sync-word call)
  and newlib's "not implemented" stubs at link time. Host build: `-Wall -Wextra -Werror -Wshadow
  -Wconversion` for the protocol library, 14/14 ctest.
- Earlier rounds (Windows/WSL) had no clang-format/cppcheck at all; that is no longer the case.

## Compliance table

| File / area | Braces on every block | Magic numbers | Fixed-width types | `volatile` for shared state | Status |
| --- | --- | --- | --- | --- | --- |
| `common/loopback_radio.c`, `common/uplink.c` | yes (search + compile) | named constants throughout (`alfp_config.h`-style) | yes (`<stdint.h>` throughout) | n/a (no cross-context state) | compiled clean |
| `firmware/sensing/audio_features.c`, `sound_features.c`, `sound_fft.c`, `sound_classify.c`, `sound_image_pick.c`, `sound_tables.c` | yes (search + compile) | named constants / table-driven | yes | n/a | compiled clean |
| `firmware/sensing/sensing.c` | yes (read) | yes, by read | yes | yes, flagged fields are `volatile` where shared with the capture ISR/trigger path | not compiled (needs Pico SDK); read-reviewed |
| `firmware/app/board.c`, `radios.c`, `storage_fatfs.c`, `freertos_hooks.c`, `hw_config.c` | yes (search) | hardware register/pin constants are named `#define`s, consistent with the project's existing convention | yes | reviewed, no cross-core/ISR state found needing it beyond what is already `volatile` | not compiled (needs Pico SDK/FreeRTOS); read-reviewed |
| `firmware_smp/app/log.c` | yes | yes (`LOG_RING_DEPTH` etc. named) | yes | yes (`s_dropped` is `volatile uint32_t`, written from 3 tasks across both cores) | not compiled (needs uT-Kernel); read-reviewed, clean |
| `firmware_smp/app/usermain_smp.c`, `rtos_port_mtk3.c`, `buildglue/usb_tinyusb_glue_overlay.c`, `rtos_port.h` | yes (search) | reviewed, no bare literals found outside hardware/kernel API constants | yes | reviewed | not compiled; read-reviewed |
| `third_party/wifi_mesh/wst_link.c`/`.h` (this project's edited lines only) | **one violation found and fixed**, see below | n/a (no new literals introduced) | yes | yes, `s_rx_isr_core_logged`/ring indices already `volatile` from the pre-existing code | fixed |

### Deviations (with justification)

- **Rule 1.2.a (80-column lines)**: `.clang-format`'s `ColumnLimit: 120`. This codebase's longest existing lines already run to 121
  characters (`firmware/app/main.c`); BARR-C traditionally suggests a narrower limit (80 is the
  commonly cited figure), but matching the project's own established width avoids a disruptive
  repo-wide reflow for a line-length rule this codebase was never written to. Recorded here, not
  silently ignored.
- `third_party/wifi_mesh/wst_link.c` keeps K&R brace style (opening brace on the same line as the
  statement) for the one new brace-adding fix below, instead of the Allman style
  (`alfp/**`/`firmware/app/**` convention) used elsewhere in this project: the surrounding file is
  Goutham's vendored code, written entirely in K&R style; switching just one new block to Allman
  inside an otherwise-K&R file would be a worse inconsistency than keeping the file internally
  uniform. The violation being fixed is the missing braces, not the brace placement.

### Fixed (separate commit from this doc)

- `third_party/wifi_mesh/wst_link.c:77` (in the lines this project's own commit
  `92b7354` added, not Goutham's original code): `if (fill > stats.rx_high_watermark)
  stats.rx_high_watermark = fill;` had no braces around its single-statement body. Fixed to add
  braces, no behavior change.

## Other owners'/other tasks' files: findings only, not fixed here

Counts only, per the brief -- these are not this task's files to fix.

- `alfp/**` (Task A's own files in this branch): read-only pattern search for braceless
  single-statement blocks found **0**. Not compiled with the strict flags as part of *this* task
  (Task A already built and ran its own tests against these files); no further findings recorded.
- `firmware/app/main.c`, `firmware_smp/app/main_smp.c` (edited by Task A in this branch): read-only
  pattern search for braceless single-statement blocks found **0**. Not otherwise reviewed in
  depth here, to avoid duplicating or second-guessing Task A's own work on these files.
- `ml/**`, `docs/sound/**`, `SOUND_PLAN.md`, `sd_card/**`: not reviewed at all (out of scope per
  the brief's ownership rules; counts would require reading files this task was told not to
  open for review purposes beyond what is already necessary to confirm they were not touched).
- `third_party/wifi_mesh/wst_link.c`/`.h` outside this project's own edited lines (i.e. Goutham's
  original vendored code): not reviewed -- out of scope, his file.
- `dashboard/**`: not reviewed (TypeScript/React, not in this task's C-focused scope; BARR-C does
  not apply to it).

## Open questions / risks

- Without the actual BARR-C:2018 text, the compliance table above is an honest best-effort
  against the standard's well-known public themes, not a verified rule-by-rule audit. Flag this
  to the professor if exact rule citations are required.
- `sensing.c` and the `firmware/app/` driver files could not be compiled here at all (missing Pico
  SDK/FreeRTOS/uT-Kernel headers); the compliance claims for them rest on a careful read, not a
  compiler. A real toolchain run (see `firmware_smp/buildglue/README.md` / `docs/BRINGUP.md`)
  would catch anything a read missed.
- `tools/check_style.sh` is wired into `tools/run_all_tests.sh` as its first stage.
