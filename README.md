# ProsperoLayer Research Core

A research-oriented PS4/PS5 emulation prototype. This branch replaces several unsafe or misleading proof-of-concept paths with small, testable, fail-closed cores.

> Status: this is **not** a complete PS5 emulator and makes no game compatibility or playability claim. It does not include games, firmware, keys, or proprietary system software.

## What changed in this branch

- CPU execution no longer copies guest bytes into host `RWX` memory or relies on process-wide signal handlers. A narrow x86-64 interpreter supports only audited, register-only instructions and an explicit syscall callback.
- Guest-memory conversion now rejects unmapped ranges, verifies permissions for whole copies, and never returns a raw host pointer for an arbitrary guest value.
- The ELF loader validates program-header bounds, loads through temporary guest write access, explicitly clears BSS, and applies final guest permissions afterwards.
- PM4 Type-3 parsing is separate from execution. A malformed packet anywhere in the stream prevents all backend side effects.
- The RDNA2 subset compiler rejects unsupported instructions rather than treating them as another operation. It emits a minimal SPIR-V 1.0 compute module with a valid entry point and function structure.

## Build and test

The dependency-free core tests are the supported verification target:

```bash
make unit
```

For cross-platform project generation, use CMake when available:

```bash
cmake -S . -B build/cmake -DPROSPERO_BUILD_TESTS=ON
cmake --build build/cmake
ctest --test-dir build/cmake --output-on-failure
```

On Windows, use the Visual Studio generator or MinGW. The runtime still requires legally obtained game data and platform dependencies; no firmware, keys, or proprietary files are included.

GPU status: Vulkan device and queue discovery are validated, and PM4 state is tracked without hard-coded viewport dimensions. Real guest-resource rendering (buffers, images, descriptor sets, graphics/compute pipelines, synchronization, and command submission) is not yet implemented; the reference framebuffer path is explicit and must not be treated as commercial-game compatibility.

This runs six focused suites:

- `cpu_interpreter_test`
- `jit_executor_test`
- `vmm_elf_loader_test`
- `pm4_decoder_test`
- `rdna2_spirv_recompiler_test`
- `game_folder_test`

A full prototype build now compiles the core sources, but linking remains blocked by incomplete HLE library wiring (audio, network/platform, video-decoder symbols) and optional FFmpeg-derived symbols. The dependency-free `make unit` target remains green; passing it is not evidence that the full prototype or a game runs.

## Scope

See `docs/SCOPE.md` for implementation limits and safety boundaries, and `docs/REFERENCES.md` for specifications and third-party reference projects.
