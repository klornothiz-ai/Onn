# Design References

This project uses an original, deliberately narrow implementation. No third-party emulator source code was copied into this branch.

## Specifications

- AMD, *RDNA 2 Shader Instruction Set Architecture: Reference Guide* (document 70648): https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture
- Khronos, *SPIR-V Specification*, Unified 1.6 Revision 7: https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html
- Khronos, *SPIRV-Tools*: https://github.com/KhronosGroup/SPIRV-Tools (Apache-2.0; used as an optional validation oracle in CI)
- System V AMD64 ABI: https://gitlab.com/x86-psABIs/x86-64-ABI

## Open-source projects consulted only as high-level references

- Kyty: https://github.com/InoriRus/Kyty (MIT)
- KytyPS5: https://github.com/KytyPS5/KytyPS5 (GPL-2.0)
- Intel XED: https://github.com/intelxed/xed (Apache-2.0)
- Zydis: https://github.com/zyantific/zydis (MIT)
- UMR: https://gitlab.freedesktop.org/tomstdenis/umr (MIT)

Before reusing external source code, verify the source file's license and obtain an appropriate legal review. In particular, GPL-licensed code must not be copied into a differently licensed codebase without satisfying the GPL's terms.
