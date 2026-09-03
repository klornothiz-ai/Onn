# Roadmap: from research core toward running games

> Status: this project is a **research prototype** and makes no game
> compatibility claim. Commercial PS5 software is out of reach: the project
> does not ship or use firmware, keys, SONY decryption, or proprietary system
> software, and retail `SCE`/ciphertext Prospero SELF containers are rejected
> fail-closed by design (`src/prospero_boot.cpp`, `src/loader/prospero_self.cpp`).
>
> This document is the honest, staged path toward **homebrew and, ultimately,
> devkit PlaintextELF payloads** actually booting further through the pipeline.
> No stage claims commercial-game support.

## What already works today (verified by `make unit`)

- ELF64 x86-64 load (`elf_loader`, `runtime_linker`) with BSS clear, checked
  relocs, DT_NEEDED resolution, cached + persistent `ModuleInfo` (r33
  mark-as-unloaded fix).
- Prospero SELF **structure** parse → flatten to flat ELF when the payload is
  plaintext (`prospero_self.cpp`), with corruption fail-closed.
- A broadly-covered x86-64 interpreter (`x86_64_interpreter`, `x86_64_isa_ext`,
  `x86_64_x87`, `x86_64_simd_full`) with real syscall dispatch (85 FreeBSD
  handlers), guest threads/TLS, fork model, kqueue/event-flags, checked VMM
  guest memory.
- PM4 Type-3 whole-stream-validated decode → GCN subset software executor and
  RDNA2→SPIR-V compute compiler; headless-safe Vulkan boundaries.
- HLE: 1727 `LIB_FUNC` registrations across 31 modules (`libKernel`, `libNet`
  real POSIX sockets, `libSaveData` real persistence, `libPad`, `libAudio`
  headless…). Most exported symbols are still logging stubs.

## Staged plan (in priority order)

### Stage 0 — Build & link parity (unblocks everything)
- Goal: `make` (full `ps5_native_vulkan_emulator` + `prospero-run`) links
  reproducibly with and without the optional SDL2/FFmpeg/Vulkan/nlohmann/fmt
  packages. The optional-stub wiring already exists; chase residual undefined
  symbols to zero, then promote a full link to a CI step.
- Also: close the remaining `-Wno-*`/dead-code hygiene gaps so `-Werror` is
  honest (see `COMPREHENSIVE_ANALYSIS.md` low-severity list).

### Stage 1 — Homebrew ELF boot through the real pipeline (Sherpas now)
- Goal: `prospero-run <dir|elf> [--interpreter]` reliably boots a small
  homebrew ET_DYN/ET_EXEC ELF, runs its init + entry, and reports a clean exit
  code — not just inside the unit-test harness, but via the real CLI.
- Round 34: `make prospero-run` now **links** (Stage 0 largely done — the
  README's "linking remains blocked" claim is stale) and a real gcc/ld PIE
  boots through the CLI and executes a guest syscall (`exit 42`). The
  non-page-aligned PT_LOAD abort ("segment mapping collision") was fixed and is
  covered by `boot_unaligned_ptload_test`.
- Known structural limit: the guest arena is identity-mapped at
  `0x1000000000` (64 GiB) for the direct-execution backend, so a conventional
  host **ET_EXEC** at link-time `0x400000` cannot load. PS5 eboots are
  ET_SCE_DYNEXEC (PIE, high addresses) and are unaffected. Supporting low-address
  ET_EXEC would need a second, non-identity guest arena — a deliberate future
  decision, not a quick fix.
- Also: the guest `exit` syscall is caught but not yet propagated into the
  process exit code reported by the CLI — close that to make exit-code reports
  honest.
- Acceptance: add an integration test that runs `prospero-run` on a synthesized
  ELF and asserts `exit_code` and `syscalls_intercepted > 0`.
- Unknown-ISA verdicts: any instruction the interpreter can't execute must fail
  closed with a named opcode so coverage gaps are visible, not silent.

### Stage 2 — One real module, end-to-end
- Goal: replace the *stub* backends that games hit on the boot path first
  (`libGraphicsDriver`, `libVideoOut`, `libDebug`) with behaviorally real,
  host-backed implementations, driven by PM4/video-out state already tracked.
- Rationale: games do not "run" until the graphics subsystem stops returning OK
  while doing nothing. Start with the smallest honest surface that a homebrew
  payload actually exercises.

### Stage 3 — GPU compute & raster fidelity
- Goal: make SMEM/MUBUF/image/DS paths in `gcn_decoder` + `rdna2_compute_compiler`
  execute over real guest buffers through `vulkan_compute_executor`
  (headless => reference framebuffer), not just validate. Close the
  buffer-resource-table gap that currently fails MUBUF dispatch fail-closed.
- Keep the software executor as the deterministic reference; diff against it.

### Stage 4 — HLE tier-A fidelity
- Goal: `libKernel` (threads/sync/VM/fs), `libAudio`, `libNet` reach
  behaviorally-faithful coverage for the syscalls a homebrew title actually
  calls. Stubs remain only where the guest path is unreachable in tests.
- Deprecate the `/30 + 130` analysis backlog by tracking per-symbol
  `implemented | stub | clarified` in a machine-readable table, not prose.

### Stage 5 — Devkit plaintext payloads
- Goal: when/if a non-ciphertext (debug/devkit) Prospero payload is available
  under its own license, boot it through `prospero_self` + the full pipeline.
- Explicitly **not** planned: retail decryption, DRM bypass, or commercial game
  compatibility. Anyone seeking those is looking at the wrong project.

## Guardrails for every stage
- Each change lands with a dependency-free unit test that fails on the old
  behaviour and passes on the new (`make unit`).
- "Resolvable but returns OK" is never counted as "implemented"; that is the
  core honesty rule of this repo (`docs/HLE_COVERAGE.md`, `docs/SCOPE.md`).
- No firmware, keys, or proprietary files are ever added to the tree.
