# Current Scope and Safety Boundary

This branch is a testable foundation for an emulator research project. It is not a complete PS5 emulator, does not support commercial game execution, and does not ship or require game files, system firmware, keys, or account credentials.

## Implemented, deliberately narrow components

- A fail-closed x86-64 interpreter for a small register-only instruction subset: `MOV`, `ADD`, immediate moves, `NOP`, `SYSCALL`, and `RET`.
- Checked guest-memory copies that never turn an unmapped guest address into a host pointer.
- ELF segment loading through temporary guest write permission followed by final guest protection, with BSS clearing and bounds checks.
- Bounds-safe Type-3 PM4 decoding with whole-stream validation before any backend action.
- A fail-closed RDNA2 subset compiler that emits a minimal SPIR-V 1.0 compute module for a small supported instruction set.

## Explicitly not implemented

- A general-purpose x86-64 JIT, relocation/linker, guest ABI, guest operating system, or production syscall surface.
- Complete PM4 packet semantics or hardware-accurate cache/MMIO/coherency behaviour; the implemented core now covers indirect-buffer traversal, guest WRITE_DATA/COPY_DATA/WAIT_REG_MEM and completion signalling, but remains a research model.
- General RDNA2 instruction coverage, full wavefront/barrier/image semantics, or universal GPU correctness. The current core includes buffer descriptors, SMEM buffer loads, MUBUF bounds checking, and a 64 KiB software LDS model for basic DS operations; unsupported instructions still fail closed on the Vulkan path.
- Compatibility or playability claims for any software.

## Test command

Run `make unit`. It builds and runs the dependency-free CPU interpreter, CPU execution-boundary, ELF/VMM, PM4, and SPIR-V unit suites. Full `make` remains optional and requires SDL2, Vulkan, FFmpeg, fmt, and nlohmann-json development packages.
