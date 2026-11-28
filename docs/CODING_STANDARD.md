# Coding standard: BARR-C:2018

[Back to the README](../README.md)

The C code follows the **Barr Group Embedded C Coding Standard, BARR-C:2018**. The standard's
*Deviation Procedure* requires conformance to every rule except where a deviation is documented
and approved; those are listed [below](#deviations).

**Project manager approving deviations: Senthil Shreya. Date: 28 November 2026.**

## Scope

All project C files (`*.c`, `*.h`): `alfp/`, `common/`, `mesh/`, `firmware/`, `firmware_smp/`,
`host/`, and the team modules in `third_party/lora_mesh/` (outside `src/`),
`third_party/wifi_mesh/` and `third_party/sensors/`.

Outside the scope, as vendor code: the kernel submodule `third_party/mtk3smp_rp2040/`, and the
Semtech LR11xx driver and Waveshare GPIO/SPI helpers in `third_party/lora_mesh/src/`. Python and
TypeScript are not covered by a C standard.

## How to check

```bash
tools/check_style.sh
```

| Step | Tool | Rules |
| --- | --- | --- |
| Layout | `clang-format` with [`.clang-format`](../.clang-format) | braces, brace placement, indentation, spacing, alignment |
| Source rules | [`tools/check_barrc.py`](../tools/check_barrc.py) | 1.2.a, 1.7.a/b, 3.3.c, 3.5.a, 4.1.a, 4.2.b, 5.2.b, 5.4.b.i, 8.4.c, `abort`/`setjmp`/`longjmp` in 8.5.b |
| Compile | `gcc -Werror -Wconversion -Wshadow` on the laptop-buildable files | type safety |

**Limit of the automation:** the naming, expression and statement rules (for example 7.1, 8.6.a,
1.4.b) were applied with one-off refactoring passes driven by the compiler's parser and then
reviewed. No script in the repository re-checks them, so reviewers must keep applying them to new
code.

## How the main rules are met

| Rule | Requirement | In this repository |
| --- | --- | --- |
| 1.2.a | lines of at most 80 characters | all in-scope lines |
| 1.3.a, 1.3.b | braces on every body, each brace on its own line | Allman style, braces inserted everywhere |
| 1.4.b | parentheses around `&&` and `||` operands | applied unless the operand is a single name or constant |
| 1.7.c | `goto` only forwards | only in `host/tools/sound_featx.c`, all forward |
| 1.8 | `static`, `const`, `volatile` where they apply | including `volatile` for data shared with the UART interrupt |
| 3.1.j | one space between a function name and its parameter list in definitions | all definitions |
| 3.2 | aligned declarations and assignments | by `clang-format` |
| 3.3.c | end-of-file comment followed by a blank line | `/*** end of file ***/` |
| 3.5.a | no tabs | none |
| 5.2 | fixed-width types where width matters; no `short` or `long` | `<stdint.h>` types, with `<inttypes.h>` format macros |
| 5.3.b | no bit operations on signed values | `sound_asr32()` gives a defined arithmetic shift |
| 5.4.b | `float32_t` and `float64_t` | defined with size assertions in [`common/float_types.h`](../common/float_types.h) |
| 6.3.b.iv | no control transfer inside macros | except D8 |
| 6.4.a, 6.5.b | task names end in `_task`, interrupt handlers in `_isr` | |
| 7.1.e | variable names of at least 3 characters | including loop counters (`idx`) |
| 7.1.j to 7.1.m | `g` for globals, `p` pointer, `pp` pointer to pointer, `b` boolean | see below |
| 8.1.a | one declaration per line | |
| 8.2.d | every `else if` chain ends in `else` | |
| 8.3.b | every `switch` has `default` | |
| 8.4.b | no assignment in a loop condition | |
| 8.4.c | infinite loops written `for (;;)` | |
| 8.6.a | constant on the left of `==` and `!=` | |

## Naming conventions

Prefixes combine in the order global, pointer, boolean:

| Prefix | Meaning | Example |
| --- | --- | --- |
| `p_`, `pp_` | pointer, pointer to pointer (including function pointers) | `p_ctx`, `pp_radios` |
| `b_` | boolean | `b_is_ok` |
| `g_`, `gp_`, `gb_` | global, global pointer, global boolean | `g_alfp`, `gp_lora`, `gb_want_stats` |
| `s_`, `p_s_`, `b_s_` | file-static, with the same pointer and boolean letters in front | `s_stack_pool`, `b_s_lora_ok` |

### Module prefixes and abbreviations (rules 1.5.b and 6.1.i)

Public functions start with their module's name or the abbreviation listed here.

| Prefix | Module | | Prefix | Module |
| --- | --- | --- | --- | --- |
| `alfp_` | file transfer protocol | | `sf_` | sound features |
| `capture_meta_`, `cm_` | capture record | | `sm_` | sound model |
| `capture_id_` | capture ids | | `sip_` | sound image picking |
| `uplink_` | bridge USB output | | `sound_` | sound classifier, labels and FFT |
| `lora_mesh_`, `lora_radio_` | LoRa mesh and radio | | `sensing_`, `audio_` | sensing and audio capture |
| `wifi_mesh_`, `wst_` | WiFi mesh and serial link | | `rtos_` | RTOS port layer |
| `board_`, `radios_`, `radio_` | board and radio setup | | `log_` | log ring |
| `app_` | application entry points | | `smartrelay_` | SmartRelay's kernel runtime layer |
| `sim_`, `net_` | laptop simulator | | `storage_` | storage back ends |

Other abbreviations: `cfg` configuration, `ctx` context, `idx` index, `len` length, `msg`
message, `rx`/`tx` receive and transmit, `sid` session id, `cap` capacity or capture, `stats`
statistics, `ms`/`us` milliseconds and microseconds.

## Deviations

All approved by **Senthil Shreya (project manager), 28 November 2026**. Where a deviation belongs
to one place in the code, it is also recorded in a comment there.

| Id | Rule | Deviation | Reason |
| --- | --- | --- | --- |
| D1 | 1.1.a | C11 / GNU11 instead of C99 | the Pico SDK builds with GNU11; `_Static_assert` is used |
| D2 | 1.6.a | not every cast has its own comment | most are `printf` arguments cast to fixed-width types, discarded values (`(void)x`) and byte packing with shifts. `-Wconversion` is enforced on the protocol library and sound code built on the laptop |
| D3 | 2.2.h | comments are not in Doxygen form | each module has a header comment describing it; no Doxygen output is generated |
| D4 | 4.1.a, 4.1.b | `FreeRTOSConfig.h` has capitals; some file names share their first 8 characters | FreeRTOS fixes that name; the toolchain has no 8-character limit |
| D5 | 4.1.d, 4.2.a | laptop test and tool programs with `main()` are named `test_*` or by purpose, without headers | build scripts and test names rely on them; they export nothing |
| D6 | 4.2.c.ii | storage defined in three headers | two are generated model and test tables with a single includer; `wifi_mesh_config.h` is a small constant table kept editable in one place |
| D7 | 6.1.c, 6.1.e, 6.1.i, 7.1.j | some names break the naming rules | they are fixed by external interfaces: newlib (`_sbrk`, `_write`, `__malloc_lock`), Pico SDK functions re-implemented for the kernel, FreeRTOS hooks, kernel symbols (`tm_usb_*`) and linker symbols (`__mutex_array_*`) |
| D8 | 6.3.b.iv | `return` inside `END()` and `DEFINE_RADIO()` | each macro generates whole functions; the `return` belongs to the generated function |
| D9 | 6.2.a | function length limit raised from 100 to **200 lines** (a project-level quantity change, which the standard permits) | the 80-column limit lengthened existing state machines; the longest is `tx_step()` at about 180 lines. Splitting them without hardware to retest was judged riskier |
| D10 | 8.5.b | `exit()` in three places in laptop-only tools | a failed setup must stop the run; never linked into firmware |
| D11 | 3.2 | the WiFi neighbour table is excluded from `clang-format` | `tools/demo_multihop.py` reads it as text |

Rules the standard marks as *preferred* are not enforced: avoiding `goto` and `continue` (1.7.c,
1.7.d), a single exit point per function (6.2.c), and no `extern` variables in headers
(4.2.c.i). Plain `int` is used where the width does not matter, as rule 5.2.a allows.

## Verification

The restyle changed thousands of lines, so it was checked at the machine-code level. The final
code was compared with the code as it was before the standard was applied, both built with the same
Arm GNU Toolchain 14.2 and Pico SDK 2.1.1, comparing the disassembly of each function while
ignoring symbol names and addresses.

**FreeRTOS build: three object files differ; all others are identical.** (The comparison covered
every object file of the build, including SDK and library objects.) The three differences are
deliberate defect fixes from the first audit pass:

| File | Fix |
| --- | --- |
| `third_party/wifi_mesh/wst_link.c` | counters written by the UART interrupt are now `volatile`; the handler is named `wst_uart_rx_isr` |
| `firmware/sensing/sound_fft.c`, `sound_features.c` | shifts of signed values replaced by `sound_asr32()` |

`sound_asr32()` was tested when introduced against the compiler's own shift on about 68 million
inputs, and the FFT and features produced identical output on 200,000 random FFTs. It adds an
estimated 50 ms of processing per 5 s capture, within the audio buffer's margin.

**Dual-core build:** the renaming, layout and later passes were compared against a build made
after those three fixes, for nodes 1 and 2. One function, `usb_owner_task`, differs only in which
stack slots hold some constants; every other function is identical.

**Limits of this check:** it covers the code built by the default configurations. Optional
branches and laptop-only code were checked by building and, where tests exist, running them.
**The restyled firmware has not been run on hardware.** Flash one node and follow
[BRINGUP.md](BRINGUP.md) before relying on it.
