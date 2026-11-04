# Coding standard (CURVEBALL round 3, Task B)

## Standard used

**BARR-C:2018**, the Barr Group Embedded C Coding Standard, as requested by the professor.

**This requires verification**: the actual BARR-C:2018 document was not available to consult
directly in this environment (no network fetch of the Barr Group text, and no copy bundled in
this repo -- checked `docs/` and the whole tree for "BARR-C"/"Barr Group", found nothing). Every
rule below is therefore cited by its well-known plain-English theme (fixed-width types, braces on
every block, no magic numbers, one statement per line, `const`/`static` use, no implicit
signed/unsigned conversion, `volatile` for data shared with an ISR or another core, naming, no
recursion, bounded loops), never by a specific rule number, since a number could not be confirmed
against the real text. Before this is presented as BARR-C:2018 conformance to the professor,
someone with access to the actual standard should check the themes below against its real rule
numbers.

## Scope

In scope for this task: `alfp/**`, `common/**`, `firmware/app/**`, `firmware/sensing/**`,
`firmware_smp/**` (excluding the vendored kernel, see below), the lines of
`third_party/wifi_mesh/wst_link.c`/`.h` added by this project's own commits (not Goutham's
original vendored code), and new test code. Out of scope / excluded: `third_party/mtk3smp_rp2040/**`
(vendored kernel fork), `third_party/lora_mesh/**`, `third_party/sensors/**`, and everything else
not listed. `firmware/app/main.c`, `firmware_smp/app/main_smp.c`, and every file under `alfp/**`
were left untouched by this task specifically to avoid colliding with Task A's work on those same
files in this branch -- their style was reviewed read-only (see "Other owners'/other tasks'
files" below) and any fixes there are for whoever owns that code next.

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

## What was and was not actually run in this environment

- **clang-format, cppcheck, clang-tidy: not installed**, neither on Windows PATH nor inside WSL
  Ubuntu (checked with `command -v` for each, in both places). `tools/check_style.sh` detects
  this and prints `SKIPPED: <tool> not installed` rather than claiming a result. Install with
  (Ubuntu/WSL) `sudo apt install clang-format clang-tidy cppcheck`, or (Windows)
  `winget install LLVM.LLVM` for clang-format/clang-tidy and `winget install cppcheck`.
- **Compiler warnings: actually run**, via WSL `gcc` (no `cmake`/`make` available either; invoked
  directly) with `-std=c11 -Wall -Wextra -Wconversion -Wshadow -Wundef -Wstrict-prototypes -Werror`,
  on every in-scope `.c` file that compiles standalone without the real Pico SDK, FreeRTOS, or
  uT-Kernel headers:
  - `common/loopback_radio.c`, `common/uplink.c`, `firmware/sensing/audio_features.c`,
    `firmware/sensing/sound_features.c`, `firmware/sensing/sound_fft.c`,
    `firmware/sensing/sound_classify.c`, `firmware/sensing/sound_image_pick.c`,
    `firmware/sensing/sound_tables.c` -- **all 8 compiled with zero warnings and zero errors**
    under those flags.
  - `firmware/sensing/sensing.c` could not be compiled standalone: it pulls in
    `third_party/sensors/DHT22.h`, which needs the real Pico SDK (`pico/stdlib.h`). Reviewed by
    reading instead (see findings below); genuinely compiling it needs a real toolchain run.
  - `firmware/app/board.c`, `radios.c`, `storage_fatfs.c`, `freertos_hooks.c`, `hw_config.c`, all
    of `firmware_smp/**`'s own C files (`log.c`, `usermain_smp.c`, `rtos_port_mtk3.c`,
    `buildglue/usb_tinyusb_glue_overlay.c`), and the `wst_link.c` edit: all need the real Pico
    SDK / FreeRTOS / uT-Kernel headers to compile and were **not compiled** here; reviewed by
    reading.
- **Braces-on-every-block, checked by direct search, not assumption**: a pattern search for
  `if`/`while`/`for` followed by a single statement with no braces (`if (...) stmt;` on one
  line) found **zero matches** across every in-scope, editable file (the two off-limits files,
  `main.c` and `main_smp.c`, were excluded from this search since they belong to Task A's work
  in this branch).

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

- `.clang-format`'s `ColumnLimit: 120`: this codebase's longest existing lines already run to 121
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
