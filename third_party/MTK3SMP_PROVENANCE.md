# third_party/mtk3smp_rp2040 provenance

Vendored as a git submodule, pinned at commit `900e86306dc3f3dcd8bbbd23f0e9869412a81d57`
(2026-10-26).

- Upstream: https://github.com/TemariVirus/mtk3smp-rp2040
- Fork chain: `sirfonzie` -> `hollowheap` -> `TemariVirus`, derived from the official
  `tron-forum/mtk3_bsp` `pico_rp2040` branch (tag `v1.00.00.B5-pico_rp2040`).
- **This is not an official T-Engine Forum release.** It is an individual's port
  ("Muhamed Fauzi Bin Abbas, with AI Assistants"), 2 commits at the time this was
  vendored, 0 stars/forks/watchers, longest documented soak test 10 seconds. No
  official uT-Kernel 3 SMP port for RP2040 exists (see ../docs/MICROKERNEL.md). The repo owner explicitly approved using this port despite the risk.
- License: mixed. The uT-Kernel 3.0 core and this port's kernel-level SMP additions
  are T-License 2.2 (Copyright Ken Sakamura / TRON Forum). The RP2040 integration
  glue (lwIP port, cyw43 service, TinyUSB glue, app_program/) is Apache-2.0
  (Copyright (c) 2026 Muhamed Fauzi Bin Abbas). Some files copied from the Pico SDK
  are BSD-3-Clause. The file header governs when these summaries disagree; some
  files still carry older T-License 2.1 headers. See `LICENSE` and `LICENSES/`
  inside the submodule. The T-License 2.x text itself may carry a registration
  obligation with the T-Engine Forum that was not verified here (binary PDF, not
  read) -- flagged in the PR for a human to check before shipping.
- Build system: GNU Make (`build_make/`), not CMake -- see
  ../docs/MICROKERNEL.md for why this repo's build
  integration lives in ../firmware_smp/buildglue/ rather than patching this
  submodule directly.

Do not edit files inside this submodule. Build-time customization (app sources,
`sources.mk`) lives in ../firmware_smp/buildglue/ and is copied in by the build
script, never committed into the submodule itself.
